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

#ifndef NINJA_VERSION_H_
#define NINJA_VERSION_H_

#include <tuple>
#include <string_view>

/// The version number of the current Ninja release.  This will always
/// be "git" on trunk.
static constexpr auto kNinjaVersion = "1.9.0.git";

/// Parse the major/minor components of a version string.
std::tuple<size_t, size_t> ParseVersion(std::string_view version);

/// Check whether \a version is compatible with the current Ninja version,
/// aborting if not.
void CheckNinjaVersion(std::string_view required_version);

#endif  // NINJA_VERSION_H_
