#pragma once

#include "CpuMonitor.h"
#include "DiskMonitor.h"
#include "MemoryMonitor.h"
#include "ProcessMonitor.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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

    std::optional<std::vector<ProcessInfo>> processes(size_t limit = ProcessMonitor::kDefaultLimit) {
        return processMonitor_.sample(limit);
    }

    Snapshot snapshot();

private:
    // CpuMonitor and ProcessMonitor keep the previous sample and guard it with
    // their own mutex, so SystemMonitor needs no locking of its own.
    CpuMonitor cpuMonitor_;
    MemoryMonitor memoryMonitor_;
    DiskMonitor diskMonitor_;
    ProcessMonitor processMonitor_;
};

}  // namespace sysmon
