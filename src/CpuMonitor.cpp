#include "CpuMonitor.h"

#include "ProcFs.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>

namespace sysmon {
namespace {

constexpr const char* kProcStatPath = "/proc/stat";

// user, nice, system, idle, iowait, irq, softirq, steal. The trailing guest and
// guest_nice fields are deliberately ignored: the kernel already counts them
// inside user and nice, so adding them would inflate the total on a VM.
constexpr size_t kAccountedFields = 8;
constexpr size_t kMinimumFields = 4;  // through idle

// Two samples closer together than this produce a delta of only a few jiffies,
// which is mostly noise, so the previous result is reused instead.
constexpr auto kMinimumSampleInterval = std::chrono::milliseconds(50);

bool isCoreLabel(const std::string& label) {
    if (label.size() <= 3) {
        return false;
    }
    return std::all_of(label.begin() + 3, label.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace

std::optional<CpuTimes> parseCpuLine(const std::string& line) {
    std::istringstream stream(line);
    std::string label;
    if (!(stream >> label) || label.rfind("cpu", 0) != 0) {
        return std::nullopt;
    }

    std::vector<uint64_t> fields;
    uint64_t value = 0;
    while (fields.size() < kAccountedFields && stream >> value) {
        fields.push_back(value);
    }
    if (fields.size() < kMinimumFields) {
        return std::nullopt;
    }

    CpuTimes times;
    times.idleTime = fields[3] + (fields.size() > 4 ? fields[4] : 0);
    for (const uint64_t field : fields) {
        times.totalTime += field;
    }
    return times;
}

std::optional<CpuSnapshot> parseProcStat(const std::string& contents) {
    std::istringstream stream(contents);
    std::string line;
    CpuSnapshot snapshot;
    bool haveAggregate = false;

    while (std::getline(stream, line)) {
        if (line.rfind("cpu", 0) != 0) {
            continue;  // intr, ctxt, btime, ... are of no interest here
        }

        const std::string label = line.substr(0, line.find(' '));
        const std::optional<CpuTimes> times = parseCpuLine(line);
        if (!times) {
            continue;
        }

        if (label == "cpu") {
            snapshot.aggregate = *times;
            haveAggregate = true;
        } else if (isCoreLabel(label)) {
            snapshot.perCore.push_back(*times);
        }
    }

    if (!haveAggregate) {
        return std::nullopt;
    }
    return snapshot;
}

std::optional<double> computeUsagePercent(const CpuTimes& previous, const CpuTimes& current) {
    if (current.totalTime <= previous.totalTime) {
        return std::nullopt;  // no tick elapsed, or counters were reset
    }

    const uint64_t totalDelta = current.totalTime - previous.totalTime;
    const uint64_t idleDelta =
        current.idleTime > previous.idleTime ? current.idleTime - previous.idleTime : 0;
    const double busyDelta = static_cast<double>(totalDelta) - static_cast<double>(idleDelta);

    const double percent = 100.0 * busyDelta / static_cast<double>(totalDelta);
    return std::clamp(percent, 0.0, 100.0);
}

CpuMonitor::CpuMonitor() {
    if (const auto contents = procfs::readFile(kProcStatPath)) {
        previous_ = parseProcStat(*contents);
        lastSampleTime_ = std::chrono::steady_clock::now();
    }
}

std::optional<CpuStats> CpuMonitor::sample() {
    const std::lock_guard<std::mutex> lock(mutex_);

    const auto now = std::chrono::steady_clock::now();
    if (lastStats_ && (now - lastSampleTime_) < kMinimumSampleInterval) {
        return lastStats_;
    }

    const auto contents = procfs::readFile(kProcStatPath);
    if (!contents) {
        return std::nullopt;
    }
    const std::optional<CpuSnapshot> current = parseProcStat(*contents);
    if (!current) {
        return std::nullopt;
    }

    CpuStats stats;
    stats.coreCount = static_cast<int>(current->perCore.size());

    if (previous_) {
        const std::optional<double> aggregate =
            computeUsagePercent(previous_->aggregate, current->aggregate);
        // A nullopt aggregate means the clock had not ticked yet; keeping the
        // previous reading is more useful than reporting a sudden zero.
        stats.usagePercent = aggregate.value_or(lastStats_ ? lastStats_->usagePercent : 0.0);

        const size_t cores = std::min(previous_->perCore.size(), current->perCore.size());
        stats.perCoreUsagePercent.reserve(cores);
        for (size_t core = 0; core < cores; ++core) {
            const std::optional<double> usage =
                computeUsagePercent(previous_->perCore[core], current->perCore[core]);
            const double fallback =
                (lastStats_ && core < lastStats_->perCoreUsagePercent.size())
                    ? lastStats_->perCoreUsagePercent[core]
                    : 0.0;
            stats.perCoreUsagePercent.push_back(usage.value_or(fallback));
        }
    } else {
        stats.perCoreUsagePercent.assign(current->perCore.size(), 0.0);
    }

    previous_ = current;
    lastStats_ = stats;
    lastSampleTime_ = now;
    return stats;
}

}  // namespace sysmon
