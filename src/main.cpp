#include "SystemMonitor.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr int kDefaultPort = 8080;
constexpr const char* kBindAddress = "0.0.0.0";
constexpr size_t kMaxProcessLimit = 500;

struct Options {
    int port = kDefaultPort;
    std::string clientDir;  // empty means "probe the usual locations"
};

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [--port N] [--client-dir PATH]\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = (i + 1) < argc;

        if ((arg == "--port" || arg == "-p") && hasValue) {
            options.port = std::atoi(argv[++i]);
            if (options.port <= 0 || options.port > 65535) {
                std::cerr << "Invalid port: " << argv[i] << "\n";
                return false;
            }
        } else if (arg == "--client-dir" && hasValue) {
            options.clientDir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        } else {
            std::cerr << "Unrecognised argument: " << arg << "\n";
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

// The dashboard is served from disk rather than embedded, so look for it
// relative to both the working directory and the executable. That covers
// running from the build directory and from the repository root.
std::string locateClientDir(const char* programPath) {
    std::vector<fs::path> candidates{"client", "../client"};

    std::error_code ec;
    const fs::path exePath = fs::weakly_canonical(fs::path(programPath), ec);
    if (!ec && exePath.has_parent_path()) {
        const fs::path exeDir = exePath.parent_path();
        candidates.push_back(exeDir / "client");
        candidates.push_back(exeDir / ".." / "client");
    }

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate / "index.html", ec)) {
            return fs::weakly_canonical(candidate, ec).string();
        }
    }
    return {};
}

// Percentages are rounded before serialisation; a dashboard has no use for the
// full double and it keeps the JSON readable.
double roundToTenth(double value) { return std::round(value * 10.0) / 10.0; }

json toJson(const sysmon::CpuStats& cpu) {
    json perCore = json::array();
    for (const double usage : cpu.perCoreUsagePercent) {
        perCore.push_back(roundToTenth(usage));
    }
    return json{{"usagePercent", roundToTenth(cpu.usagePercent)},
                {"coreCount", cpu.coreCount},
                {"perCoreUsagePercent", perCore}};
}

json toJson(const sysmon::MemoryStats& memory) {
    return json{{"totalBytes", memory.totalBytes},
                {"usedBytes", memory.usedBytes},
                {"availableBytes", memory.availableBytes},
                {"usagePercent", roundToTenth(memory.usagePercent)}};
}

json toJson(const sysmon::ProcessInfo& process) {
    return json{{"pid", process.pid},
                {"name", process.name},
                {"cpuPercent", roundToTenth(process.cpuPercent)},
                {"memoryBytes", process.memoryBytes}};
}

json toJson(const sysmon::DiskStats& disk) {
    return json{{"totalBytes", disk.totalBytes},
                {"usedBytes", disk.usedBytes},
                {"freeBytes", disk.freeBytes},
                {"usagePercent", roundToTenth(disk.usagePercent)}};
}

void respondJson(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void respondUnavailable(httplib::Response& res, const std::string& metric) {
    respondJson(res, json{{"error", metric + " metrics are unavailable on this system"}}, 503);
}

// Reads an optional positive "limit" query parameter, clamped so a caller
// cannot ask the server to serialise every process on the machine.
size_t parseLimit(const httplib::Request& req, size_t fallback) {
    if (!req.has_param("limit")) {
        return fallback;
    }
    const long requested = std::strtol(req.get_param_value("limit").c_str(), nullptr, 10);
    if (requested <= 0) {
        return fallback;
    }
    return std::min<size_t>(static_cast<size_t>(requested), kMaxProcessLimit);
}

void registerRoutes(httplib::Server& server, sysmon::SystemMonitor& monitor) {
    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        respondJson(res, json{{"status", "ok"}});
    });

    server.Get("/api/cpu", [&monitor](const httplib::Request&, httplib::Response& res) {
        if (const auto cpu = monitor.cpu()) {
            respondJson(res, toJson(*cpu));
        } else {
            respondUnavailable(res, "CPU");
        }
    });

    server.Get("/api/memory", [&monitor](const httplib::Request&, httplib::Response& res) {
        if (const auto memory = monitor.memory()) {
            respondJson(res, toJson(*memory));
        } else {
            respondUnavailable(res, "Memory");
        }
    });

    server.Get("/api/disk", [&monitor](const httplib::Request&, httplib::Response& res) {
        if (const auto disk = monitor.disk()) {
            respondJson(res, toJson(*disk));
        } else {
            respondUnavailable(res, "Disk");
        }
    });

    server.Get("/api/processes", [&monitor](const httplib::Request& req, httplib::Response& res) {
        const size_t limit = parseLimit(req, sysmon::ProcessMonitor::kDefaultLimit);
        const auto processes = monitor.processes(limit);
        if (!processes) {
            respondUnavailable(res, "Process");
            return;
        }

        json body = json::array();
        for (const sysmon::ProcessInfo& process : *processes) {
            body.push_back(toJson(process));
        }
        respondJson(res, body);
    });

    server.Get("/api/system", [&monitor](const httplib::Request&, httplib::Response& res) {
        const sysmon::SystemMonitor::Snapshot snapshot = monitor.snapshot();
        if (!snapshot.cpu && !snapshot.memory && !snapshot.disk && !snapshot.uptimeSeconds) {
            respondUnavailable(res, "System");
            return;
        }

        json body;
        body["cpu"] = snapshot.cpu ? toJson(*snapshot.cpu) : json(nullptr);
        body["memory"] = snapshot.memory ? toJson(*snapshot.memory) : json(nullptr);
        body["disk"] = snapshot.disk ? toJson(*snapshot.disk) : json(nullptr);
        body["uptimeSeconds"] =
            snapshot.uptimeSeconds ? json(std::llround(*snapshot.uptimeSeconds)) : json(nullptr);
        respondJson(res, body);
    });
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 1;
    }

    sysmon::SystemMonitor monitor;
    httplib::Server server;
    registerRoutes(server, monitor);

    const std::string clientDir =
        options.clientDir.empty() ? locateClientDir(argv[0]) : options.clientDir;
    if (clientDir.empty() || !server.set_mount_point("/", clientDir)) {
        std::cerr << "Warning: dashboard files not found; serving the API only.\n"
                  << "         Pass --client-dir PATH to point at the client directory.\n";
    }

    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            res.status = 500;
            res.set_content(json{{"error", "internal server error"}}.dump(),
                            "application/json");
        });

    // Flushed explicitly so the banner appears immediately when stdout is a pipe.
    std::cout << "System Monitor running on http://localhost:" << options.port << std::endl;
    if (!server.listen(kBindAddress, options.port)) {
        std::cerr << "Failed to bind port " << options.port << "\n";
        return 1;
    }
    return 0;
}
