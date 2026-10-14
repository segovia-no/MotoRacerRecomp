// macOS folders: ~/Library/Application Support/MotoRacerRecomp, ~/Library/Logs/MotoRacerRecomp.
#include <cstdlib>

#include "platform.h"

namespace plat {

namespace {

std::string home() {
    const char *h = getenv("HOME");
    return h ? h : "";
}

class MacPaths final : public HostPaths {
public:
    std::string data_dir() override { return home().empty() ? "" : home() + "/Library/Application Support/MotoRacerRecomp"; }
    std::string log_dir() override { return home().empty() ? "" : home() + "/Library/Logs/MotoRacerRecomp"; }
};

}  // namespace

HostPaths &os_paths() {
    static MacPaths paths;
    return paths;
}

}  // namespace plat
