// Linux / SteamOS folders (XDG base directories):
//   data  $XDG_DATA_HOME/MotoRacerRecomp   (~/.local/share/MotoRacerRecomp)
//   log   $XDG_STATE_HOME/MotoRacerRecomp  (~/.local/state/MotoRacerRecomp)
#include <cstdlib>

#include "platform.h"

namespace plat {

namespace {

std::string xdg(const char *var, const char *fallback_under_home) {
    if (const char *v = getenv(var); v && *v == '/') return v;
    const char *h = getenv("HOME");
    return h ? std::string(h) + "/" + fallback_under_home : "";
}

class XdgPaths final : public HostPaths {
public:
    std::string data_dir() override { return join(xdg("XDG_DATA_HOME", ".local/share")); }
    std::string log_dir() override { return join(xdg("XDG_STATE_HOME", ".local/state")); }

private:
    static std::string join(const std::string &base) { return base.empty() ? "" : base + "/MotoRacerRecomp"; }
};

}  // namespace

HostPaths &os_paths() {
    static XdgPaths paths;
    return paths;
}

}  // namespace plat
