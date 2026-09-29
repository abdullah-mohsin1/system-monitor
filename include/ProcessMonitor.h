#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sysmon {

struct ProcessInfo {
    int pid = 0;
    std::string name;
    double cpuPercent = 0.0;  // share of the whole machine, not of one core
    uint64_t memoryBytes = 0;
};

// The subset of /proc/[pid]/stat this program needs.
struct ProcessStat {
    int pid = 0;
    std::string name;
    uint64_t cpuJiffies = 0;        // utime + stime
    uint64_t startTimeJiffies = 0;  // guards against PID reuse between samples
    uint64_t residentPages = 0;     // fallback when VmRSS is unavailable
};

// Parses /proc/[pid]/stat. The comm field is wrapped in parentheses and may
// itself contain spaces and parentheses ("(sd-pam)", "((sd-pam))"), so the
// numeric fields are read from after the *last* closing parenthesis.
std::optional<ProcessStat> parseProcessStat(const std::string& contents);

// Parses VmRSS out of /proc/[pid]/status and converts it to bytes. Kernel
// threads have no VmRSS line, hence the optional.
std::optional<uint64_t> parseResidentMemoryBytes(const std::string& contents);

// CPU share of one process between two samples, as a percentage of total system
// CPU time. Returns nullopt when the system counters did not advance.
std::optional<double> computeProcessCpuPercent(uint64_t processJiffiesDelta,
                                               uint64_t systemJiffiesDelta);

// Walks /proc once per sample and reports the busiest processes.
//
// Processes that vanish mid-scan, or that the current user may not inspect, are
// skipped silently: that is normal on a live system, not an error. Safe to call
// from multiple threads.
class ProcessMonitor {
public:
    static constexpr size_t kDefaultLimit = 50;

    // Returns processes sorted by CPU usage, descending, truncated to `limit`.
    // nullopt means /proc itself could not be read.
    std::optional<std::vector<ProcessInfo>> sample(size_t limit = kDefaultLimit);

private:
    struct PreviousSample {
        uint64_t cpuJiffies = 0;
        uint64_t startTimeJiffies = 0;
    };

    std::mutex mutex_;
    std::unordered_map<int, PreviousSample> previousSamples_;
    uint64_t previousSystemJiffies_ = 0;
};

}  // namespace sysmon
