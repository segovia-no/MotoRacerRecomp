// The generated game code as the runtime sees it (docs/28-easy-install.md): linked into the program
// (default), or a module the program loads at the first use (MOTORACER_GAME_MODULE): the
// translated C compiled on the player's machine into gamecode.so / gamecode.dll next to the program.
// Either way the code exports one RecompModule (dispatch table, translated fallbacks of the native
// functions, import table); a module's import shims are slots filled here from the host's shims.
#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>

#include "platform/platform.h"
#include "runtime.h"

#ifndef RECOMP_GAME_MODULE
extern "C" const RecompModule recomp_module;
#endif

namespace rt {

namespace {
std::map<std::string, RecompFn> &shims() {
    static std::map<std::string, RecompFn> m;
    return m;
}

#ifdef RECOMP_GAME_MODULE
std::string module_path;   // set by the first-start installer (installer.cpp)

const RecompModule *load_module() {
    plat::Modules &mods = plat::host().modules;
    std::string path = module_path;
    if (const char *e = getenv("MOTORACER_GAME_MODULE"); e && *e) {
        path = e;
    } else if (path.empty()) {
        const char *base = SDL_GetBasePath();
        path = std::string(base ? base : "") + "gamecode" + mods.suffix();
    }
    std::string error;
    void *h = mods.open(path, &error);
    if (!h) fatal("cannot load the game code %s: %s", path.c_str(), error.c_str());
    auto *m = static_cast<const RecompModule *>(mods.symbol(h, "recomp_module"));
    if (!m) fatal("%s is not a Moto Racer game-code module (no recomp_module)", path.c_str());
    if (m->version != RECOMP_MODULE_VERSION)
        fatal("%s is game-code module version %u, this program needs %u: rebuild it", path.c_str(), m->version,
              RECOMP_MODULE_VERSION);
    uint32_t found = 0;
    for (uint32_t i = 0; i < m->slot_count; i++)
        if ((m->slots[i] = import_shim(m->slot_names[i]))) found++;
    log("game code: %s, %u functions, %u of %u import shims\n", path.c_str(), m->function_count, found, m->slot_count);
    return m;
}
#endif
}  // namespace

ImportShim::ImportShim(const char *name, RecompFn fn) { shims()[name] = fn; }

#ifdef RECOMP_GAME_MODULE
void set_game_module(const std::string &path) { module_path = path; }
#endif

RecompFn import_shim(const char *name) {
    auto it = shims().find(name);
    return it == shims().end() ? nullptr : it->second;
}

const RecompModule &guest_code() {
#ifdef RECOMP_GAME_MODULE
    static const RecompModule *m = load_module();
    return *m;
#else
    return recomp_module;
#endif
}

RecompFn guest_fallback(uint32_t va) {
    const RecompModule &g = guest_code();
    for (uint32_t i = 0; i < g.fallback_count; i++)
        if (g.fallbacks[i].va == va) return g.fallbacks[i].fn;
    fatal("no translated fallback for 0x%08x in the game code", va);
}

}  // namespace rt

#ifdef RECOMP_GAME_MODULE
// The guest functions the runtime calls by name (native hooks and their check mode): forwarders to
// the module's code.
#define GUEST_FN(VA)                                                                           \
    extern "C" void f_##VA(void) {                                                             \
        static const RecompFn f = rt::find_function(0x##VA);                                   \
        f();                                                                                   \
    }
#define GUEST_FALLBACK(VA)                                                                     \
    extern "C" void f_##VA##_t(void) {                                                         \
        static const RecompFn f = rt::guest_fallback(0x##VA);                                  \
        f();                                                                                   \
    }
GUEST_FN(00419d60) GUEST_FN(00419ee0) GUEST_FN(0041dde0) GUEST_FN(0041e000) GUEST_FN(0041e1b0)
GUEST_FN(0041e2b0) GUEST_FN(0041e380) GUEST_FN(0042f650) GUEST_FN(0049c590) GUEST_FN(0049ca70)
GUEST_FALLBACK(0040c7e0) GUEST_FALLBACK(00419d60) GUEST_FALLBACK(00419ee0) GUEST_FALLBACK(0041dde0)
GUEST_FALLBACK(0041e2b0) GUEST_FALLBACK(0041e380) GUEST_FALLBACK(00422b90) GUEST_FALLBACK(0042f650)
GUEST_FALLBACK(00497080) GUEST_FALLBACK(0049b2c0) GUEST_FALLBACK(0049b7f0) GUEST_FALLBACK(0049c110)
GUEST_FALLBACK(0049c590) GUEST_FALLBACK(0049ca70) GUEST_FALLBACK(004b8800) GUEST_FALLBACK(004c5e9d)
#endif
