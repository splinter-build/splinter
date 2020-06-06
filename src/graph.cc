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

#include "graph.h"

#include <algorithm>
#include <assert.h>
#include <stdio.h>

#include "build_log.h"
#include "debug_flags.h"
#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "manifest_parser.h"
#include "metrics.h"
#include "state.h"
#include "util.h"

bool Node::Stat(DiskInterface* disk_interface, std::string* err) {
  return (mtime_ = disk_interface->Stat(path_, err)) != -1;
}

bool DependencyScan::RecomputeDirty(Node* node, std::string* err) {
  std::vector<Node*> stack;
  return RecomputeDirty(node, &stack, err);
}

bool DependencyScan::RecomputeDirty(Node* node, std::vector<Node*>* stack,
                                    std::string* err) {
  Edge* edge = node->in_edge();
  if (!edge) {
    // If we already visited this leaf node then we are done.
    if (node->status_known())
      return true;
    // This node has no in-edge; it is dirty if it is missing.
    if (!node->StatIfNecessary(disk_interface_, err))
      return false;
    if (!node->exists())
      EXPLAIN("%s has no in-edge and is missing", node->path().c_str());
    node->set_dirty(!node->exists());
    return true;
  }

  // If we already finished this edge then we are done.
  if (edge->mark_ == Edge::VisitDone)
    return true;

  // If we encountered this edge earlier in the call stack we have a cycle.
  if (!VerifyDAG(node, stack, err))
    return false;

  // Mark the edge temporarily while in the call stack.
  edge->mark_ = Edge::VisitInStack;
  stack->push_back(node);

  bool dirty = false;
  edge->outputs_ready_ = true;
  edge->deps_missing_ = false;

  if (!edge->deps_loaded_) {
    // This is our first encounter with this edge.
    // If there is a pending dyndep file, visit it now:
    // * If the dyndep file is ready then load it now to get any
    //   additional inputs and outputs for this and other edges.
    //   Once the dyndep file is loaded it will no longer be pending
    //   if any other edges encounter it, but they will already have
    //   been updated.
    // * If the dyndep file is not ready then since is known to be an
    //   input to this edge, the edge will not be considered ready below.
    //   Later during the build the dyndep file will become ready and be
    //   loaded to update this edge before it can possibly be scheduled.
    if (edge->dyndep_ && edge->dyndep_->dyndep_pending()) {
      if (!RecomputeDirty(edge->dyndep_, stack, err))
        return false;

      if (!edge->dyndep_->in_edge() ||
          edge->dyndep_->in_edge()->outputs_ready()) {
        // The dyndep file is ready, so load it now.
        if (!LoadDyndeps(edge->dyndep_, err))
          return false;
      }
    }
  }

  // Load output mtimes so we can compare them to the most recent input below.
  for (const auto & item : edge->outputs_)
  {
    if (!item->StatIfNecessary(disk_interface_, err))
      return false;
  }

  if (!edge->deps_loaded_) {
    // This is our first encounter with this edge.  Load discovered deps.
    edge->deps_loaded_ = true;
    if (!dep_loader_.LoadDeps(edge, err)) {
      if (!err->empty())
        return false;
      // Failed to load dependency info: rebuild to regenerate it.
      // LoadDeps() did EXPLAIN() already, no need to do it here.
      dirty = edge->deps_missing_ = true;
    }
  }

  // Visit all inputs; we're dirty if any of the inputs are dirty.
  Node* most_recent_input = nullptr;
  for (std::vector<Node*>::iterator i = edge->inputs_.begin();
       i != edge->inputs_.end(); ++i) {
    // Visit this input.
    if (!RecomputeDirty(*i, stack, err))
      return false;

    // If an input is not ready, neither are our outputs.
    if (Edge* in_edge = (*i)->in_edge()) {
      if (!in_edge->outputs_ready_)
        edge->outputs_ready_ = false;
    }

    if (!edge->is_order_only(i - edge->inputs_.begin())) {
      // If a regular input is dirty (or missing), we're dirty.
      // Otherwise consider mtime.
      if ((*i)->dirty()) {
        EXPLAIN("%s is dirty", (*i)->path().c_str());
        dirty = true;
      } else {
        if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime()) {
          most_recent_input = *i;
        }
      }
    }
  }

  // We may also be dirty due to output state: missing outputs, out of
  // date outputs, etc.  Visit all outputs and determine whether they're dirty.
  if (!dirty)
    if (!RecomputeOutputsDirty(edge, most_recent_input, &dirty, err))
      return false;

  // Finally, visit each output and update their dirty state if necessary.
  for (const auto & item : edge->outputs_)
  {
    if (dirty)
      item->MarkDirty();
  }

  // If an edge is dirty, its outputs are normally not ready.  (It's
  // possible to be clean but still not be ready in the presence of
  // order-only inputs.)
  // But phony edges with no inputs have nothing to do, so are always
  // ready.
  if (dirty && !(edge->is_phony() && edge->inputs_.empty()))
    edge->outputs_ready_ = false;

  // Mark the edge as finished during this walk now that it will no longer
  // be in the call stack.
  edge->mark_ = Edge::VisitDone;
  assert(stack->back() == node);
  stack->pop_back();

  return true;
}

bool DependencyScan::VerifyDAG(Node* node, std::vector<Node*>* stack, std::string* err) {
  Edge* edge = node->in_edge();
  assert(edge != nullptr);

  // If we have no temporary mark on the edge then we do not yet have a cycle.
  if (edge->mark_ != Edge::VisitInStack)
    return true;

  // We have this edge earlier in the call stack.  Find it.
  std::vector<Node*>::iterator start = stack->begin();
  while (start != stack->end() && (*start)->in_edge() != edge)
    ++start;
  assert(start != stack->end());

  // Make the cycle clear by reporting its start as the node at its end
  // instead of some other output of the starting edge.  For example,
  // running 'ninja b' on
  //   build a b: cat c
  //   build c: cat a
  // should report a -> c -> a instead of b -> c -> a.
  *start = node;

  // Construct the error message rejecting the cycle.
  *err = "dependency cycle: ";
  for (std::vector<Node*>::const_iterator i = start; i != stack->end(); ++i)
  {
    err->append((*i)->path());
    err->append(" -> ");
  }
  err->append((*start)->path());

  if ((start + 1) == stack->end() && edge->maybe_phonycycle_diagnostic()) {
    // The manifest parser would have filtered out the self-referencing
    // input if it were not configured to allow the error.
    err->append(" [-w phonycycle=err]");
  }

  return false;
}

bool DependencyScan::RecomputeOutputsDirty(Edge* edge, Node* most_recent_input,
                                           bool* outputs_dirty, std::string* err) {
  std::string command = edge->EvaluateCommand(/*incl_rsp_file=*/true);
  for (const auto & item : edge->outputs_)
  {
    if (RecomputeOutputDirty(edge, most_recent_input, command, item))
    {
      *outputs_dirty = true;
      return true;
    }
  }
  return true;
}

bool DependencyScan::RecomputeOutputDirty(const Edge* edge,
                                          const Node* most_recent_input,
                                          const std::string& command,
                                          Node* output) {
  if (edge->is_phony()) {
    // Phony edges don't write any output.  Outputs are only dirty if
    // there are no inputs and we're missing the output.
    if (edge->inputs_.empty() && !output->exists()) {
      EXPLAIN("output %s of phony edge with no inputs doesn't exist",
              output->path().c_str());
      return true;
    }
    return false;
  }

  BuildLog::LogEntry* entry = 0;

  // Dirty if we're missing the output.
  if (!output->exists()) {
    EXPLAIN("output %s doesn't exist", output->path().c_str());
    return true;
  }

  // Dirty if the output is older than the input.
  if (most_recent_input && output->mtime() < most_recent_input->mtime()) {
    TimeStamp output_mtime = output->mtime();

    // If this is a restat rule, we may have cleaned the output with a restat
    // rule in a previous run and stored the most recent input mtime in the
    // build log.  Use that mtime instead, so that the file will only be
    // considered dirty if an input was modified since the previous run.
    bool used_restat = false;
    if (edge->GetBindingBool("restat") && build_log() &&
        (entry = build_log()->LookupByOutput(output->path()))) {
      output_mtime = entry->mtime;
      used_restat = true;
    }

    if (output_mtime < most_recent_input->mtime()) {
      EXPLAIN("%soutput %s older than most recent input %s "
              "(%" PRId64 " vs %" PRId64 ")",
              used_restat ? "restat of " : "", output->path().c_str(),
              most_recent_input->path().c_str(),
              output_mtime, most_recent_input->mtime());
      return true;
    }
  }

  if (build_log()) {
    bool generator = edge->GetBindingBool("generator");
    if (entry || (entry = build_log()->LookupByOutput(output->path()))) {
      if (!generator &&
          BuildLog::LogEntry::HashCommand(command) != entry->command_hash) {
        // May also be dirty due to the command changing since the last build.
        // But if this is a generator rule, the command changing does not make us
        // dirty.
        EXPLAIN("command line changed for %s", output->path().c_str());
        return true;
      }
      if (most_recent_input && entry->mtime < most_recent_input->mtime()) {
        // May also be dirty due to the mtime in the log being older than the
        // mtime of the most recent input.  This can occur even when the mtime
        // on disk is newer if a previous run wrote to the output file but
        // exited with an error or was interrupted.
        EXPLAIN("recorded mtime of %s older than most recent input %s (%" PRId64 " vs %" PRId64 ")",
                output->path().c_str(), most_recent_input->path().c_str(),
                entry->mtime, most_recent_input->mtime());
        return true;
      }
    }
    if (!entry && !generator) {
      EXPLAIN("command line not found in log for %s", output->path().c_str());
      return true;
    }
  }

  return false;
}

bool DependencyScan::LoadDyndeps(Node* node, std::string* err) const {
  return dyndep_loader_.LoadDyndeps(node, err);
}

bool DependencyScan::LoadDyndeps(Node* node, DyndepFile* ddf,
                                 std::string* err) const {
  return dyndep_loader_.LoadDyndeps(node, ddf, err);
}

bool Edge::AllInputsReady() const {
  for (const auto & item : inputs_)
  {
    if (item->in_edge() && !item->in_edge()->outputs_ready())
      return false;
  }
  return true;
}

/// An Env for an Edge, providing $in and $out.
struct EdgeEnv final : public Env {
  enum EscapeKind { kShellEscape, kDoNotEscape };

  EdgeEnv(const Edge* const edge, const EscapeKind escape)
      : edge_(edge), escape_in_out_(escape), recursive_(false) {}
  std::string LookupVariable(const std::string& var) override final;

  /// Given a span of Nodes, construct a list of paths suitable for a command
  /// line.
  std::string MakePathList(const Node* const* span, size_t size, char sep) const;

 private:
  std::vector<std::string> lookups_;
  const Edge* const edge_;
  EscapeKind escape_in_out_;
  bool recursive_;
};

std::string EdgeEnv::LookupVariable(const std::string& var) {
  if (var == "in" || var == "in_newline") {
    int explicit_deps_count = edge_->inputs_.size() - edge_->implicit_deps_ -
      edge_->order_only_deps_;
#if __cplusplus >= 201103L
    return MakePathList(edge_->inputs_.data(), explicit_deps_count,
#else
    return MakePathList(&edge_->inputs_[0], explicit_deps_count,
#endif
                        var == "in" ? ' ' : '\n');
  } else if (var == "out") {
    int explicit_outs_count = edge_->outputs_.size() - edge_->implicit_outs_;
    return MakePathList(&edge_->outputs_[0], explicit_outs_count, ' ');
  }

  if (recursive_) {
    std::vector<std::string>::const_iterator it;
    if ((it = std::find(lookups_.begin(), lookups_.end(), var)) != lookups_.end()) {
      std::string cycle;
      for (; it != lookups_.end(); ++it)
        cycle.append(*it + " -> ");
      cycle.append(var);
      Fatal(("cycle in rule variables: " + cycle).c_str());
    }
  }

  // See notes on BindingEnv::LookupWithFallback.
  const EvalString* eval = edge_->rule_->GetBinding(var);
  if (recursive_ && eval)
    lookups_.push_back(var);

  // In practice, variables defined on rules never use another rule variable.
  // For performance, only start checking for cycles after the first lookup.
  recursive_ = true;
  return edge_->env_->LookupWithFallback(var, eval, this);
}

std::string EdgeEnv::MakePathList(const Node* const* const span,
                                  const size_t size, const char sep) const {
  std::string result;
  for (const Node* const* i = span; i != span + size; ++i) {
    if (!result.empty())
      result.push_back(sep);
    const std::string& path = (*i)->PathDecanonicalized();
    if (escape_in_out_ == kShellEscape) {
#ifdef _WIN32
      GetWin32EscapedString(path, &result);
#else
      GetShellEscapedString(path, &result);
#endif
    } else {
      result.append(path);
    }
  }
  return result;
}

std::string Edge::EvaluateCommand(const bool incl_rsp_file) const {
  std::string command = GetBinding("command");
  if (incl_rsp_file) {
    std::string rspfile_content = GetBinding("rspfile_content");
    if (!rspfile_content.empty())
      command += ";rspfile=" + rspfile_content;
  }
  return command;
}

std::string Edge::GetBinding(const std::string& key) const {
  EdgeEnv env(this, EdgeEnv::kShellEscape);
  return env.LookupVariable(key);
}

bool Edge::GetBindingBool(const std::string& key) const {
  return !GetBinding(key).empty();
}

std::string Edge::GetUnescapedDepfile() const {
  EdgeEnv env(this, EdgeEnv::kDoNotEscape);
  return env.LookupVariable("depfile");
}

std::string Edge::GetUnescapedDyndep() const {
  EdgeEnv env(this, EdgeEnv::kDoNotEscape);
  return env.LookupVariable("dyndep");
}

std::string Edge::GetUnescapedRspfile() const {
  EdgeEnv env(this, EdgeEnv::kDoNotEscape);
  return env.LookupVariable("rspfile");
}

void Edge::Dump(const char* prefix) const
{
  printf("%s[ ", prefix);
  for(auto const& in : inputs_)
  {
    if(in == nullptr)
    {
      break;
    }
    printf("%s ", in->path().c_str());
  }
  printf("--%s-> ", rule_->name().c_str());
  for(auto const& out : outputs_)
  {
    if(out == nullptr)
    {
      break;
    }
    printf("%s ", out->path().c_str());
  }
  if (pool_) {
    if (!pool_->name().empty()) {
      printf("(in pool '%s')", pool_->name().c_str());
    }
  } else {
    printf("(null pool?)");
  }
  printf("] 0x%p\n", this);
}

bool Edge::is_phony() const {
  return rule_ == &State::kPhonyRule;
}

bool Edge::use_console() const {
  return pool() == &State::kConsolePool;
}

bool Edge::maybe_phonycycle_diagnostic() const {
  // CMake 2.8.12.x and 3.0.x produced self-referencing phony rules
  // of the form "build a: phony ... a ...".   Restrict our
  // "phonycycle" diagnostic option to the form it used.
  return is_phony() && outputs_.size() == 1 && implicit_outs_ == 0 &&
      implicit_deps_ == 0;
}

// static
std::string Node::PathDecanonicalized(const std::string& path, uint64_t slash_bits) {
  std::string result = path;
#ifdef _WIN32
  uint64_t mask = 1;
  for (char* c = &result[0]; (c = strchr(c, '/')) != nullptr;) {
    if (slash_bits & mask)
      *c = '\\';
    c++;
    mask <<= 1;
  }
#endif
  return result;
}

void Node::Dump(const char* prefix) const {
  printf("%s <%s 0x%p> mtime: %" PRId64 "%s, (:%s), ",
         prefix, path().c_str(), this,
         mtime(), mtime() ? "" : " (:missing)",
         dirty() ? " dirty" : " clean");
  if (in_edge()) {
    in_edge()->Dump("in-edge: ");
  } else {
    printf("no in-edge\n");
  }
  printf(" out edges:\n");
  for(auto const& out : out_edges())
  {
    if(out == nullptr)
    {
       break;
    }
    out->Dump(" +- ");
  }
}

bool ImplicitDepLoader::LoadDeps(Edge* edge, std::string* err) {
  std::string deps_type = edge->GetBinding("deps");
  if (!deps_type.empty())
    return LoadDepsFromLog(edge, err);

  std::string depfile = edge->GetUnescapedDepfile();
  if (!depfile.empty())
    return LoadDepFile(edge, depfile, err);

  // No deps to load.
  return true;
}

struct matches {
  matches(std::vector<std::string_view>::iterator i) : i_(i) {}

  bool operator()(const Node* node) const {
    return *i_ == node->path();
  }

  std::vector<std::string_view>::iterator i_;
};

bool ImplicitDepLoader::LoadDepFile(Edge* edge, const std::string& path,
                                    std::string* err) {
  METRIC_RECORD("depfile load");
  // Read depfile content.  Treat a missing depfile as empty.
  std::string content;
  switch (disk_interface_->ReadFile(path, &content, err)) {
  case DiskInterface::Okay:
    break;
  case DiskInterface::NotFound:
    err->clear();
    break;
  case DiskInterface::OtherError:
    *err = "loading '" + path + "': " + *err;
    return false;
  }
  // On a missing depfile: return false and empty *err.
  if (content.empty()) {
    EXPLAIN("depfile '%s' is missing", path.c_str());
    return false;
  }

  DepfileParser depfile(depfile_parser_options_
                        ? *depfile_parser_options_
                        : DepfileParserOptions());
  std::string depfile_err;
  if (!depfile.Parse(&content, &depfile_err)) {
    *err = path + ": " + depfile_err;
    return false;
  }

  if (depfile.outs_.empty()) {
    *err = path + ": no outputs declared";
    return false;
  }

  uint64_t unused;
  std::vector<std::string_view>::iterator primary_out = depfile.outs_.begin();
  // WTF. Const cast?
  size_t size = primary_out->size();
  if (!CanonicalizePath(const_cast<char*>(primary_out->data()),
                        &size, &unused, err)) {
    *err = path + ": " + *err;
    // Only needed becasue CanonicalizePath wanting to modify the inputs...
    *primary_out = primary_out->substr(0, size);
    return false;
  }
  // Only needed becasue CanonicalizePath wanting to modify the inputs...
  *primary_out = primary_out->substr(0, size);

  // Check that this depfile matches the edge's output, if not return false to
  // mark the edge as dirty.
  Node* first_output = edge->outputs_[0];
  std::string_view opath{first_output->path()};
  if (opath != *primary_out) {
    EXPLAIN("expected depfile '%s' to mention '%s', got '%s'", path.c_str(),
            first_output->path().c_str(), std::string(*primary_out).c_str());
    return false;
  }

  // Ensure that all mentioned outputs are outputs of the edge.
  for (std::vector<std::string_view>::iterator o = depfile.outs_.begin();
       o != depfile.outs_.end(); ++o) {
    matches m(o);
    if (std::find_if(edge->outputs_.begin(), edge->outputs_.end(), m) == edge->outputs_.end()) {
      *err = path + ": depfile mentions '" + std::string(*o) + "' as an output, but no such output was declared";
      return false;
    }
  }

  // Preallocate space in edge->inputs_ to be filled in below.
  std::vector<Node*>::iterator implicit_dep =
      PreallocateSpace(edge, depfile.ins_.size());

  // Add all its in-edges.
  for(auto & dep : depfile.ins_) {
    uint64_t slash_bits;
    size_t size = dep.size();
    // WTF. Const cast?
    if (!CanonicalizePath(const_cast<char*>(dep.data()), &size, &slash_bits, err))
    {
      // Only needed becasue CanonicalizePath wanting to modify the inputs...
      dep = dep.substr(0, size);
      return false;
    }
    // Only needed becasue CanonicalizePath wanting to modify the inputs...
    dep = dep.substr(0, size);

    Node* node = state_->GetNode(dep, slash_bits);
    *implicit_dep = node;
    node->AddOutEdge(edge);
    CreatePhonyInEdge(node);
    ++implicit_dep;
  }

  return true;
}

bool ImplicitDepLoader::LoadDepsFromLog(Edge* edge, std::string* err) {
  // NOTE: deps are only supported for single-target edges.
  Node* output = edge->outputs_[0];
  DepsLog::Deps* deps = deps_log_ ? deps_log_->GetDeps(output) : nullptr;
  if (!deps) {
    EXPLAIN("deps for '%s' are missing", output->path().c_str());
    return false;
  }

  // Deps are invalid if the output is newer than the deps.
  if (output->mtime() > deps->mtime) {
    EXPLAIN("stored deps info out of date for '%s' (%" PRId64 " vs %" PRId64 ")",
            output->path().c_str(), deps->mtime, output->mtime());
    return false;
  }

  std::vector<Node*>::iterator implicit_dep =
      PreallocateSpace(edge, deps->node_count);
  for (int i = 0; i < deps->node_count; ++i, ++implicit_dep) {
    Node* node = deps->nodes[i];
    *implicit_dep = node;
    node->AddOutEdge(edge);
    CreatePhonyInEdge(node);
  }
  return true;
}

std::vector<Node*>::iterator ImplicitDepLoader::PreallocateSpace(Edge* edge,
                                                            int count) {
  edge->inputs_.insert(edge->inputs_.end() - edge->order_only_deps_,
                       (size_t)count, 0);
  edge->implicit_deps_ += count;
  return edge->inputs_.end() - edge->order_only_deps_ - count;
}

void ImplicitDepLoader::CreatePhonyInEdge(Node* node) {
  if (node->in_edge())
    return;

  Edge* phony_edge = state_->AddEdge(&State::kPhonyRule);
  node->set_in_edge(phony_edge);
  phony_edge->outputs_.push_back(node);

  // RecomputeDirty might not be called for phony_edge if a previous call
  // to RecomputeDirty had caused the file to be stat'ed.  Because previous
  // invocations of RecomputeDirty would have seen this node without an
  // input edge (and therefore ready), we have to set outputs_ready_ to true
  // to avoid a potential stuck build.  If we do call RecomputeDirty for
  // this node, it will simply set outputs_ready_ to the correct value.
  phony_edge->outputs_ready_ = true;
}
