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

#ifndef NINJA_DISK_INTERFACE_H_
#define NINJA_DISK_INTERFACE_H_

#include "timestamp.h"

#include <map>
#include <string>
#include <filesystem>

/// Interface for reading files from disk.  See DiskInterface for details.
/// This base offers the minimum interface needed just to read files.
struct FileReader {
  /// Result of ReadFile.
  enum Status {
    Okay,
    NotFound,
    OtherError
  };

  /// Read and store in given string.  On success, return Okay.
  /// On error, return another Status and fill |err|.
  virtual Status ReadFile(std::filesystem::path const& path,
                          std::string* contents,
                          std::string* err) = 0;

protected:
  ~FileReader() = default;
};

/// Interface for accessing the disk.
///
/// Abstract so it can be mocked out for tests.  The real implementation
/// is RealDiskInterface.
struct DiskInterface: public FileReader {
  /// stat() a file, returning the mtime, or TimeStamp::min() if missing
  ///  and TimeStamp::max() on other errors.
  virtual TimeStamp Stat(std::filesystem::path const& path, std::string* err) const = 0;

  /// Create a directory, returning false on failure.
  virtual bool MakeDir(std::filesystem::path const& path) = 0;

  /// Create a file, with the specified name and contents
  /// Returns true on success, false on failure
  virtual bool WriteFile(std::filesystem::path const& path, std::string_view contents) = 0;

  /// Remove the file named @a path. It behaves like 'rm -f path' so no errors
  /// are reported if it does not exists.
  /// @returns 0 if the file has been removed,
  ///          1 if the file does not exist, and
  ///          -1 if an error occurs.
  virtual int RemoveFile(std::filesystem::path const& path) = 0;

  /// Create all the parent directories for path; like mkdir -p
  /// `basename path`.
  virtual bool MakeDirs(std::filesystem::path const& path) = 0;

protected:
  ~DiskInterface() = default;
};

/// Implementation of DiskInterface that actually hits the disk.
struct RealDiskInterface final : public DiskInterface {
  RealDiskInterface() = default;
  ~RealDiskInterface() = default;

  TimeStamp Stat(std::filesystem::path const& path, std::string* err) const override final;
  bool MakeDir(std::filesystem::path const& path) override final;
  bool WriteFile(std::filesystem::path const& path, std::string_view contents) override final;
  Status ReadFile(std::filesystem::path const& path, std::string* contents,
                  std::string* err) override final;
  int RemoveFile(std::filesystem::path const& path) override final;
  bool MakeDirs(std::filesystem::path const& path) override final;

  /// Whether stat information can be cached.  Only has an effect on Windows.
  void AllowStatCache(bool allow);

 private:
  /// Whether stat information can be cached.
  bool use_cache_ = false;

  using DirCache = std::map<std::filesystem::path, TimeStamp, std::less<>>;

  // TODO: Neither a map nor a hashmap seems ideal here.  If the statcache
  // works out, come up with a better data structure.
  // A Trie is the appropriate datastructure for this.
  using Cache = std::map<std::filesystem::path, DirCache, std::less<>>;

  mutable Cache cache_;
};

#endif  // NINJA_DISK_INTERFACE_H_
