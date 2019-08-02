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
  if(auto const& i = bindings_.find(var); i != bindings_.end())
  {
    return i->second;
  }
  else if(parent_)
  {
    return parent_->LookupVariable(var);
  }
  return {};
}

void BindingEnv::AddBinding(const std::string& key, const std::string& val) {
  bindings_[key] = val;
}

void BindingEnv::AddRule(const Rule* rule) {
  assert(LookupRuleCurrentScope(rule->name()) == nullptr);
  rules_[rule->name()] = rule;
}

const Rule* BindingEnv::LookupRuleCurrentScope(const std::string& rule_name) {
  if(auto const& i = rules_.find(rule_name); i != rules_.end())
  {
    return i->second;
  }
  return nullptr;
}

const Rule* BindingEnv::LookupRule(const std::string& rule_name) {
  if(auto const& i = rules_.find(rule_name); i != rules_.end())
  {
    return i->second;
  }
  else if (parent_)
  {
    return parent_->LookupRule(rule_name);
  }
  return nullptr;
}

void Rule::AddBinding(const std::string& key, const EvalString& val) {
  bindings_[key] = val;
}

const EvalString* Rule::GetBinding(const std::string& key) const {
  if(auto const& i = bindings_.find(key); i != bindings_.end())
  {
    return &i->second;
  }
  return nullptr;
}

// static
bool Rule::IsReservedBinding(const std::string& var) {
  return    var == "deps"
         || var == "pool"
         || var == "dyndep"
         || var == "restat"
         || var == "command"
         || var == "depfile"
         || var == "rspfile"
         || var == "generator"
         || var == "description"
         || var == "rspfile_content"
         || var == "msvc_deps_prefix";
}

const std::map<std::string, const Rule*>& BindingEnv::GetRules() const {
  return rules_;
}

std::string BindingEnv::LookupWithFallback(const std::string& var,
                                      const EvalString* eval,
                                      Env* env) {
  if (auto const& i = bindings_.find(var); i != bindings_.end())
  {
    return i->second;
  }

  if (eval)
  {
    return eval->Evaluate(env);
  }

  if (parent_)
  {
    return parent_->LookupVariable(var);
  }

  return {};
}

std::string EvalString::Evaluate(Env* env) const {
  std::string result;
  for (auto const& [token, type] : parsed_)
  {
    if (type == RAW)
    {
      result.append(token);
    }
    else
    {
      result.append(env->LookupVariable(token));
    }
  }
  return result;
}

void EvalString::AddText(std::string_view text) {
  // Add it to the end of an existing RAW token if possible.
  if(parsed_.empty())
  {
    parsed_.emplace_back(std::string(text), RAW);
  }
  else if(auto & [token, type] = parsed_.back(); type == RAW)
  {
    token.append(text);
  }
  else
  {
    parsed_.emplace_back(std::string(text), RAW);
  }
}

void EvalString::AddSpecial(std::string_view text) {
  parsed_.emplace_back(std::string(text), SPECIAL);
}

std::string EvalString::Serialize() const {
  std::string result;
  // TODO: Precalculate allocation
  for (auto const& [token, type] : parsed_)
  {
    if(type == SPECIAL)
    {
        string_append(result, "[$", token, "]");
    }
    else
    {
        string_append(result, "[", token, "]");
    }
  }
  return result;
}

std::string EvalString::Unparse() const {
  std::string result;
  // TODO: Precalculate allocation
  for (auto const& [token, type] : parsed_)
  {
    if(type == SPECIAL)
    {
        string_append(result, "${", token, "}");
    }
    else
    {
        result.append(token);
    }
  }
  return result;
}
