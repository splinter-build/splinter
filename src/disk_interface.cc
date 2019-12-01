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

#include "disk_interface.h"

#include "metrics.h"

// DiskInterface ---------------------------------------------------------------

bool DiskInterface::MakeDirs(std::filesystem::path const& path)
{
  std::error_code ec;
  bool const success = std::filesystem::create_directories(path, ec);
  return success && ! ec;
}

// RealDiskInterface -----------------------------------------------------------

TimeStamp RealDiskInterface::Stat(std::filesystem::path const& path, std::string* err) const
{
  METRIC_RECORD("node stat");

  if( ! use_cache_)
  {
    std::error_code ec;
    auto time = std::filesystem::last_write_time(path, ec);
    if(ec)
    {
      *err = ec.message();
      return TimeStamp::max();
    }
    else
    {
      return time;
    }
  }

  std::filesystem::path const& directory = path.parent_path();
  std::string dir = directory.native();
  std::transform(dir.begin(), dir.end(), dir.begin(), ::tolower);

  Cache::iterator ci = cache_.find(dir);
  if(ci == cache_.end())
  {
    ci = cache_.emplace(std::move(dir), DirCache()).first;
    // TODO: Assert success.
    for(auto const& entry : std::filesystem::directory_iterator(directory))
    {
      std::string lowername = entry.path().filename();
      std::transform(lowername.begin(), lowername.end(), lowername.begin(), ::tolower);

      std::error_code ec;
      auto const& [it, success] = ci->second.emplace(std::move(lowername), entry.last_write_time(ec));
      if( ! success || ec)
      {
          *err = ec.message();
          cache_.erase(ci);
          // TODO: If we're going to abort, shouldn't we also remove all of the previously inserted entries?
          return TimeStamp::max();
      }
    }
  }

  DirCache const& cache = ci->second;
  DirCache::const_iterator di = cache.find(path.filename());
  return di != cache.end() ? di->second : TimeStamp::min();
}

bool RealDiskInterface::WriteFile(std::filesystem::path const& path, std::string_view const contents)
{
  FILE* fp = fopen(path.c_str(), "w");
  if(fp == nullptr)
  {
    Error("WriteFile(%s): Unable to create file. %s",
          path.c_str(), strerror(errno));
    return false;
  }

  if(fwrite(contents.data(), 1, contents.length(), fp) < contents.length())
  {
    Error("WriteFile(%s): Unable to write to the file. %s",
          path.c_str(), strerror(errno));
    fclose(fp);
    return false;
  }

  if(fclose(fp) == EOF)
  {
    Error("WriteFile(%s): Unable to close the file. %s",
          path.c_str(), strerror(errno));
    return false;
  }

  return true;
}

bool RealDiskInterface::MakeDir(std::filesystem::path const& path)
{
  std::error_code ec;
  bool const success = std::filesystem::create_directory(path, ec);
  if( ! success || ec)
  {
    if(ec == std::make_error_code(std::errc::file_exists))
    {
      return true;
    }
    else
    {
      Error("mkdir(%s): %s", path.c_str(), ec.message().c_str());
      return false;
    }
  }
  return true;
}

FileReader::Status RealDiskInterface::ReadFile(std::filesystem::path const& path,
                                               std::string* contents,
                                               std::string* err)
{
  // Need to call the version of this from util.cc
  // Bad naming that there's an overlap.
  switch(::ReadFile(path, contents, err))
  {
    case 0:       return Okay;
    case -ENOENT: return NotFound;
    default:      return OtherError;
  }
}

int RealDiskInterface::RemoveFile(std::filesystem::path const& path)
{
  std::error_code ec;
  bool const success = std::filesystem::remove(path, ec);
  if( ! success || ec)
  {
    if(ec == std::make_error_code(std::errc::no_such_file_or_directory))
    {
        return 1;
    }
    else
    {
      Error("remove(%s): %s", path.c_str(), ec.message().c_str());
      return -1;
    }
  }
  return 0;
}

void RealDiskInterface::AllowStatCache(bool allow)
{
  use_cache_ = allow;
  if( ! use_cache_)
  {
    cache_.clear();
  }
}
