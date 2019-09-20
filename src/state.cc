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

#include "state.h"

#include "graph.h"
#include "util.h"
#include "metrics.h"
#include "edit_distance.h"
#include "string_concat.h"

#include <assert.h>
#include <stdio.h>

void Pool::EdgeScheduled(const Edge& edge) {
  if (depth_ != 0)
    current_use_ += edge.weight();
}

void Pool::EdgeFinished(const Edge& edge) {
  if (depth_ != 0)
    current_use_ -= edge.weight();
}

void Pool::DelayEdge(Edge* edge) {
  assert(depth_ != 0);
  delayed_.insert(edge);
}

void Pool::RetrieveReadyEdges(std::set<Edge*>* ready_queue) {
  DelayedEdges::iterator it = delayed_.begin();
  while (it != delayed_.end()) {
    Edge* edge = *it;
    if (current_use_ + edge->weight() > depth_)
      break;
    ready_queue->insert(edge);
    EdgeScheduled(*edge);
    ++it;
  }
  delayed_.erase(delayed_.begin(), it);
}

void Pool::Dump() const {
  printf("%s (%d/%d) ->\n", name_.c_str(), current_use_, depth_);
  for (const auto & item : delayed_)
  {
    printf("\t");
    item->Dump();
  }
}

// static
bool Pool::WeightedEdgeCmp(const Edge* a, const Edge* b) {
  if (!a) return b;
  if (!b) return false;
  int weight_diff = a->weight() - b->weight();
  return ((weight_diff < 0) || (weight_diff == 0 && a < b));
}

Pool State::kDefaultPool("", 0);
Pool State::kConsolePool("console", 1);
const Rule State::kPhonyRule("phony");

State::State() {
  bindings_.AddRule(&kPhonyRule);
  AddPool(&kDefaultPool);
  AddPool(&kConsolePool);
}

void State::AddPool(Pool* pool) {
  assert(LookupPool(pool->name()) == nullptr);
  pools_[pool->name()] = pool;
}

Pool* State::LookupPool(std::string_view pool_name) {
  auto const& i = pools_.find(pool_name);
  return   i != pools_.end()
         ? i->second
         : nullptr;
}

Edge* State::AddEdge(const Rule* rule) {
  Edge* edge = new Edge();
  edge->rule_ = rule;
  edge->pool_ = &State::kDefaultPool;
  edge->env_ = &bindings_;
  edges_.push_back(edge);
  return edge;
}

Node* State::GetNode(std::string_view path, uint64_t slash_bits) {
  Node* node = LookupNode(path);
  if (node)
    return node;
  node = new Node(std::string(path), slash_bits);
  paths_[node->path()] = node;
  return node;
}

Node* State::LookupNode(std::string_view path) const {
  METRIC_RECORD("lookup node");
  if(auto const& i = paths_.find(path); i != paths_.end())
  {
    return i->second;
  }
  return nullptr;
}

Node* State::SpellcheckNode(std::string_view path) {
  const bool kAllowReplacements = true;
  const int kMaxValidEditDistance = 3;

  int min_distance = kMaxValidEditDistance + 1;
  Node* result = nullptr;
  for (auto const& [pathKey, node] : paths_)
  {
    int distance = EditDistance(pathKey, path, kAllowReplacements, kMaxValidEditDistance);
    if (distance < min_distance && node)
    {
      min_distance = distance;
      result = node;
    }
  }
  return result;
}

void State::AddIn(Edge* edge, std::string_view path, uint64_t slash_bits) {
  Node* node = GetNode(path, slash_bits);
  edge->inputs_.push_back(node);
  node->AddOutEdge(edge);
}

bool State::AddOut(Edge* edge, std::string_view path, uint64_t slash_bits) {
  Node* node = GetNode(path, slash_bits);
  if (node->in_edge())
    return false;
  edge->outputs_.push_back(node);
  node->set_in_edge(edge);
  return true;
}

bool State::AddDefault(std::string_view path, std::string* err) {
  Node* node = LookupNode(path);
  if (!node) {
    *err = string_concat("unknown target '", path, "'");
    return false;
  }
  defaults_.push_back(node);
  return true;
}

std::vector<Node*> State::RootNodes(std::string* err) const {
  std::vector<Node*> root_nodes;
  // Search for nodes with no output.
  for (const auto & edge : edges_)
  {
    for (const auto & output : edge->outputs_)
    {
      if (output->out_edges().empty())
      {
        root_nodes.push_back(output);
      }
    }
  }

  if (!edges_.empty() && root_nodes.empty())
    *err = "could not determine root nodes of build graph";

  return root_nodes;
}

std::vector<Node*> State::DefaultNodes(std::string* err) const {
  return defaults_.empty() ? RootNodes(err) : defaults_;
}

void State::Reset() {
  for (const auto & path : paths_)
  {
    path.second->ResetState();
  }
  for (const auto & edge : edges_)
  {
    edge->outputs_ready_ = false;
    edge->deps_loaded_ = false;
    edge->mark_ = Edge::VisitNone;
  }
}

void State::Dump() {
  for (const auto & path : paths_)
  {
    Node* node = path.second;
    printf("%s %s [id:%d]\n",
           node->path().c_str(),
           node->status_known()
            ? (  node->dirty()
               ? "dirty"
               : "clean")
            : "unknown",
           node->id());
  }
  if (!pools_.empty()) {
    printf("resource_pools:\n");
    for (const auto & pool : pools_)
    {
      if (!pool.second->name().empty())
      {
        pool.second->Dump();
      }
    }
  }
}
