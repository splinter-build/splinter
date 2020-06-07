// Copyright 2018 Google Inc. All Rights Reserved.
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

#include "parser.h"

#include "metrics.h"
#include "disk_interface.h"
#include "string_concat.h"

bool Parser::Load(std::filesystem::path const& filename, std::error_code& err, Lexer* parent) {
  METRIC_RECORD(".ninja parse");
  std::string contents;
  if(auto outcome = file_reader_->ReadFile(filename, &contents, err);
     (outcome != FileReader::Okay) || err)
  {
    if(parent)
    {
      parent->Error(err.message(), err);
    }
    return false;
  }

  // The lexer needs a nul byte at the end of its input, to know when it's done.
  // It takes a StringPiece, and StringPiece's std::string constructor uses
  // std::string::data().  data()'s return value isn't guaranteed to be
  // null-terminated (although in practice - libc++, libstdc++, msvc's stl --
  // it is, and C++11 demands that too), so add an explicit nul byte.
  contents.resize(contents.size() + 1);

  return Parse(filename, contents, err);
}

bool Parser::ExpectToken(Lexer::Token expected, std::error_code& err) {
  Lexer::Token token = lexer_.ReadToken();
  if (token != expected) {
    std::string message = string_concat("expected ", Lexer::TokenName(expected),
                                        ", got ", Lexer::TokenName(token),
                                        Lexer::TokenErrorHint(expected));
    return lexer_.Error(std::move(message), err);
  }
  return true;
}
