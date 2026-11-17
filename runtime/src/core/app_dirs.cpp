// Where the program keeps the player's data (game files, compiled game code, settings, saves) and its
// log. Release packages on Linux / Steam Deck and Windows keep everything in their own folder, next to
// the program and its kit (issue #5: one folder instead of MotoRacer/ plus ~/.local/share/MotoRacerRecomp):
// <program folder>/game, /gamecode, /logs. A program folder that cannot be written to (e.g. under
// Program Files) and developer builds use the platform's per-user folders (plat::HostPaths); so does
// macOS, whose app bundle must stay unchanged (its signature). An earlier install in the per-user
// folder is moved into the program folder on the first start (copied, then removed, across disks;
// left where it is if that fails).
#include "platform/platform.h"
#include "runtime.h"

#if defined(RECOMP_GAME_MODULE) && !defined(__APPLE__)
#include <SDL3/SDL_filesystem.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#define RECOMP_ONE_FOLDER 1
#endif

namespace rt {

namespace {

#ifdef RECOMP_ONE_FOLDER
namespace fs = std::filesystem;

// the program's folder when it is a release package (kit/BUILD_ID next to it) we can write to
std::string package_dir() {
    static const std::string dir = [] {
        const char *base = SDL_GetBasePath();
        if (!base || !*base) return std::string();
        std::error_code ec;
        fs::path root = fs::path(base).lexically_normal();
        if (root.has_filename() == false) root = root.parent_path();   // a trailing separator
        if (!fs::exists(root / "kit" / "BUILD_ID", ec)) return std::string();
        const fs::path probe = root / ".write-test";
        if (FILE *f = fopen(probe.string().c_str(), "w")) {
            fclose(f);
            fs::remove(probe, ec);
        } else {
            return std::string();   // read-only location: the per-user folders
        }
        return root.generic_string();
    }();
    return dir;
}

// Copies a folder tree byte by byte, creating each file with the usual permissions (fs::copy creates
// a file write-only and then copies the source's permissions, which some file systems refuse silently,
// leaving the copies unreadable).
bool copy_tree(const fs::path &from, const fs::path &to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return false;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path dst = to / fs::relative(it->path(), from, ec);
        if (ec) return false;
        if (it->is_directory(ec)) {
            fs::create_directories(dst, ec);
            if (ec) return false;
            continue;
        }
        FILE *in = fopen(it->path().string().c_str(), "rb");
        FILE *out = in ? fopen(dst.string().c_str(), "wb") : nullptr;
        bool good = in && out;
        static char buf[1 << 16];
        for (size_t n; good && (n = fread(buf, 1, sizeof buf, in)) > 0;) good = fwrite(buf, 1, n, out) == n;
        if (in) good = good && !ferror(in), fclose(in);
        if (out) good = fclose(out) == 0 && good;
        if (!good) return false;
    }
    return !ec;
}

// Moves an earlier install's game/ and gamecode/ from the per-user folder; false if that failed.
bool adopt_old_install(const std::string &root) {
    std::error_code ec;
    const std::string old = plat::host().paths.data_dir();
    if (old.empty() || fs::exists(fs::path(root) / "game", ec)) return true;
    bool ok = true;
    for (const char *sub : {"game", "gamecode"}) {
        const fs::path from = fs::path(old) / sub, to = fs::path(root) / sub;
        if (!fs::exists(from, ec)) continue;
        fs::rename(from, to, ec);   // the same disk: instant
        if (!ec) continue;
        ec.clear();                 // another disk: copy, then remove the old copy
        if (!copy_tree(from, to)) {
            std::error_code ignore;
            fs::remove_all(to, ignore);   // a partial copy: keep using the old folder
            ok = false;
            continue;
        }
        fs::remove_all(from, ec);
    }
    return ok;
}

// What an earlier install leaves in the per-user folders once its game files moved into the package
// (GitHub issue #5: "still 2 different root directories"): the module build folder with zig's cache,
// an unpacking folder, the logs. Removed when the per-user data folder no longer holds an install of
// its own (no game/, gamecode/ or a source build's bin/), then the folders themselves if empty. Only
// these known names are removed; anything else keeps its folder.
void remove_old_leftovers() {
    std::error_code ec;
    const std::string data = plat::host().paths.data_dir(), logs = plat::host().paths.log_dir();
    if (data.empty() || !fs::exists(data, ec)) return;
    for (const char *sub : {"game", "gamecode", "bin"})
        if (fs::exists(fs::path(data) / sub, ec)) return;
    uintmax_t removed = 0;
    for (const char *sub : {"gamecode-build", "install-tmp"}) removed += fs::remove_all(fs::path(data) / sub, ec);
    if (!logs.empty())
        for (const char *name : {"motoracer.log", "motoracer.log.1", "dump"}) removed += fs::remove_all(fs::path(logs) / name, ec);
    if (!logs.empty()) fs::remove(logs, ec);   // only when empty (Windows: <data>/logs)
    fs::remove(data, ec);
    if (removed || !fs::exists(data, ec))
        log("data folder: removed %ju leftover entries of an earlier install in %s\n", removed, data.c_str());
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// `dir`'s entry named `name` in any case (the game's own file names vary in case: Data\champ.., data/)
fs::path find_ci(const fs::path &dir, const std::string &name) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (lower(it->path().filename().string()) == lower(name)) return it->path();
    return dir / name;
}

bool player_file(const std::string &in_data, bool data) {
    const std::string n = lower(in_data);
    if (!data) return n == "motoracer.ini" || n == "recomp_state.txt";
    return n == "config.bpt" || n == "bestscor.dat" ||
           (n.rfind("champ", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".sav") == 0);
}

// Saves left behind in the per-user folder: when the package already had its own game/ while an
// earlier install's game/ stayed in the per-user folder (not a source build: no bin/), that one was
// never moved, and its championships (data/Champ*.sav), best times, options and settings were not used.
// Copied into the package's game/ when missing there, or when newer: a file the player never changed
// keeps the installer's date (innoextract keeps it), so any save is newer. The replaced file is kept as
// <name>.bak; nothing is deleted. A copy gets the current time, so it is not copied again.
void recover_old_saves(const std::string &root) {
    std::error_code ec;
    const std::string data = plat::host().paths.data_dir();
    if (data.empty()) return;
    const fs::path old_game = fs::path(data) / "game", new_game = fs::path(root) / "game";
    if (!fs::exists(old_game, ec) || !fs::exists(new_game, ec) || fs::exists(fs::path(data) / "bin", ec)) return;
    int copied = 0;
    auto take = [&](const fs::path &from, const fs::path &to) {
        std::error_code e;
        if (fs::exists(to, e)) {
            if (fs::last_write_time(from, e) <= fs::last_write_time(to, e) || e) return;
            fs::copy_file(to, fs::path(to.string() + ".bak"), fs::copy_options::overwrite_existing, e);
            if (e) return;
        }
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, e);
        if (e) return;
        copied++;
        log("data folder: recovered %s from %s\n", to.string().c_str(), from.string().c_str());
    };
    for (auto it = fs::directory_iterator(old_game, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_regular_file() && player_file(name, false)) take(it->path(), find_ci(new_game, name));
        if (it->is_directory() && lower(name) == "data") {
            const fs::path new_data = find_ci(new_game, name);
            std::error_code e;
            for (auto d = fs::directory_iterator(it->path(), e); !e && d != fs::directory_iterator(); d.increment(e)) {
                const std::string f = d->path().filename().string();
                if (d->is_regular_file() && player_file(f, true)) take(d->path(), find_ci(new_data, f));
            }
        }
    }
    if (copied) log("data folder: %d saved files recovered from the earlier install in %s\n", copied, data.c_str());
}
#endif

}  // namespace

std::string app_data_dir() {
#ifdef RECOMP_ONE_FOLDER
    static const std::string dir = [] {
        const std::string root = package_dir();
        if (root.empty()) return plat::host().paths.data_dir();
        if (!adopt_old_install(root)) return plat::host().paths.data_dir();
        recover_old_saves(root);
        remove_old_leftovers();
        return root;
    }();
    return dir;
#else
    return plat::host().paths.data_dir();
#endif
}

void register_launchers(const std::string &game_dir) {
#ifdef RECOMP_ONE_FOLDER
    if (const std::string root = package_dir(); !root.empty()) plat::register_launchers(root, game_dir);
#else
    (void)game_dir;
#endif
}

std::string app_log_dir() {
#ifdef RECOMP_ONE_FOLDER
    if (const std::string root = package_dir(); !root.empty()) return root + "/logs";
#endif
    return plat::host().paths.log_dir();
}

}  // namespace rt
