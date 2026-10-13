#include "settings.h"

#include <strings.h>

#include <fstream>
#include <map>

#include "runtime.h"

namespace settings {

namespace {

Settings s;

// GOG launcher defaults (motoracer.ini as shipped).
const char *kDefaultArgs =
    "-DoRealLock -OpaqueBlackLevel3 -TextureSquareOnly -Pal16 -CorrectRatioTextures -NoDither "
    "-TextureVideoMem268435456 -FrameRateMax60";

std::string trim(const std::string &v) {
    size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : v.substr(a, b - a + 1);
}

bool truthy(const std::string &v) { return v == "1" || !strcasecmp(v.c_str(), "true") || !strcasecmp(v.c_str(), "yes"); }

}  // namespace

void load(const std::string &game_dir) {
    s = Settings{};
    s.game_args = kDefaultArgs;
    std::map<std::string, std::map<std::string, std::string>> ini;
    std::ifstream in(game_dir + "/motoracer.ini");
    std::string line, sec;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { sec = trim(line.substr(1, line.find(']') - 1)); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        for (auto &c : sec) c = static_cast<char>(tolower(c));
        for (auto &c : k) c = static_cast<char>(tolower(c));
        ini[sec][k] = v;
    }
    auto get = [&](const char *sec_, const char *key) -> const std::string * {
        auto a = ini.find(sec_);
        if (a == ini.end()) return nullptr;
        auto b = a->second.find(key);
        return b == a->second.end() ? nullptr : &b->second;
    };
    if (auto v = get("settings", "params"); v && !v->empty()) s.game_args = *v;
    if (auto v = get("recomp", "fullscreen")) s.fullscreen = truthy(*v);
    if (auto v = get("recomp", "windowwidth")) s.window_w = std::max(320, atoi(v->c_str()));
    if (auto v = get("recomp", "windowheight")) s.window_h = std::max(240, atoi(v->c_str()));
    if (auto v = get("recomp", "filter")) s.nearest = !strcasecmp(v->c_str(), "nearest");
    if (auto v = get("recomp", "integerscale")) s.integer_scale = truthy(*v);
    if (auto v = get("recomp", "vsync")) s.vsync = truthy(*v);
    if (auto v = get("recomp", "renderer")) s.d3d = strcasecmp(v->c_str(), "software") != 0;
    if (const char *e = getenv("MOTORACER_RENDERER")) s.d3d = strcasecmp(e, "software") != 0;   // dev override
    if (auto v = get("recomp", "renderscale")) s.render_scale = strcasecmp(v->c_str(), "auto") ? atoi(v->c_str()) : 0;
    if (const char *e = getenv("MOTORACER_RENDER_SCALE")) s.render_scale = atoi(e);
    if (auto v = get("recomp", "custommodes")) s.custom_modes = truthy(*v);
    if (auto v = get("recomp", "controllermenus")) s.controller_menus = truthy(*v);
    if (auto v = get("recomp", "triggermode")) {
        if (!strcasecmp(v->c_str(), "axis")) s.trigger_mode = TriggerMode::Axis;
        else if (!strcasecmp(v->c_str(), "buttons")) s.trigger_mode = TriggerMode::Buttons;
    }
    if (auto v = get("recomp", "widescreen")) s.widescreen = strcasecmp(v->c_str(), "off") && strcasecmp(v->c_str(), "0");
    if (s.d3d) {
        // GOG's launcher adds these when its D3D / ZBuffer options are on.
        if (s.game_args.find("-D3D") == std::string::npos) s.game_args += " -D3D";
        if (s.game_args.find("-ZBuffer") == std::string::npos) s.game_args += " -ZBuffer";
        // The game's own readme: without -Alpha the name-entry letters vanish in D3D mode.
        if (s.game_args.find("-Alpha") == std::string::npos) s.game_args += " -Alpha";
    }
}

const Settings &get() { return s; }

}  // namespace settings
