#pragma once

#include <optional>
#include <string>

namespace sysmon::procfs {

// Reads a whole file from /proc (or anywhere else) into a string.
//
// Files under /proc report a size of zero, so the contents have to be streamed
// rather than sized up front. Returns nullopt when the file is missing or
// unreadable; callers treat that as "this metric is unavailable" instead of an
// error, because /proc entries can vanish between listing and reading.
std::optional<std::string> readFile(const std::string& path);

}  // namespace sysmon::procfs
