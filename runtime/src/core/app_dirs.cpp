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
#endif

}  // namespace

std::string app_data_dir() {
#ifdef RECOMP_ONE_FOLDER
    static const std::string dir = [] {
        const std::string root = package_dir();
        if (root.empty()) return plat::host().paths.data_dir();
        return adopt_old_install(root) ? root : plat::host().paths.data_dir();
    }();
    return dir;
#else
    return plat::host().paths.data_dir();
#endif
}

std::string app_log_dir() {
#ifdef RECOMP_ONE_FOLDER
    if (const std::string root = package_dir(); !root.empty()) return root + "/logs";
#endif
    return plat::host().paths.log_dir();
}

}  // namespace rt
