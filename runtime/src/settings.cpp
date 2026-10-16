#include "settings.h"

#include <strings.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

#include "platform/platform.h"

#include "runtime.h"

namespace settings {

namespace {

Settings s;
std::string ini_path;

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
    ini_path = game_dir + "/motoracer.ini";
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
    if (auto v = get("recomp", "hidpi")) s.hidpi = !strcasecmp(v->c_str(), "auto") ? -1 : truthy(*v) ? 1 : 0;
    if (const char *e = getenv("MOTORACER_HIDPI")) s.hidpi = !strcasecmp(e, "auto") ? -1 : truthy(e) ? 1 : 0;   // dev override
    if (const char *e = getenv("MOTORACER_FULLSCREEN")) s.fullscreen = truthy(e);   // dev override (E2E: windowed)
    if (auto v = get("recomp", "windowwidth")) s.window_w = std::max(320, atoi(v->c_str()));
    if (auto v = get("recomp", "windowheight")) s.window_h = std::max(240, atoi(v->c_str()));
    if (auto v = get("recomp", "filter")) s.nearest = !strcasecmp(v->c_str(), "nearest");
    if (auto v = get("recomp", "integerscale")) s.integer_scale = truthy(*v);
    if (auto v = get("recomp", "vsync")) s.vsync = truthy(*v);
    auto frame_rate = [](const std::string &v) {
        if (!strcasecmp(v.c_str(), "unlimited")) return -1;
        if (!strcasecmp(v.c_str(), "display")) return -2;
        return std::max(0, atoi(v.c_str()));   // "game" -> 0
    };
    if (auto v = get("recomp", "framerate")) s.frame_rate = frame_rate(*v);
    if (const char *e = getenv("MOTORACER_FRAMERATE")) s.frame_rate = frame_rate(e);   // dev override
    if (s.frame_rate == -1) s.vsync = false;
    if (const char *e = getenv("MOTORACER_VSYNC")) s.vsync = truthy(e);   // dev override
    {
        // The runtime paces frames (host_video.cpp), so the frame rate can change while the game
        // runs. The game's own limiter busy-waits and only takes 21..98; it is turned off
        // (-FrameRateMax0) and its value kept for FrameRate=game (30 without the argument).
        size_t p = s.game_args.find("-FrameRateMax");
        if (p != std::string::npos) {
            size_t end = s.game_args.find(' ', p);
            int n = atoi(s.game_args.c_str() + p + 13);
            s.game_frame_cap = n > 20 && n < 99 ? n : n == 0 ? 0 : 30;
            s.game_args.erase(p, end == std::string::npos ? std::string::npos : end - p);
        }
        s.game_args += " -FrameRateMax0";
    }
    if (auto v = get("recomp", "renderer")) s.d3d = strcasecmp(v->c_str(), "software") != 0;
    if (const char *e = getenv("MOTORACER_RENDERER")) s.d3d = strcasecmp(e, "software") != 0;   // dev override
    if (auto v = get("recomp", "renderscale")) s.render_scale = strcasecmp(v->c_str(), "auto") ? atoi(v->c_str()) : 0;
    if (const char *e = getenv("MOTORACER_RENDER_SCALE")) s.render_scale = atoi(e);
    if (auto v = get("recomp", "custommodes")) s.custom_modes = truthy(*v);
    if (auto v = get("recomp", "drawdistance")) s.draw_distance = static_cast<float>(atof(v->c_str()));
    if (const char *e = getenv("MOTORACER_DRAW_DISTANCE")) s.draw_distance = static_cast<float>(atof(e));   // dev override
    if (auto v = get("recomp", "hudscale")) s.hud_scale = strcasecmp(v->c_str(), "auto") ? static_cast<float>(atof(v->c_str())) : 0.f;
    if (auto v = get("recomp", "controllerpreset")) s.controller_preset = truthy(*v);
    if (auto v = get("recomp", "controllermenus")) s.controller_menus = truthy(*v);
    if (auto v = get("recomp", "triggermode")) {
        if (!strcasecmp(v->c_str(), "axis")) s.trigger_mode = TriggerMode::Axis;
        else if (!strcasecmp(v->c_str(), "buttons")) s.trigger_mode = TriggerMode::Buttons;
    }
    if (auto v = get("recomp", "widescreen")) s.widescreen = strcasecmp(v->c_str(), "off") && strcasecmp(v->c_str(), "0");
    if (const char *e = getenv("MOTORACER_EXTRA_ARGS")) s.game_args += std::string(" ") + e;   // dev
    if (s.d3d) {
        // GOG's launcher adds these when its D3D / ZBuffer options are on.
        if (s.game_args.find("-D3D") == std::string::npos) s.game_args += " -D3D";
        if (s.game_args.find("-ZBuffer") == std::string::npos) s.game_args += " -ZBuffer";
        // The game's own readme: without -Alpha the name-entry letters vanish in D3D mode.
        if (s.game_args.find("-Alpha") == std::string::npos) s.game_args += " -Alpha";
    }
}

const Settings &get() { return s; }
Settings &edit() { return s; }

void save(const std::string &key, const std::string &value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(ini_path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    auto lower = [](std::string v) { for (auto &c : v) c = static_cast<char>(tolower(c)); return v; };
    size_t sec_end = lines.size(), insert = lines.size();
    bool in_sec = false, found_sec = false, done = false;
    for (size_t i = 0; i < lines.size() && !done; i++) {
        std::string t = trim(lines[i]);
        if (!t.empty() && t[0] == '[') {
            if (in_sec) { sec_end = i; break; }
            in_sec = lower(trim(t.substr(1, t.find(']') - 1))) == "recomp";
            found_sec |= in_sec;
            continue;
        }
        if (!in_sec) continue;
        if (!t.empty()) insert = i + 1;
        size_t eq = t.find('=');
        if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == lower(key)) {
            lines[i] = key + "=" + value;
            done = true;
        }
    }
    if (!done) {
        if (!found_sec) {
            lines.push_back("[Recomp]");
            insert = lines.size();
        } else if (insert > sec_end) {
            insert = sec_end;
        }
        lines.insert(lines.begin() + static_cast<long>(insert), key + "=" + value);
    }
    std::string tmp = ini_path + ".new";
    {
        std::ofstream out(tmp, std::ios::binary);
        for (auto &l : lines) out << l << "\r\n";   // the GOG file uses CRLF
    }
    if (!plat::host().fs.rename(tmp, ini_path)) rt::log("settings: cannot write %s\n", ini_path.c_str());
}

}  // namespace settings
