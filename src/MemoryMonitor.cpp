#include "MemoryMonitor.h"

#include "ProcFs.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace sysmon {
namespace {

constexpr const char* kProcMemInfoPath = "/proc/meminfo";
constexpr uint64_t kBytesPerKibibyte = 1024;

// Every /proc/meminfo line is "Key:<spaces><value> kB" (a handful of entries
// carry no unit, but none of the ones read here).
std::unordered_map<std::string, uint64_t> parseKibibyteFields(const std::string& contents) {
    std::unordered_map<std::string, uint64_t> fields;
    std::istringstream stream(contents);
    std::string line;

    while (std::getline(stream, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, colon);
        std::istringstream valueStream(line.substr(colon + 1));
        uint64_t kibibytes = 0;
        if (valueStream >> kibibytes) {
            fields.emplace(key, kibibytes);
        }
    }
    return fields;
}

uint64_t lookupOrZero(const std::unordered_map<std::string, uint64_t>& fields,
                      const std::string& key) {
    const auto entry = fields.find(key);
    return entry == fields.end() ? 0 : entry->second;
}

}  // namespace

std::optional<MemoryStats> parseMemInfo(const std::string& contents) {
    const auto fields = parseKibibyteFields(contents);

    const uint64_t totalKib = lookupOrZero(fields, "MemTotal");
    if (totalKib == 0) {
        return std::nullopt;
    }

    uint64_t availableKib = lookupOrZero(fields, "MemAvailable");
    if (fields.find("MemAvailable") == fields.end()) {
        availableKib = lookupOrZero(fields, "MemFree") + lookupOrZero(fields, "Buffers") +
                       lookupOrZero(fields, "Cached") + lookupOrZero(fields, "SReclaimable");
    }
    availableKib = std::min(availableKib, totalKib);

    MemoryStats stats;
    stats.totalBytes = totalKib * kBytesPerKibibyte;
    stats.availableBytes = availableKib * kBytesPerKibibyte;
    stats.usedBytes = stats.totalBytes - stats.availableBytes;
    stats.usagePercent = 100.0 * static_cast<double>(stats.usedBytes) /
                         static_cast<double>(stats.totalBytes);
    return stats;
}

std::optional<MemoryStats> MemoryMonitor::sample() const {
    const auto contents = procfs::readFile(kProcMemInfoPath);
    if (!contents) {
        return std::nullopt;
    }
    return parseMemInfo(*contents);
}

}  // namespace sysmon
