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

#include <assert.h>

#include "eval_env.h"
#include "string_concat.h"

std::string BindingEnv::LookupVariable(const std::string& var) {
  std::map<std::string, std::string>::iterator i = bindings_.find(var);
  if (i != bindings_.end())
    return i->second;
  if (parent_)
    return parent_->LookupVariable(var);
  return "";
}

void BindingEnv::AddBinding(const std::string& key, const std::string& val) {
  bindings_[key] = val;
}

void BindingEnv::AddRule(const Rule* rule) {
  assert(LookupRuleCurrentScope(rule->name()) == nullptr);
  rules_[rule->name()] = rule;
}

const Rule* BindingEnv::LookupRuleCurrentScope(const std::string& rule_name) {
  std::map<std::string, const Rule*>::iterator i = rules_.find(rule_name);
  if (i == rules_.end())
    return nullptr;
  return i->second;
}

const Rule* BindingEnv::LookupRule(const std::string& rule_name) {
  std::map<std::string, const Rule*>::iterator i = rules_.find(rule_name);
  if (i != rules_.end())
    return i->second;
  if (parent_)
    return parent_->LookupRule(rule_name);
  return nullptr;
}

void Rule::AddBinding(const std::string& key, const EvalString& val) {
  bindings_[key] = val;
}

const EvalString* Rule::GetBinding(const std::string& key) const {
  Bindings::const_iterator i = bindings_.find(key);
  if (i == bindings_.end())
    return nullptr;
  return &i->second;
}

// static
bool Rule::IsReservedBinding(const std::string& var) {
  return var == "command" ||
      var == "depfile" ||
      var == "dyndep" ||
      var == "description" ||
      var == "deps" ||
      var == "generator" ||
      var == "pool" ||
      var == "restat" ||
      var == "rspfile" ||
      var == "rspfile_content" ||
      var == "msvc_deps_prefix";
}

const std::map<std::string, const Rule*>& BindingEnv::GetRules() const {
  return rules_;
}

std::string BindingEnv::LookupWithFallback(const std::string& var,
                                      const EvalString* eval,
                                      Env* env) {
  std::map<std::string, std::string>::iterator i = bindings_.find(var);
  if (i != bindings_.end())
    return i->second;

  if (eval)
    return eval->Evaluate(env);

  if (parent_)
    return parent_->LookupVariable(var);

  return "";
}

std::string EvalString::Evaluate(Env* env) const {
  std::string result;
  for (const auto & item : parsed_)
  {
    if (item.second == RAW)
      result.append(item.first);
    else
      result.append(env->LookupVariable(item.first));
  }
  return result;
}

void EvalString::AddText(std::string_view text) {
  // Add it to the end of an existing RAW token if possible.
  if (!parsed_.empty() && parsed_.back().second == RAW) {
    parsed_.back().first.append(text);
  } else {
    parsed_.emplace_back(std::string(text), RAW);
  }
}
void EvalString::AddSpecial(std::string_view text) {
  parsed_.emplace_back(std::string(text), SPECIAL);
}

std::string EvalString::Serialize() const {
  std::string result;
  for (const auto & item : parsed_)
  {
    if(item.second == SPECIAL)
    {
        string_append(result, "[$", item.first, "]");
    }
    else
    {
        string_append(result, "[", item.first, "]");
    }
  }
  return result;
}

std::string EvalString::Unparse() const {
  std::string result;
  for (auto const& item : parsed_) {
    bool special = (item.second == SPECIAL);
    if (special)
      result.append("${");
    result.append(item.first);
    if (special)
      result.append("}");
  }
  return result;
}
