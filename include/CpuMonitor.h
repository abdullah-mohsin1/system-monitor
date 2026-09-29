#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sysmon {

struct CpuStats {
    double usagePercent = 0.0;
    int coreCount = 0;
    std::vector<double> perCoreUsagePercent;
};

// Cumulative jiffie counters taken from a single "cpu"/"cpuN" line of /proc/stat.
struct CpuTimes {
    uint64_t idleTime = 0;   // idle + iowait
    uint64_t totalTime = 0;  // every accounted state, idle included
};

struct CpuSnapshot {
    CpuTimes aggregate;
    std::vector<CpuTimes> perCore;  // index == core number
};

// Parses one "cpu"/"cpuN" line. Returns nullopt if the line is not a CPU line
// or carries fewer fields than the kernel is documented to provide.
std::optional<CpuTimes> parseCpuLine(const std::string& line);

// Parses the CPU lines of /proc/stat, ignoring the rest of the file.
std::optional<CpuSnapshot> parseProcStat(const std::string& contents);

// Share of non-idle time between two cumulative samples, as a percentage.
// Returns nullopt when no time elapsed or the counters moved backwards.
std::optional<double> computeUsagePercent(const CpuTimes& previous, const CpuTimes& current);

// Reads /proc/stat and reports usage as the delta since the previous read.
// The baseline is taken on construction, so the first sample covers the time
// between start-up and that call. Safe to call from multiple threads.
class CpuMonitor {
public:
    CpuMonitor();

    std::optional<CpuStats> sample();

private:
    mutable std::mutex mutex_;
    std::optional<CpuSnapshot> previous_;
    std::optional<CpuStats> lastStats_;
    std::chrono::steady_clock::time_point lastSampleTime_{};
};

}  // namespace sysmon
