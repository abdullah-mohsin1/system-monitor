#include "DiskMonitor.h"

#include <sys/statvfs.h>

#include <utility>

namespace sysmon {

std::optional<DiskStats> computeDiskStats(uint64_t blockSize, uint64_t totalBlocks,
                                          uint64_t freeBlocks, uint64_t availableBlocks) {
    if (blockSize == 0 || totalBlocks == 0) {
        return std::nullopt;
    }

    DiskStats stats;
    stats.totalBytes = totalBlocks * blockSize;
    stats.freeBytes = availableBlocks * blockSize;
    // Blocks reserved for root are neither free nor available, so treating
    // "total minus free" as used keeps the numbers consistent with df.
    stats.usedBytes = (totalBlocks - freeBlocks) * blockSize;

    const uint64_t usableBytes = stats.usedBytes + stats.freeBytes;
    stats.usagePercent = usableBytes == 0
                             ? 0.0
                             : 100.0 * static_cast<double>(stats.usedBytes) /
                                   static_cast<double>(usableBytes);
    return stats;
}

DiskMonitor::DiskMonitor(std::string mountPoint) : mountPoint_(std::move(mountPoint)) {}

std::optional<DiskStats> DiskMonitor::sample() const {
    struct statvfs filesystem{};
    if (statvfs(mountPoint_.c_str(), &filesystem) != 0) {
        return std::nullopt;
    }

    // f_frsize is the fragment size, which is the unit f_blocks is counted in;
    // f_bsize is only a hint about preferred I/O size.
    const uint64_t blockSize = filesystem.f_frsize != 0 ? filesystem.f_frsize : filesystem.f_bsize;
    return computeDiskStats(blockSize, filesystem.f_blocks, filesystem.f_bfree,
                            filesystem.f_bavail);
}

}  // namespace sysmon
