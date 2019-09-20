// Copyright 2013 Google Inc. All Rights Reserved.
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

#include "version.h"
#include "util.h"

#include <charconv>
#include <stdexcept>
//#include <system_error>

std::tuple<size_t, size_t> ParseVersion(std::string_view const version)
{
  size_t start = 0;
  size_t end   = version.find('.');

  size_t major = 0;
  size_t minor = 0;

  std::from_chars_result res = std::from_chars(version.begin() + start,
                                               version.begin() + end,
                                               major);
  if(res.ptr == version.begin()+start)
  {
//    throw std::system_error(std::make_error_code(res.ec));
    return {0, 0};
  }

  if(std::string_view::npos != end)
  {
    start = end + 1;
    end = version.find('.', start);

    res = std::from_chars(version.begin() + start,
                          version.begin() + end,
                          minor);

    if(res.ptr == version.begin()+start)
    {
//      throw std::system_error(std::make_error_code(res.ec));
      return {major, 0};
    }
  }

  return {major, minor};
}

void CheckNinjaVersion(std::string_view const version)
{
  auto const& [bin_major, bin_minor]   = ParseVersion(kNinjaVersion);
  auto const& [file_major, file_minor] = ParseVersion(version);

  if(bin_major > file_major)
  {
    Warning("ninja executable version (%s) greater than build file "
            "ninja_required_version (%s); versions may be incompatible.",
            kNinjaVersion, std::string(version).c_str());
    return;
  }

  if(   (bin_major == file_major && bin_minor < file_minor)
     || (bin_major < file_major))
  {
    Fatal("ninja version (%s) incompatible with build file "
          "ninja_required_version version (%s).",
          kNinjaVersion, std::string(version).c_str());
  }
}
