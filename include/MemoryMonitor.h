#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sysmon {

struct MemoryStats {
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t availableBytes = 0;
    double usagePercent = 0.0;
};

// Parses /proc/meminfo. Prefers MemAvailable (the kernel's own estimate of what
// can be handed out without swapping); on kernels older than 3.14 that field is
// absent, so MemFree + Buffers + Cached + SReclaimable is used instead.
// Returns nullopt when MemTotal is missing or zero.
std::optional<MemoryStats> parseMemInfo(const std::string& contents);

class MemoryMonitor {
public:
    std::optional<MemoryStats> sample() const;
};

}  // namespace sysmon
