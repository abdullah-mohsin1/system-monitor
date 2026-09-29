#include "ProcFs.h"

#include <fstream>
#include <sstream>

namespace sysmon::procfs {

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad()) {
        return std::nullopt;
    }
    return contents.str();
}

}  // namespace sysmon::procfs
