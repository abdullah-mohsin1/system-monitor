#include "SystemMonitor.h"

#include "ProcFs.h"

#include <sys/utsname.h>

#include <sstream>

namespace sysmon {
namespace {

constexpr const char* kProcUptimePath = "/proc/uptime";

std::optional<HostInfo> readHostInfo() {
    struct utsname system{};
    if (uname(&system) != 0) {
        return std::nullopt;
    }
    return HostInfo{system.nodename, system.release, system.machine};
}

}  // namespace

SystemMonitor::SystemMonitor() : host_(readHostInfo()) {}

std::optional<double> parseUptimeSeconds(const std::string& contents) {
    std::istringstream stream(contents);
    double seconds = 0.0;
    if (!(stream >> seconds) || seconds < 0.0) {
        return std::nullopt;
    }
    return seconds;
}

std::optional<double> SystemMonitor::uptimeSeconds() const {
    const auto contents = procfs::readFile(kProcUptimePath);
    if (!contents) {
        return std::nullopt;
    }
    return parseUptimeSeconds(*contents);
}

SystemMonitor::Snapshot SystemMonitor::snapshot() {
    Snapshot snapshot;
    snapshot.cpu = cpu();
    snapshot.memory = memory();
    snapshot.disk = disk();
    snapshot.uptimeSeconds = uptimeSeconds();
    return snapshot;
}

}  // namespace sysmon
