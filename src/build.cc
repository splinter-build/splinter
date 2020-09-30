// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "build.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <functional>
#include <algorithm>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#if defined(__SVR4) && defined(__sun)
#include <sys/termios.h>
#endif

#include "build_log.h"
#include "clparser.h"
#include "debug_flags.h"
#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "graph.h"
#include "state.h"
#include "subprocess.h"
#include "util.h"
#include "string_concat.h"

using namespace std;

namespace {

/// A CommandRunner that doesn't actually run the commands.
struct DryRunCommandRunner final : public CommandRunner {
  ~DryRunCommandRunner() = default;

  // Overridden from CommandRunner:
  bool CanRunMore() const override final;
  bool StartCommand(Edge* edge) override final;
  bool WaitForCommand(Result* result) override final;

 private:
  std::queue<Edge*> finished_;
};

bool DryRunCommandRunner::CanRunMore() const {
  return true;
}

bool DryRunCommandRunner::StartCommand(Edge* edge) {
  finished_.push(edge);
  return true;
}

bool DryRunCommandRunner::WaitForCommand(Result* result) {
   if (finished_.empty())
     return false;

   result->status = ExitSuccess;
   result->edge = finished_.front();
   finished_.pop();
   return true;
}

}  // namespace

BuildStatus::BuildStatus(const BuildConfig& config)
    : config_(config),
      start_time_millis_(GetTimeMillis()),
      current_rate_(config.parallelism) {
  // Don't do anything fancy in verbose mode.
  if (config_.verbosity != BuildConfig::NORMAL)
    printer_.set_smart_terminal(false);

  progress_status_format_ = getenv("NINJA_STATUS");
  if (!progress_status_format_)
    progress_status_format_ = "[%f/%t] ";
}

void BuildStatus::PlanHasTotalEdges(int total) {
  total_edges_ = total;
}

void BuildStatus::BuildEdgeStarted(const Edge* edge) {
  assert(running_edges_.find(edge) == running_edges_.end());
  int start_time = (int)(GetTimeMillis() - start_time_millis_);
  running_edges_.emplace(edge, start_time);
  ++started_edges_;

  if (edge->use_console() || printer_.is_smart_terminal())
    PrintStatus(edge, kEdgeStarted);

  if (edge->use_console())
    printer_.SetConsoleLocked(true);
}

void BuildStatus::BuildEdgeFinished(Edge* edge,
                                    bool success,
                                    const std::string& output,
                                    int* start_time,
                                    int* end_time) {
  int64_t now = GetTimeMillis();

  ++finished_edges_;

  RunningEdgeMap::iterator i = running_edges_.find(edge);
  *start_time = i->second;
  *end_time = (int)(now - start_time_millis_);
  running_edges_.erase(i);

  if (edge->use_console())
    printer_.SetConsoleLocked(false);

  if (config_.verbosity == BuildConfig::QUIET)
    return;

  if (!edge->use_console())
    PrintStatus(edge, kEdgeFinished);

  // Print the command that is spewing before printing its output.
  if (!success) {
    std::string outputs;
    for(const auto & item : edge->outputs_)
    {
        string_append(outputs, item->path().generic_string().c_str(), " ");
    }

    if (printer_.supports_color()) {
        printer_.PrintOnNewLine(string_concat("\x1B[31m" "FAILED: " "\x1B[0m", outputs, "\n"));
    } else {
        printer_.PrintOnNewLine(string_concat("FAILED: ", outputs, "\n"));
    }
    printer_.PrintOnNewLine(edge->EvaluateCommand() + "\n");
  }

  if (!output.empty()) {
    // ninja sets stdout and stderr of subprocesses to a pipe, to be able to
    // check if the output is empty. Some compilers, e.g. clang, check
    // isatty(stderr) to decide if they should print colored output.
    // To make it possible to use colored output with ninja, subprocesses should
    // be run with a flag that forces them to always print color escape codes.
    // To make sure these escape codes don't show up in a file if ninja's output
    // is piped to a file, ninja strips ansi escape codes again if it's not
    // writing to a |smart_terminal_|.
    // (Launching subprocesses in pseudo ttys doesn't work because there are
    // only a few hundred available on some systems, and ninja can launch
    // thousands of parallel compile commands.)
    std::string final_output;
    if (!printer_.supports_color())
      final_output = StripAnsiEscapeCodes(output);
    else
      final_output = output;

#ifdef _WIN32
    // Fix extra CR being added on Windows, writing out CR CR LF (#773)
    _setmode(_fileno(stdout), _O_BINARY);  // Begin Windows extra CR fix
#endif

    printer_.PrintOnNewLine(final_output);

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_TEXT);  // End Windows extra CR fix
#endif
  }
}

void BuildStatus::BuildLoadDyndeps() {
  // The DependencyScan calls EXPLAIN() to print lines explaining why
  // it considers a portion of the graph to be out of date.  Normally
  // this is done before the build starts, but our caller is about to
  // load a dyndep file during the build.  Doing so may generate more
  // explanation lines (via fprintf directly to stderr), but in an
  // interactive console the cursor is currently at the end of a status
  // line.  Start a new line so that the first explanation does not
  // append to the status line.  After the explanations are done a
  // new build status line will appear.
  if (g_explaining)
    printer_.PrintOnNewLine("");
}

void BuildStatus::BuildStarted() {
  overall_rate_.Restart();
  current_rate_.Restart();
}

void BuildStatus::BuildFinished() {
  printer_.SetConsoleLocked(false);
  printer_.PrintOnNewLine("");
}

std::string BuildStatus::FormatProgressStatus(
    const char* progress_status_format, EdgeStatus status) const {
  std::string out;
  char buf[32];
  int percent;
  for (const char* s = progress_status_format; *s != '\0'; ++s) {
    if (*s == '%') {
      ++s;
      switch (*s) {
      case '%':
        out.push_back('%');
        break;

        // Started edges.
      case 's':
        snprintf(buf, sizeof(buf), "%d", started_edges_);
        out += buf;
        break;

        // Total edges.
      case 't':
        snprintf(buf, sizeof(buf), "%d", total_edges_);
        out += buf;
        break;

        // Running edges.
      case 'r': {
        int running_edges = started_edges_ - finished_edges_;
        // count the edge that just finished as a running edge
        if (status == kEdgeFinished)
          running_edges++;
        snprintf(buf, sizeof(buf), "%d", running_edges);
        out += buf;
        break;
      }

        // Unstarted edges.
      case 'u':
        snprintf(buf, sizeof(buf), "%d", total_edges_ - started_edges_);
        out += buf;
        break;

        // Finished edges.
      case 'f':
        snprintf(buf, sizeof(buf), "%d", finished_edges_);
        out += buf;
        break;

        // Overall finished edges per second.
      case 'o':
        overall_rate_.UpdateRate(finished_edges_);
        SnprintfRate(overall_rate_.rate(), buf, "%.1f");
        out += buf;
        break;

        // Current rate, average over the last '-j' jobs.
      case 'c':
        current_rate_.UpdateRate(finished_edges_);
        SnprintfRate(current_rate_.rate(), buf, "%.1f");
        out += buf;
        break;

        // Percentage
      case 'p':
        percent = (100 * finished_edges_) / total_edges_;
        snprintf(buf, sizeof(buf), "%3i%%", percent);
        out += buf;
        break;

      case 'e': {
        double elapsed = overall_rate_.Elapsed();
        snprintf(buf, sizeof(buf), "%.3f", elapsed);
        out += buf;
        break;
      }

      default:
        Fatal("unknown placeholder '%%%c' in $NINJA_STATUS", *s);
        return "";
      }
    } else {
      out.push_back(*s);
    }
  }

  return out;
}

void BuildStatus::PrintStatus(const Edge* edge, EdgeStatus status) {
  if (config_.verbosity == BuildConfig::QUIET)
    return;

  bool force_full_command = config_.verbosity == BuildConfig::VERBOSE;

  std::string to_print = edge->GetBinding("description");
  if (to_print.empty() || force_full_command)
    to_print = edge->GetBinding("command");

  to_print = FormatProgressStatus(progress_status_format_, status) + to_print;

  printer_.Print(to_print,
                 force_full_command ? LinePrinter::FULL : LinePrinter::ELIDE);
}

Plan::Plan(Builder* builder)
  : builder_(builder)
  , command_edges_(0)
  , wanted_edges_(0)
{}

void Plan::Reset() {
  command_edges_ = 0;
  wanted_edges_ = 0;
  ready_.clear();
  want_.clear();
}

bool Plan::AddTarget(const Node* node, std::string* err)
{
  return AddSubTarget(node, nullptr, err, nullptr);
}

bool Plan::AddSubTarget(const Node*      node,
                        const Node*      dependent,
                        std::string*     err,
                        std::set<Edge*>* dyndep_walk)
{
  Edge* edge = node->in_edge();
  if( ! edge) // Leaf node.
  {
    if(node->dirty())
    {
      if(dependent)
      {
        *err = string_concat("'", node->path().generic_string().c_str(), "', needed by '", dependent->path().generic_string().c_str(), "', missing and no known rule to make it");
      }
      else
      {
        *err = string_concat("'", node->path().generic_string().c_str(), "'", " missing and no known rule to make it");
      }
    }
    return false;
  }

  if(edge->outputs_ready())
  {
    return false;  // Don't need to do anything.
  }

  // If an entry in want_ does not already exist for edge, create an entry which
  // maps to kWantNothing, indicating that we do not want to build this entry itself.
  auto const& [it, success] = want_.emplace(edge, kWantNothing);
  Want& want = it->second;

  if(dyndep_walk && (want == kWantToFinish))
  {
    return false;  // Don't need to do anything with already-scheduled edge.
  }

  // If we do need to build edge and we haven't already marked it as wanted,
  // mark it now.
  if(node->dirty() && (want == kWantNothing))
  {
    want = kWantToStart;
    EdgeWanted(edge);
    if( ! dyndep_walk && edge->AllInputsReady())
    {
      ScheduleWork(it);
    }
  }

  if(dyndep_walk)
  {
    dyndep_walk->insert(edge);
  }

  if( ! success)
  {
    return true;  // We've already processed the inputs.
  }

  for(auto const& item : edge->inputs_)
  {
    if(   ! AddSubTarget(item, node, err, dyndep_walk)
       && ! err->empty())
    {
        return false;
    }
  }

  return true;
}

void Plan::EdgeWanted(const Edge* edge) {
  ++wanted_edges_;
  if (!edge->is_phony())
  {
    ++command_edges_;
  }
}

Edge* Plan::FindWork() {
  if (ready_.empty())
    return nullptr;
  std::set<Edge*>::iterator e = ready_.begin();
  Edge* edge = *e;
  ready_.erase(e);
  return edge;
}

void Plan::ScheduleWork(std::map<Edge*, Want>::iterator want_e) {
  if (want_e->second == kWantToFinish) {
    // This edge has already been scheduled.  We can get here again if an edge
    // and one of its dependencies share an order-only input, or if a node
    // duplicates an out edge (see https://github.com/ninja-build/ninja/pull/519).
    // Avoid scheduling the work again.
    return;
  }
  assert(want_e->second == kWantToStart);
  want_e->second = kWantToFinish;

  Edge* edge = want_e->first;
  if(Pool* pool = edge->pool();
     pool->ShouldDelayEdge())
  {
    pool->DelayEdge(edge);
    pool->RetrieveReadyEdges(&ready_);
  }
  else
  {
    pool->EdgeScheduled(*edge);
    ready_.insert(edge);
  }
}

bool Plan::EdgeFinished(Edge* edge, EdgeResult result, std::string* err) {
  std::map<Edge*, Want>::iterator e = want_.find(edge);
  assert(e != want_.end());
  bool directly_wanted = e->second != kWantNothing;

  // See if this job frees up any delayed jobs.
  if (directly_wanted)
    edge->pool()->EdgeFinished(*edge);
  edge->pool()->RetrieveReadyEdges(&ready_);

  // The rest of this function only applies to successful commands.
  if (result != kEdgeSucceeded)
    return true;

  if (directly_wanted)
    --wanted_edges_;
  want_.erase(e);
  edge->outputs_ready_ = true;

  // Check off any nodes we were waiting for with this edge.
  for(const auto & item : edge->outputs_)
  {
    if(!NodeFinished(item, err))
    {
      return false;
    }
  }
  return true;
}

bool Plan::NodeFinished(Node* node, std::string* err) {
  // If this node provides dyndep info, load it now.
  if (node->dyndep_pending()) {
    assert(builder_ && "dyndep requires Plan to have a Builder");
    // Load the now-clean dyndep file.  This will also update the
    // build plan and schedule any new work that is ready.
    return builder_->LoadDyndeps(node, err);
  }

  // See if we we want any edges from this node.
  for(const auto & item : node->out_edges())
  {
    if(auto const& want_e = want_.find(item);
       want_e != want_.end())
    {
      // See if the edge is now ready.
      if( ! EdgeMaybeReady(want_e, err))
      {
        return false;
      }
    }
  }
  return true;
}

bool Plan::EdgeMaybeReady(std::map<Edge*, Want>::iterator want_e, std::string* err) {
  if(Edge* edge = want_e->first;
     edge->AllInputsReady())
  {
    if(want_e->second != kWantNothing)
    {
      ScheduleWork(want_e);
    }
    // We do not need to build this edge, but we might need to build one of
    // its dependents.
    else if( ! EdgeFinished(edge, kEdgeSucceeded, err))
    {
        return false;
    }
  }
  return true;
}

bool Plan::CleanNode(DependencyScan* scan, Node* node, std::string* err) {
  node->set_dirty(false);

  for (const auto & item : node->out_edges())
  {
    // Don't process edges that we don't actually want.
    std::map<Edge*, Want>::iterator want_e = want_.find(item);
    if (want_e == want_.end() || want_e->second == kWantNothing)
      continue;

    // Don't attempt to clean an edge if it failed to load deps.
    if (item->deps_missing_)
      continue;

    // If all non-order-only inputs for this edge are now clean,
    // we might have changed the dirty state of the outputs.
    auto const& begin = item->inputs_.begin();
    auto const& end = item->inputs_.end() - item->order_only_deps_;
    if (std::find_if(begin, end, std::mem_fn(&Node::dirty)) == end) {
      // Recompute most_recent_input.
      Node* most_recent_input = nullptr;
      for (std::vector<Node*>::iterator i = begin; i != end; ++i) {
        if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime())
          most_recent_input = *i;
      }

      // Now, this edge is dirty if any of the outputs are dirty.
      // If the edge isn't dirty, clean the outputs and mark the edge as not
      // wanted.
      bool outputs_dirty = false;
      if (!scan->RecomputeOutputsDirty(item, most_recent_input,
                                       &outputs_dirty, err)) {
        return false;
      }
      if (!outputs_dirty) {
        for (const auto & inner : item->outputs_)
        {
          if (!CleanNode(scan, inner, err))
            return false;
        }

        want_e->second = kWantNothing;
        --wanted_edges_;
        if (!item->is_phony())
          --command_edges_;
      }
    }
  }
  return true;
}

bool Plan::DyndepsLoaded(DependencyScan* scan, const Node* node,
                         const DyndepFile& ddf, std::string* err) {
  // Recompute the dirty state of all our direct and indirect dependents now
  // that our dyndep information has been loaded.
  if (!RefreshDyndepDependents(scan, node, err))
    return false;

  // We loaded dyndep information for those out_edges of the dyndep node that
  // specify the node in a dyndep binding, but they may not be in the plan.
  // Starting with those already in the plan, walk newly-reachable portion
  // of the graph through the dyndep-discovered dependencies.

  // Find edges in the the build plan for which we have new dyndep info.
  std::vector<DyndepFile::const_iterator> dyndep_roots;
  for (DyndepFile::const_iterator oe = ddf.begin(); oe != ddf.end(); ++oe) {
    Edge* edge = oe->first;

    // If the edge outputs are ready we do not need to consider it here.
    if (edge->outputs_ready())
      continue;

    // If the edge has not been encountered before then nothing already in the
    // plan depends on it so we do not need to consider the edge yet either.
    if(want_.end() == want_.find(edge))
    {
      continue;
    }

    // This edge is already in the plan so queue it for the walk.
    dyndep_roots.push_back(oe);
  }

  // Walk dyndep-discovered portion of the graph to add it to the build plan.
  std::set<Edge*> dyndep_walk;
  for (auto const& oe : dyndep_roots) {
    for (auto const& item : oe->second.implicit_inputs_) {
      if (!AddSubTarget(item, oe->first->outputs_[0], err, &dyndep_walk) &&
          !err->empty())
        return false;
    }
  }

  // Add out edges from this node that are in the plan (just as
  // Plan::NodeFinished would have without taking the dyndep code path).
  for(auto const& item : node->out_edges())
  {
    if(auto const& want_e = want_.find(item); want_e != want_.end())
    {
      dyndep_walk.insert(want_e->first);
    }
  }

  // See if any encountered edges are now ready.
  for (auto const& item : dyndep_walk)
  {
    if(auto const& want_e = want_.find(item);
       (want_e != want_.end()) && ! EdgeMaybeReady(want_e, err))
    {
      return false;
    }
  }

  return true;
}

bool Plan::RefreshDyndepDependents(DependencyScan* scan, const Node* node,
                                   std::string* err) {
  // Collect the transitive closure of dependents and mark their edges
  // as not yet visited by RecomputeDirty.
  std::set<Node*> dependents;
  UnmarkDependents(node, &dependents);

  // Update the dirty state of all dependents and check if their edges
  // have become wanted.
  for (auto const& n : dependents) {
    // Check if this dependent node is now dirty.  Also checks for new cycles.
    if (!scan->RecomputeDirty(n, err))
      return false;
    if (!n->dirty())
      continue;

    // This edge was encountered before.  However, we may not have wanted to
    // build it if the outputs were not known to be dirty.  With dyndep
    // information an output is now known to be dirty, so we want the edge.
    Edge* edge = n->in_edge();
    assert(edge && !edge->outputs_ready());
    std::map<Edge*, Want>::iterator want_e = want_.find(edge);
    assert(want_e != want_.end());
    if (want_e->second == kWantNothing) {
      want_e->second = kWantToStart;
      EdgeWanted(edge);
    }
  }
  return true;
}

void Plan::UnmarkDependents(const Node* node, std::set<Node*>* dependents) {
  for(auto const& edge : node->out_edges())
  {
    if(want_.end() == want_.find(edge))
    {
      continue;
    }

    if(edge->mark_ != Edge::VisitNone)
    {
      edge->mark_ = Edge::VisitNone;

      for(auto const& item : edge->outputs_)
      {
        if(auto const& [it, success] = dependents->insert(item); success)
        {
          UnmarkDependents(item, dependents);
        }
      }
    }
  }
}

void Plan::Dump() const {
  printf("pending: %d\n", (int)want_.size());
  for (auto const& [edge, want] : want_) {
    if (want != kWantNothing)
      printf("want ");
    edge->Dump();
  }
  printf("ready: %d\n", (int)ready_.size());
}

struct RealCommandRunner final : public CommandRunner {
  explicit RealCommandRunner(const BuildConfig& config) : config_(config) {}
  ~RealCommandRunner() = default;
  bool CanRunMore() const override final;
  bool StartCommand(Edge* edge) override final;
  bool WaitForCommand(Result* result) override final;
  std::vector<Edge*> GetActiveEdges() override final;
  void Abort() override final;

  const BuildConfig& config_;
  SubprocessSet subprocs_;
  std::map<const Subprocess*, Edge*> subproc_to_edge_;
};

std::vector<Edge*> RealCommandRunner::GetActiveEdges() {
  std::vector<Edge*> edges;
  for (auto const& [subproc, edge] : subproc_to_edge_)
  {
    edges.push_back(edge);
  }
  return edges;
}

void RealCommandRunner::Abort() {
  subprocs_.Clear();
}

bool RealCommandRunner::CanRunMore() const {
  std::size_t subproc_number =
      subprocs_.running_.size() + subprocs_.finished_.size();
  return (int)subproc_number < config_.parallelism
    && ((subprocs_.running_.empty() || config_.max_load_average <= 0.0f)
        || GetLoadAverage() < config_.max_load_average);
}

bool RealCommandRunner::StartCommand(Edge* edge) {
  std::string command = edge->EvaluateCommand();
  Subprocess* subproc = subprocs_.Add(command, edge->use_console());
  if (!subproc)
    return false;
  subproc_to_edge_.emplace(subproc, edge);

  return true;
}

bool RealCommandRunner::WaitForCommand(Result* result) {
  Subprocess* subproc;
  while ((subproc = subprocs_.NextFinished()) == nullptr) {
    bool interrupted = subprocs_.DoWork();
    if (interrupted)
      return false;
  }

  result->status = subproc->Finish();
  result->output = subproc->GetOutput();

  std::map<const Subprocess*, Edge*>::iterator e = subproc_to_edge_.find(subproc);
  result->edge = e->second;
  subproc_to_edge_.erase(e);

  delete subproc;
  return true;
}

Builder::Builder(State* state, const BuildConfig& config,
                 BuildLog* build_log, DepsLog* deps_log,
                 DiskInterface* disk_interface)
    : state_(state), config_(config),
      plan_(this), disk_interface_(disk_interface),
      scan_(state, build_log, deps_log, disk_interface,
            &config_.depfile_parser_options) {
  status_ = new BuildStatus(config);
}

Builder::~Builder() {
  Cleanup();
}

void Builder::Cleanup() {
  if (command_runner_.get()) {
    std::vector<Edge*> active_edges = command_runner_->GetActiveEdges();
    command_runner_->Abort();

    for (const auto & item : active_edges)
    {
      std::filesystem::path const& depfile = item->GetUnescapedDepfile();
      for (const auto & inner : item->outputs_)
      {
        // Only delete this output if it was actually modified.  This is
        // important for things like the generator where we don't want to
        // delete the manifest file if we can avoid it.  But if the rule
        // uses a depfile, always delete.  (Consider the case where we
        // need to rebuild an output because of a modified header file
        // mentioned in a depfile, and the command touches its depfile
        // but is interrupted before it touches its output file.)
        std::string err;
        TimeStamp new_mtime = disk_interface_->Stat(inner->path(), &err);
        if (new_mtime == TimeStamp::max())  // Log and ignore Stat() errors.
          Error("%s", err.c_str());
        if (!depfile.empty() || inner->mtime() != new_mtime)
          disk_interface_->RemoveFile(inner->path());
      }
      if (!depfile.empty())
        disk_interface_->RemoveFile(depfile);
    }
  }
}

Node* Builder::AddTarget(std::filesystem::path const& name, std::string* err) {
  if(Node* node = state_->LookupNode(name); node)
  {
    if(AddTarget(node, err))
    {
      return node;
    }
    else
    {
      return nullptr;
    }
  }
  else
  {
    *err = string_concat("unknown target: '", name.generic_string().c_str(), "'");
    return nullptr;
  }
}

bool Builder::AddTarget(Node* node, std::string* err)
{
  if( ! scan_.RecomputeDirty(node, err))
  {
    return false;
  }

  if(Edge* in_edge = node->in_edge(); in_edge)
  {
    if(in_edge->outputs_ready())
    {
      return true;  // Nothing to do.
    }
  }

  return plan_.AddTarget(node, err);
}

bool Builder::AlreadyUpToDate() const {
  return !plan_.more_to_do();
}

bool Builder::Build(std::string* err) {
  assert(!AlreadyUpToDate());

  status_->PlanHasTotalEdges(plan_.command_edge_count());
  int pending_commands = 0;
  int failures_allowed = config_.failures_allowed;

  // Set up the command runner if we haven't done so already.
  if (!command_runner_.get()) {
    if (config_.dry_run)
      command_runner_.reset(new DryRunCommandRunner);
    else
      command_runner_.reset(new RealCommandRunner(config_));
  }

  // We are about to start the build process.
  status_->BuildStarted();

  // This main loop runs the entire build process.
  // It is structured like this:
  // First, we attempt to start as many commands as allowed by the
  // command runner.
  // Second, we attempt to wait for / reap the next finished command.
  while (plan_.more_to_do()) {
    // See if we can start any more commands.
    if (failures_allowed && command_runner_->CanRunMore()) {
      if (Edge* edge = plan_.FindWork(); edge) {
        if (!StartEdge(edge, err)) {
          Cleanup();
          status_->BuildFinished();
          return false;
        }

        if (edge->is_phony()) {
          if (!plan_.EdgeFinished(edge, Plan::kEdgeSucceeded, err)) {
            Cleanup();
            status_->BuildFinished();
            return false;
          }
        } else {
          ++pending_commands;
        }

        // We made some progress; go back to the main loop.
        continue;
      }
    }

    // See if we can reap any finished commands.
    if (pending_commands) {
      CommandRunner::Result result;
      if (!command_runner_->WaitForCommand(&result) ||
          result.status == ExitInterrupted) {
        Cleanup();
        status_->BuildFinished();
        *err = "interrupted by user";
        return false;
      }

      --pending_commands;
      if (!FinishCommand(&result, err)) {
        Cleanup();
        status_->BuildFinished();
        return false;
      }

      if (!result.success()) {
        if (failures_allowed)
          failures_allowed--;
      }

      // We made some progress; start the main loop over.
      continue;
    }

    // If we get here, we cannot make any more progress.
    status_->BuildFinished();
    if (failures_allowed == 0) {
      if (config_.failures_allowed > 1)
        *err = "subcommands failed";
      else
        *err = "subcommand failed";
    } else if (failures_allowed < config_.failures_allowed)
      *err = "cannot make progress due to previous errors";
    else
      *err = "stuck [this is a bug]";

    return false;
  }

  status_->BuildFinished();
  return true;
}

bool Builder::StartEdge(Edge* edge, std::string* err) {
  METRIC_RECORD("StartEdge");
  if (edge->is_phony())
    return true;

  status_->BuildEdgeStarted(edge);

  // Create directories necessary for outputs.
  // XXX: this will block; do we care?
  for (const auto & item : edge->outputs_)
  {
    if (!disk_interface_->MakeDirs(item->path()))
      return false;
  }

  // Create response file, if needed
  // XXX: this may also block; do we care?
  if(std::filesystem::path const& rspfile = edge->GetUnescapedRspfile(); ! rspfile.empty())
  {
    if(std::string const& content = edge->GetBinding("rspfile_content");
       ! disk_interface_->WriteFile(rspfile, content))
    {
      return false;
    }
  }

  // start command computing and run it
  if (!command_runner_->StartCommand(edge)) {
    *err = string_concat("command '", edge->EvaluateCommand(), "' failed.");
    return false;
  }

  return true;
}

bool Builder::FinishCommand(CommandRunner::Result* result, std::string* err) {
  METRIC_RECORD("FinishCommand");

  Edge* edge = result->edge;

  // First try to extract dependencies from the result, if any.
  // This must happen first as it filters the command output (we want
  // to filter /showIncludes output, even on compile failure) and
  // extraction itself can fail, which makes the command fail from a
  // build perspective.
  std::vector<Node*> deps_nodes;
  std::string deps_type = edge->GetBinding("deps");
  const std::string deps_prefix = edge->GetBinding("msvc_deps_prefix");
  if (!deps_type.empty()) {
    std::string extract_err;
    if (!ExtractDeps(result, deps_type, deps_prefix, &deps_nodes,
                     &extract_err) &&
        result->success()) {
      if (!result->output.empty())
        result->output.append("\n");
      result->output.append(extract_err);
      result->status = ExitFailure;
    }
  }

  int start_time, end_time;
  status_->BuildEdgeFinished(edge, result->success(), result->output,
                             &start_time, &end_time);

  // The rest of this function only applies to successful commands.
  if (!result->success()) {
    return plan_.EdgeFinished(edge, Plan::kEdgeFailed, err);
  }

  // Restat the edge outputs
  TimeStamp output_mtime = TimeStamp::min();
  bool restat = edge->GetBindingBool("restat");
  if (!config_.dry_run) {
    bool node_cleaned = false;

    for (const auto & item : edge->outputs_) {
      TimeStamp new_mtime = disk_interface_->Stat(item->path(), err);
      if (new_mtime == TimeStamp::max())
        return false;
      if (new_mtime > output_mtime)
        output_mtime = new_mtime;
      if (item->mtime() == new_mtime && restat) {
        // The rule command did not change the output.  Propagate the clean
        // state through the build graph.
        // Note that this also applies to nonexistent outputs (mtime == 0).
        if (!plan_.CleanNode(&scan_, item, err))
          return false;
        node_cleaned = true;
      }
    }

    if (node_cleaned) {
      TimeStamp restat_mtime = TimeStamp::min();
      // If any output was cleaned, find the most recent mtime of any
      // (existing) non-order-only input or the depfile.
      for (std::vector<Node*>::iterator i = edge->inputs_.begin();
           i != edge->inputs_.end() - edge->order_only_deps_; ++i) {
        TimeStamp input_mtime = disk_interface_->Stat((*i)->path(), err);
        if (input_mtime == TimeStamp::max())
          return false;
        if (input_mtime > restat_mtime)
          restat_mtime = input_mtime;
      }

      std::filesystem::path const& depfile = edge->GetUnescapedDepfile();
      if (restat_mtime != TimeStamp::min() && deps_type.empty() && !depfile.empty()) {
        TimeStamp depfile_mtime = disk_interface_->Stat(depfile, err);
        if (depfile_mtime == TimeStamp::max())
          return false;
        if (depfile_mtime > restat_mtime)
          restat_mtime = depfile_mtime;
      }

      // The total number of edges in the plan may have changed as a result
      // of a restat.
      status_->PlanHasTotalEdges(plan_.command_edge_count());

      output_mtime = restat_mtime;
    }
  }

  if (!plan_.EdgeFinished(edge, Plan::kEdgeSucceeded, err))
    return false;

  // Delete any left over response file.
  if(std::filesystem::path const& rspfile = edge->GetUnescapedRspfile();
     !rspfile.empty() && !g_keep_rsp)
  {
    disk_interface_->RemoveFile(rspfile);
  }

  if (scan_.build_log()) {
    if (!scan_.build_log()->RecordCommand(edge, start_time, end_time,
                                          output_mtime)) {
      *err = string_concat("Error writing to build log: ", strerror(errno));
      return false;
    }
  }

  if (!deps_type.empty() && !config_.dry_run) {
    assert(!edge->outputs_.empty() && "should have been rejected by parser");
    for(auto const& output : edge->outputs_) {
      TimeStamp deps_mtime = disk_interface_->Stat(output->path(), err);
      if (deps_mtime == TimeStamp::max())
      {
        return false;
      }

      if( ! scan_.deps_log()->RecordDeps(output, deps_mtime, deps_nodes))
      {
        *err = string_concat("Error writing to deps log: ", strerror(errno));
        return false;
      }
    }
  }
  return true;
}

bool Builder::ExtractDeps(CommandRunner::Result* result,
                          const std::string& deps_type,
                          const std::string& deps_prefix,
                          std::vector<Node*>* deps_nodes,
                          std::string* err) {
  if (deps_type == "msvc") {
    CLParser parser;
    std::string output;
    if (!parser.Parse(result->output, deps_prefix, &output, err))
      return false;
    result->output = output;
    for (const auto & item : parser.includes_) {
      // ~0 is assuming that with MSVC-parsed headers, it's ok to always make
      // all backslashes (as some of the slashes will certainly be backslashes
      // anyway). This could be fixed if necessary with some additional
      // complexity in IncludesNormalize::Relativize.
      deps_nodes->push_back(state_->GetNode(item));
    }
  } else if (deps_type == "gcc") {
    std::filesystem::path const& depfile = result->edge->GetUnescapedDepfile();
    if (depfile.empty()) {
      *err = std::string("edge with deps=gcc but no depfile makes no sense");
      return false;
    }

    // Read depfile content.  Treat a missing depfile as empty.
    std::string content;
    switch (disk_interface_->ReadFile(depfile, &content, err)) {
    case DiskInterface::Okay:
      break;
    case DiskInterface::NotFound:
      err->clear();
      break;
    case DiskInterface::OtherError:
      return false;
    }
    if (content.empty())
      return true;

    DepfileParser deps(config_.depfile_parser_options);
    if (!deps.Parse(&content, err))
      return false;

    // XXX check depfile matches expected output.
    deps_nodes->reserve(deps.ins_.size());
    for (auto & item : deps.ins_)
    {
      deps_nodes->push_back(state_->GetNode(item));
    }

    if (!g_keep_depfile)
    {
      if (disk_interface_->RemoveFile(depfile) < 0)
      {
        *err = string_concat("deleting depfile: ", strerror(errno), "\n");
        return false;
      }
    }
  }
  else
  {
    Fatal("unknown deps type '%s'", deps_type.c_str());
  }

  return true;
}

bool Builder::LoadDyndeps(Node* node, std::string* err) {
  status_->BuildLoadDyndeps();

  // Load the dyndep information provided by this node.
  DyndepFile ddf;
  if (!scan_.LoadDyndeps(node, &ddf, err))
    return false;

  // Update the build plan to account for dyndep modifications to the graph.
  if (!plan_.DyndepsLoaded(&scan_, node, ddf, err))
    return false;

  // New command edges may have been added to the plan.
  status_->PlanHasTotalEdges(plan_.command_edge_count());

  return true;
}
