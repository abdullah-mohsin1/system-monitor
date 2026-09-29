#pragma once

#include "CpuMonitor.h"
#include "DiskMonitor.h"
#include "MemoryMonitor.h"

#include <optional>
#include <string>

namespace sysmon {

// Parses /proc/uptime, whose first field is seconds since boot with two
// decimals. Returns nullopt if the field is missing or negative.
std::optional<double> parseUptimeSeconds(const std::string& contents);

// Owns the individual monitors and reads them together for /api/system.
// Any metric that cannot be read comes back empty rather than failing the
// whole snapshot.
class SystemMonitor {
public:
    struct Snapshot {
        std::optional<CpuStats> cpu;
        std::optional<MemoryStats> memory;
        std::optional<DiskStats> disk;
        std::optional<double> uptimeSeconds;
    };

    std::optional<CpuStats> cpu() { return cpuMonitor_.sample(); }
    std::optional<MemoryStats> memory() const { return memoryMonitor_.sample(); }
    std::optional<DiskStats> disk() const { return diskMonitor_.sample(); }
    std::optional<double> uptimeSeconds() const;

    Snapshot snapshot();

private:
    CpuMonitor cpuMonitor_;  // stateful and thread-safe in its own right
    MemoryMonitor memoryMonitor_;
    DiskMonitor diskMonitor_;
};

}  // namespace sysmon
