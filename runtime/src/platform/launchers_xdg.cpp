// Linux packages: the desktop menu entry, the Steam library entry and that entry's controller layout,
// made on every start for the program in its MotoRacer folder, without asking (docs/notes/28-easy-install.md).
//   menu     $XDG_DATA_HOME/applications/motoracer-recomp.desktop (the name setup_linux.sh uses), the icon
//            from the GOG installer's .ico (its PNG image) as <folder>/motoracer.png; rewritten when the
//            folder moved
//   Steam    added the way SteamOS's own steamos-add-to-steam does it (steam://addnonsteamgame/<desktop
//            file>) when Steam is running, the game was not started by Steam and no shortcut points at
//            this program; once per folder (<folder>/.steam-shortcut), so a shortcut the player removed
//            is not added again
//   layout   Steam gives a new non-Steam game the "Keyboard (WASD) and Mouse" layout: the Deck's controls
//            then type keys instead of being a gamepad (double presses in the menus, no analog throttle).
//            Each account's configset_controller_neptune.vdf (Steam Deck controls; the entry is keyed by
//            the game's name in lower case) gets "Gamepad with Joystick Trackpad" for "moto racer" when it
//            has no entry or still the keyboard template; a layout the player chose is kept.
#include <dirent.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "platform/platform.h"
#include "runtime.h"

namespace plat {

namespace {
namespace fs = std::filesystem;

constexpr const char *kName = "Moto Racer";
constexpr const char *kLayoutKey = "moto racer";   // Steam's key: the shortcut's name in lower case
constexpr const char *kKeyboardTemplate = "controller_neptune_wasd.vdf";
constexpr const char *kGamepadTemplate = "controller_neptune_gamepad_joystick.vdf";

std::string read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// writes through a temporary file; false if unchanged or on failure
bool write_if_changed(const fs::path &p, const std::string &content) {
    std::error_code ec;
    if (fs::exists(p, ec) && read_file(p) == content) return false;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << content;
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string home() {
    const char *h = getenv("HOME");
    return h ? h : "";
}

std::string data_home() {
    const char *x = getenv("XDG_DATA_HOME");
    if (x && *x) return x;
    return home().empty() ? "" : home() + "/.local/share";
}

// the largest PNG image in a Windows .ico (Vista-style icons keep them as PNG)
bool ico_to_png(const fs::path &ico, const fs::path &png) {
    const std::string d = read_file(ico);
    auto u16 = [&](size_t o) { return o + 2 <= d.size() ? uint32_t(uint8_t(d[o]) | uint8_t(d[o + 1]) << 8) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= d.size() ? u16(o) | u16(o + 2) << 16 : 0u; };
    const uint32_t count = u16(4);
    uint32_t best_w = 0, best_off = 0, best_size = 0;
    for (uint32_t i = 0; i < count; i++) {
        const size_t e = 6 + 16 * i;
        if (e + 16 > d.size()) break;
        const uint32_t w = uint8_t(d[e]) ? uint8_t(d[e]) : 256, size = u32(e + 8), off = u32(e + 12);
        if (off + size > d.size() || size < 8 || d.compare(off, 4, "\x89PNG") != 0) continue;
        if (w > best_w) best_w = w, best_off = off, best_size = size;
    }
    if (!best_w) return false;
    return write_if_changed(png, d.substr(best_off, best_size)) || fs::exists(png);
}

// desktop entry values: Exec quoting (spec: \ " ` $ inside double quotes), then the string escapes
std::string desktop_string(const std::string &s) {
    std::string r;
    for (char c : s) r += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return r;
}
std::string desktop_exec(const std::string &program) {
    std::string q = "\"";
    for (char c : program) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') q += '\\';
        if (c == '%') q += '%';
        q += c;
    }
    return desktop_string(q + "\"");
}

std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += static_cast<char>(c);
        else r += '%', r += hex[c >> 4], r += hex[c & 15];
    }
    return r;
}

bool process_running(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    while (dirent *e = readdir(d)) {
        if (!isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
        std::string comm = read_file(std::string("/proc/") + e->d_name + "/comm");
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
        if (comm == name) { found = true; break; }
    }
    closedir(d);
    return found;
}

std::vector<fs::path> subdirs(const fs::path &dir) {
    std::vector<fs::path> r;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) r.push_back(it->path());
    return r;
}

// A shortcut in any account's shortcuts.vdf (binary; the program's path is stored as text) starts it.
bool in_steam(const fs::path &steam, const std::string &program) {
    for (const fs::path &user : subdirs(steam / "userdata")) {
        const std::string vdf = read_file(user / "config" / "shortcuts.vdf");
        if (vdf.find(program) != std::string::npos) return true;
    }
    return false;
}

// The "moto racer" entry of one configset_controller_neptune.vdf (text VDF: "controller_config" { "key"
// { "template" "..." } ... }); returns the new text, or "" when nothing changes.
std::string with_gamepad_layout(const std::string &vdf) {
    const std::string entry = std::string("\t\"") + kLayoutKey + "\"\n\t{\n\t\t\"template\"\t\t\"" + kGamepadTemplate + "\"\n\t}\n";
    if (vdf.empty()) return std::string("\"controller_config\"\n{\n") + entry + "}\n";
    const size_t key = vdf.find(std::string("\"") + kLayoutKey + "\"");
    if (key != std::string::npos) {
        const size_t open = vdf.find('{', key), close = open == std::string::npos ? open : vdf.find('}', open);
        if (close == std::string::npos) return "";
        const std::string block = vdf.substr(open, close - open);
        const size_t t = block.find(kKeyboardTemplate);
        if (t == std::string::npos) return "";   // the player's layout (or ours already)
        std::string r = vdf;
        r.replace(open + t, strlen(kKeyboardTemplate), kGamepadTemplate);
        return r;
    }
    const size_t last = vdf.rfind('}');
    if (last == std::string::npos) return "";
    return vdf.substr(0, last) + entry + vdf.substr(last);
}

void set_layouts(const fs::path &steam) {
    for (const fs::path &account : subdirs(steam / "steamapps" / "common" / "Steam Controller Configs")) {
        const fs::path dir = account / "config", file = dir / "configset_controller_neptune.vdf";
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        const std::string old = fs::exists(file, ec) ? read_file(file) : std::string();
        const std::string updated = with_gamepad_layout(old);
        if (updated.empty()) continue;
        const fs::path bak = file.string() + ".motoracer-bak";
        if (!old.empty() && !fs::exists(bak, ec)) write_if_changed(bak, old);
        if (write_if_changed(file, updated))
            rt::log("launchers: Steam controller layout for \"%s\": Gamepad with Joystick Trackpad (%s)\n", kName,
                file.string().c_str());
    }
}

}  // namespace

void register_launchers(const std::string &program_dir, const std::string &game_dir) {
    const std::string data = data_home();
    std::error_code ec;
    const std::string program = fs::canonical("/proc/self/exe", ec).string();
    if (data.empty() || ec || program.empty()) return;

    // menu entry
    std::string icon = "applications-games";
    const fs::path png = fs::path(program_dir) / "motoracer.png";
    if (ico_to_png(fs::path(game_dir) / "goggame-1207658895.ico", png)) icon = png.string();
    const fs::path desktop = fs::path(data) / "applications" / "motoracer-recomp.desktop";
    const std::string entry = std::string("[Desktop Entry]\nType=Application\nName=") + kName +
                              "\nComment=Moto Racer, recompiled from your GOG copy\nExec=" + desktop_exec(program) +
                              "\nPath=" + desktop_string(program_dir) + "\nIcon=" + desktop_string(icon) +
                              "\nCategories=Game;\nTerminal=false\n";
    if (write_if_changed(desktop, entry)) rt::log("launchers: menu entry %s\n", desktop.string().c_str());

    // Steam: the controller layout first, then the library entry
    const fs::path steam = fs::path(data) / "Steam";
    if (!fs::is_directory(steam / "userdata", ec)) return;
    set_layouts(steam);
    const fs::path marker = fs::path(program_dir) / ".steam-shortcut";
    if (in_steam(steam, program)) {
        write_if_changed(marker, program + "\n");
        return;
    }
    const char *by_steam = getenv("SteamGameId");
    if (by_steam && *by_steam) return;                       // started from a Steam shortcut elsewhere
    if (read_file(marker) == program + "\n") return;         // added once already (or removed by the player)
    if (!process_running("steam")) return;                    // next start
    FILE *f = fopen("/tmp/addnonsteamgamefile", "w");         // as steamos-add-to-steam does
    if (f) fclose(f);
    // with Steam running the link returns at once; detached and time-limited all the same
    const std::string cmd = "(timeout 20 steam 'steam://addnonsteamgame/" + url_encode(desktop.string()) +
                            "' >/dev/null 2>&1 &)";
    if (system(cmd.c_str()) == 0) {
        write_if_changed(marker, program + "\n");
        rt::log("launchers: added \"%s\" to the Steam library (%s)\n", kName, program.c_str());
    }
}

}  // namespace plat
