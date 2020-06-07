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

#include "clean.h"

#include <assert.h>
#include <stdio.h>

#include "disk_interface.h"
#include "graph.h"
#include "state.h"
#include "util.h"

Cleaner::Cleaner(State* state,
                 const BuildConfig& config,
                 DiskInterface* disk_interface)
  : state_(state)
  , config_(config)
  , dyndep_loader_(state, disk_interface)
  , removed_()
  , cleaned_()
  , cleaned_files_count_(0)
  , disk_interface_(disk_interface)
  , status_(0)
{ }

int Cleaner::RemoveFile(std::filesystem::path const& path)
{
  return disk_interface_->RemoveFile(path);
}

bool Cleaner::FileExists(std::filesystem::path const& path)
{
  std::error_code err;
  TimeStamp mtime = disk_interface_->Stat(path, err);
  if(mtime == TimeStamp::max())
  {
    Error("%s", err.message().c_str());
  }
  return mtime > TimeStamp::min();  // Treat Stat() errors as "file does not exist".
}

void Cleaner::Report(std::filesystem::path const& path) {
  ++cleaned_files_count_;
  if (IsVerbose())
    printf("Remove %s\n", path.generic_string().c_str());
}

void Cleaner::Remove(std::filesystem::path const& path) {
  if (!IsAlreadyRemoved(path)) {
    removed_.insert(path);
    if (config_.dry_run) {
      if (FileExists(path))
        Report(path);
    } else {
      int ret = RemoveFile(path);
      if (ret == 0)
        Report(path);
      else if (ret == -1)
        status_ = 1;
    }
  }
}

bool Cleaner::IsAlreadyRemoved(std::filesystem::path const& path) {
  return removed_.find(path) != removed_.end();
}

void Cleaner::RemoveEdgeFiles(Edge* edge) {
  std::filesystem::path const& depfile = edge->GetUnescapedDepfile();
  if (!depfile.empty())
    Remove(depfile);

  std::filesystem::path const& rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty())
    Remove(rspfile);
}

void Cleaner::PrintHeader() {
  if (config_.verbosity == BuildConfig::QUIET)
    return;
  printf("Cleaning...");
  if (IsVerbose())
    printf("\n");
  else
    printf(" ");
  fflush(stdout);
}

void Cleaner::PrintFooter() {
  if (config_.verbosity == BuildConfig::QUIET)
    return;
  printf("%d files.\n", cleaned_files_count_);
}

int Cleaner::CleanAll(bool generator) {
  Reset();
  PrintHeader();
  LoadDyndeps();
  for (const auto & item : state_->edges_)
  {
    // Do not try to remove phony targets
    if (item->is_phony())
      continue;
    // Do not remove generator's files unless generator specified.
    if (!generator && item->GetBindingBool("generator"))
      continue;
    for (const auto & inner : item->outputs_)
    {
      Remove(inner->path());
    }

    RemoveEdgeFiles(item);
  }
  PrintFooter();
  return status_;
}

int Cleaner::CleanDead(const BuildLog::Entries& entries) {
  Reset();
  PrintHeader();
  for (const auto& entry : entries) {
    Node* n = state_->LookupNode(entry.first);
    if (!n || !n->in_edge()) {
      Remove(entry.first);
    }
  }
  PrintFooter();
  return status_;
}

void Cleaner::DoCleanTarget(Node* target) {
  if (Edge* e = target->in_edge()) {
    // Do not try to remove phony targets
    if (!e->is_phony()) {
      Remove(target->path());
      RemoveEdgeFiles(e);
    }
    for (const auto & next : e->inputs_) {
      // call DoCleanTarget recursively if this node has not been visited
      if (cleaned_.count(next) == 0) {
        DoCleanTarget(next);
      }
    }
  }

  // mark this target to be cleaned already
  cleaned_.insert(target);
}

int Cleaner::CleanTarget(Node* target) {
  assert(target);

  Reset();
  PrintHeader();
  LoadDyndeps();
  DoCleanTarget(target);
  PrintFooter();
  return status_;
}

int Cleaner::CleanTarget(const char* target) {
  assert(target);

  Reset();
  Node* node = state_->LookupNode(target);
  if (node) {
    CleanTarget(node);
  } else {
    Error("unknown target '%s'", target);
    status_ = 1;
  }
  return status_;
}

int Cleaner::CleanTargets(int target_count, char* targets[]) {
  Reset();
  PrintHeader();
  LoadDyndeps();
  for (int i = 0; i < target_count; ++i) {
    Node* target = state_->LookupNode(targets[i]);
    if (target) {
      if (IsVerbose())
        printf("Target %s\n", targets[i]);
      DoCleanTarget(target);
    } else {
      Error("unknown target '%s'", targets[i]);
      status_ = 1;
    }
  }
  PrintFooter();
  return status_;
}

void Cleaner::DoCleanRule(const Rule* rule) {
  assert(rule);

  for (const auto & item : state_->edges_)
  {
    if (item->rule().name() == rule->name())
    {
      for (const auto & inner : item->outputs_)
      {
        Remove(inner->path());
        RemoveEdgeFiles(item);
      }
    }
  }
}

int Cleaner::CleanRule(const Rule* rule) {
  assert(rule);

  Reset();
  PrintHeader();
  LoadDyndeps();
  DoCleanRule(rule);
  PrintFooter();
  return status_;
}

int Cleaner::CleanRule(const char* rule) {
  assert(rule);

  Reset();
  const Rule* r = state_->bindings_.LookupRule(rule);
  if (r) {
    CleanRule(r);
  } else {
    Error("unknown rule '%s'", rule);
    status_ = 1;
  }
  return status_;
}

int Cleaner::CleanRules(int rule_count, char* rules[]) {
  assert(rules);

  Reset();
  PrintHeader();
  LoadDyndeps();
  for (int i = 0; i < rule_count; ++i) {
    const char* rule_name = rules[i];
    const Rule* rule = state_->bindings_.LookupRule(rule_name);
    if (rule) {
      if (IsVerbose())
        printf("Rule %s\n", rule_name);
      DoCleanRule(rule);
    } else {
      Error("unknown rule '%s'", rule_name);
      status_ = 1;
    }
  }
  PrintFooter();
  return status_;
}

void Cleaner::Reset() {
  status_ = 0;
  cleaned_files_count_ = 0;
  removed_.clear();
  cleaned_.clear();
}

void Cleaner::LoadDyndeps() {
  // Load dyndep files that exist, before they are cleaned.
  for (auto const& item : state_->edges_) {
    if (Node* dyndep = item->dyndep_) {
      // Capture and ignore errors loading the dyndep file.
      // We clean as much of the graph as we know.
      std::error_code err;
      dyndep_loader_.LoadDyndeps(dyndep, err);
    }
  }
}
