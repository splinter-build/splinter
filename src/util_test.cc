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

#include <algorithm>
#include <cstring>
#include "util.h"

#include "test.h"

namespace
{

TEST(PathEscaping, TortureTest) {
  std::string result;

  GetWin32EscapedString("foo bar\\\"'$@d!st!c'\\path'\\", &result);
  EXPECT_EQ("\"foo bar\\\\\\\"'$@d!st!c'\\path'\\\\\"", result);
  result.clear();

  GetShellEscapedString("foo bar\"/'$@d!st!c'/path'", &result);
  EXPECT_EQ("'foo bar\"/'\\''$@d!st!c'\\''/path'\\'''", result);
}

TEST(PathEscaping, SensiblePathsAreNotNeedlesslyEscaped) {
  const char* path = "some/sensible/path/without/crazy/characters.c++";
  std::string result;

  GetWin32EscapedString(path, &result);
  EXPECT_EQ(path, result);
  result.clear();

  GetShellEscapedString(path, &result);
  EXPECT_EQ(path, result);
}

TEST(PathEscaping, SensibleWin32PathsAreNotNeedlesslyEscaped) {
  const char* path = "some\\sensible\\path\\without\\crazy\\characters.c++";
  std::string result;

  GetWin32EscapedString(path, &result);
  EXPECT_EQ(path, result);
}

TEST(StripAnsiEscapeCodes, EscapeAtEnd) {
  std::string stripped = StripAnsiEscapeCodes("foo\33");
  EXPECT_EQ("foo", stripped);

  stripped = StripAnsiEscapeCodes("foo\33[");
  EXPECT_EQ("foo", stripped);
}

TEST(StripAnsiEscapeCodes, StripColors) {
  // An actual clang warning.
  std::string input = "\33[1maffixmgr.cxx:286:15: \33[0m\33[0;1;35mwarning: "
                 "\33[0m\33[1musing the result... [-Wparentheses]\33[0m";
  std::string stripped = StripAnsiEscapeCodes(input);
  EXPECT_EQ("affixmgr.cxx:286:15: warning: using the result... [-Wparentheses]",
            stripped);
}

TEST(ElideMiddle, NothingToElide) {
  std::string input = "Nothing to elide in this short string.";
  EXPECT_EQ(input, ElideMiddle(input, 80));
  EXPECT_EQ(input, ElideMiddle(input, 38));
  EXPECT_EQ("", ElideMiddle(input, 0));
  EXPECT_EQ(".", ElideMiddle(input, 1));
  EXPECT_EQ("..", ElideMiddle(input, 2));
  EXPECT_EQ("...", ElideMiddle(input, 3));
}

TEST(ElideMiddle, ElideInTheMiddle) {
  std::string input = "01234567890123456789";
  std::string elided = ElideMiddle(input, 10);
  EXPECT_EQ("012...789", elided);
  EXPECT_EQ("01234567...23456789", ElideMiddle(input, 19));
}

}
