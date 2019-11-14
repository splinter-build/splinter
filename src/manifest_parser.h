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

#ifndef NINJA_MANIFEST_PARSER_H_
#define NINJA_MANIFEST_PARSER_H_

#include "parser.h"

struct BindingEnv;
struct EvalString;

enum DupeEdgeAction {
  kDupeEdgeActionWarn,
  kDupeEdgeActionError,
};

enum PhonyCycleAction {
  kPhonyCycleActionWarn,
  kPhonyCycleActionError,
};

struct ManifestParserOptions final {
  ManifestParserOptions() = default;
  DupeEdgeAction dupe_edge_action_ = kDupeEdgeActionWarn;
  PhonyCycleAction phony_cycle_action_ = kPhonyCycleActionWarn;
};

/// Parses .ninja files.
struct ManifestParser final : public Parser {
  ManifestParser(State* state, FileReader* file_reader,
                 ManifestParserOptions options = ManifestParserOptions());

  /// Parse a text string of input.  Used by tests.
  bool ParseTest(const std::string& input, std::error_code& err) {
    quiet_ = true;
    return Parse("input", input, err);
  }

private:
  /// Parse a file, given its contents as a std::string.
  bool Parse(std::filesystem::path const& filename, const std::string& input, std::error_code& err) override final;

  /// Parse various statement types.
  bool ParsePool(std::error_code& err);
  bool ParseRule(std::error_code& err);
  bool ParseLet(std::string* key, EvalString* val, std::error_code& err);
  bool ParseEdge(std::error_code& err);
  bool ParseDefault(std::error_code& err);

  /// Parse either a 'subninja' or 'include' line.
  bool ParseFileInclude(bool new_scope, std::error_code& err);

  BindingEnv* env_;
  ManifestParserOptions options_;
  bool quiet_ = false;
};

#endif  // NINJA_MANIFEST_PARSER_H_
