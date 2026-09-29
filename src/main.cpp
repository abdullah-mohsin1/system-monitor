#include <httplib.h>
#include <nlohmann/json.hpp>

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

void registerRoutes(httplib::Server& server) {
    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"status", "ok"}}.dump(), "application/json");
    });
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 1;
    }

    httplib::Server server;
    registerRoutes(server);

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
    std::cout << "System Monitor running on http://localhost:" << options.port
              << std::endl;
    if (!server.listen(kBindAddress, options.port)) {
        std::cerr << "Failed to bind port " << options.port << "\n";
        return 1;
    }
    return 0;
}
