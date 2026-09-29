#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sysmon {

struct DiskStats {
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t freeBytes = 0;  // free to an unprivileged user
    double usagePercent = 0.0;
};

// Computes usage the way df(1) does: reserved blocks count as used, and the
// percentage is measured against what a normal user could actually occupy.
// Returns nullopt when the filesystem reports no blocks at all.
std::optional<DiskStats> computeDiskStats(uint64_t blockSize, uint64_t totalBlocks,
                                          uint64_t freeBlocks, uint64_t availableBlocks);

class DiskMonitor {
public:
    explicit DiskMonitor(std::string mountPoint = "/");

    std::optional<DiskStats> sample() const;
    const std::string& mountPoint() const { return mountPoint_; }

private:
    std::string mountPoint_;
};

}  // namespace sysmon
