#include "ProcessMonitor.h"

#include "CpuMonitor.h"
#include "ProcFs.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace sysmon {
namespace {

constexpr const char* kProcPath = "/proc";
constexpr const char* kProcStatPath = "/proc/stat";
constexpr uint64_t kBytesPerKibibyte = 1024;

// Zero-based offsets into the fields that follow comm in /proc/[pid]/stat.
// The file's own numbering starts at 1 (pid) and comm is 2, so field N lives at
// offset N - 3.
constexpr size_t kUserTimeOffset = 11;   // field 14
constexpr size_t kSystemTimeOffset = 12; // field 15
constexpr size_t kStartTimeOffset = 19;  // field 22
constexpr size_t kRssOffset = 21;        // field 24, in pages

bool isAllDigits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return std::isdigit(c) != 0;
           });
}

uint64_t pageSizeBytes() {
    static const uint64_t pageSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return pageSize;
}

std::optional<uint64_t> readSystemCpuJiffies() {
    const auto contents = procfs::readFile(kProcStatPath);
    if (!contents) {
        return std::nullopt;
    }
    const auto snapshot = parseProcStat(*contents);
    if (!snapshot) {
        return std::nullopt;
    }
    return snapshot->aggregate.totalTime;
}

// Reads one process, returning nullopt if it disappeared or is inaccessible.
std::optional<ProcessStat> readProcessStat(int pid) {
    const std::string base = std::string(kProcPath) + "/" + std::to_string(pid);
    const auto statContents = procfs::readFile(base + "/stat");
    if (!statContents) {
        return std::nullopt;
    }

    std::optional<ProcessStat> stat = parseProcessStat(*statContents);
    if (!stat) {
        return std::nullopt;
    }
    return stat;
}

uint64_t readProcessMemoryBytes(int pid, const ProcessStat& stat) {
    const std::string statusPath = std::string(kProcPath) + "/" + std::to_string(pid) + "/status";
    if (const auto contents = procfs::readFile(statusPath)) {
        if (const auto residentBytes = parseResidentMemoryBytes(*contents)) {
            return *residentBytes;
        }
    }
    return stat.residentPages * pageSizeBytes();
}

}  // namespace

std::optional<ProcessStat> parseProcessStat(const std::string& contents) {
    const size_t nameOpen = contents.find('(');
    const size_t nameClose = contents.rfind(')');
    if (nameOpen == std::string::npos || nameClose == std::string::npos || nameClose < nameOpen) {
        return std::nullopt;
    }

    ProcessStat stat;
    try {
        stat.pid = std::stoi(contents.substr(0, nameOpen));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    stat.name = contents.substr(nameOpen + 1, nameClose - nameOpen - 1);

    std::istringstream stream(contents.substr(nameClose + 1));
    std::vector<std::string> fields;
    std::string field;
    while (fields.size() <= kRssOffset && stream >> field) {
        fields.push_back(field);
    }
    if (fields.size() <= kStartTimeOffset) {
        return std::nullopt;
    }

    try {
        stat.cpuJiffies = std::stoull(fields[kUserTimeOffset]) +
                          std::stoull(fields[kSystemTimeOffset]);
        stat.startTimeJiffies = std::stoull(fields[kStartTimeOffset]);
        if (fields.size() > kRssOffset) {
            stat.residentPages = std::stoull(fields[kRssOffset]);
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return stat;
}

std::optional<uint64_t> parseResidentMemoryBytes(const std::string& contents) {
    std::istringstream stream(contents);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind("VmRSS:", 0) != 0) {
            continue;
        }
        std::istringstream valueStream(line.substr(std::string("VmRSS:").size()));
        uint64_t kibibytes = 0;
        if (valueStream >> kibibytes) {
            return kibibytes * kBytesPerKibibyte;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> computeProcessCpuPercent(uint64_t processJiffiesDelta,
                                               uint64_t systemJiffiesDelta) {
    if (systemJiffiesDelta == 0) {
        return std::nullopt;
    }
    const double percent = 100.0 * static_cast<double>(processJiffiesDelta) /
                           static_cast<double>(systemJiffiesDelta);
    return std::clamp(percent, 0.0, 100.0);
}

std::optional<std::vector<ProcessInfo>> ProcessMonitor::sample(size_t limit) {
    const std::lock_guard<std::mutex> lock(mutex_);

    const std::optional<uint64_t> systemJiffies = readSystemCpuJiffies();
    if (!systemJiffies) {
        return std::nullopt;
    }
    const uint64_t systemJiffiesDelta =
        *systemJiffies > previousSystemJiffies_ ? *systemJiffies - previousSystemJiffies_ : 0;

    // directory_iterator is stepped with the error_code overload: entries under
    // /proc come and go while it is being walked, and the throwing overload
    // would turn an exiting process into an exception.
    std::error_code ec;
    fs::directory_iterator entry(kProcPath, ec);
    if (ec) {
        return std::nullopt;
    }

    std::vector<ProcessInfo> processes;
    std::unordered_map<int, PreviousSample> currentSamples;

    for (const fs::directory_iterator end; entry != end; entry.increment(ec)) {
        if (ec) {
            break;  // report what was collected so far rather than nothing
        }

        const std::string name = entry->path().filename().string();
        if (!isAllDigits(name)) {
            continue;  // /proc also holds meminfo, stat, self, ...
        }

        int pid = 0;
        try {
            pid = std::stoi(name);
        } catch (const std::exception&) {
            continue;  // not a PID we can represent
        }

        const std::optional<ProcessStat> stat = readProcessStat(pid);
        if (!stat) {
            continue;
        }

        ProcessInfo info;
        info.pid = pid;
        info.name = stat->name;
        info.memoryBytes = readProcessMemoryBytes(pid, *stat);

        const auto previous = previousSamples_.find(pid);
        // A different start time means the PID was recycled, so the old counter
        // belongs to a different process and has to be discarded.
        if (previous != previousSamples_.end() &&
            previous->second.startTimeJiffies == stat->startTimeJiffies &&
            stat->cpuJiffies >= previous->second.cpuJiffies) {
            const uint64_t processDelta = stat->cpuJiffies - previous->second.cpuJiffies;
            info.cpuPercent = computeProcessCpuPercent(processDelta, systemJiffiesDelta).value_or(0.0);
        }

        currentSamples.emplace(pid, PreviousSample{stat->cpuJiffies, stat->startTimeJiffies});
        processes.push_back(std::move(info));
    }

    // Replacing the map wholesale drops the processes that have since exited.
    previousSamples_ = std::move(currentSamples);
    previousSystemJiffies_ = *systemJiffies;

    std::sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        if (a.cpuPercent != b.cpuPercent) {
            return a.cpuPercent > b.cpuPercent;
        }
        return a.memoryBytes > b.memoryBytes;
    });
    if (processes.size() > limit) {
        processes.resize(limit);
    }
    return processes;
}

}  // namespace sysmon
