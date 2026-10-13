// MotoRacer host entry point.
//
// usage: motoracer [--game-dir DIR] [--trace] [-- game arguments...]
//   DIR defaults to ./extracted/app (the innoextract output); it must contain game.EXE + data/.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL3/SDL.h>

#include "runtime.h"

namespace rt {
void install_crash_handlers();
}

extern "C" void call_on_stack(void *stack_top, void (*fn)(void));

using namespace rt;

namespace {

// GOG's launcher (MotoRacer.exe + motoracer.ini) passes these by default.
const char *kDefaultGameArgs =
    "-DoRealLock -OpaqueBlackLevel3 -TextureSquareOnly -Pal16 -CorrectRatioTextures -NoDither "
    "-TextureVideoMem268435456 -FrameRateMax60";

void run_game() {
    PUSH32(RETURN_MAGIC);   // the CRT entry never returns, but give it somewhere to go
    call_indirect(image.entry);
    exit_process(R.eax);
}

}  // namespace

int main(int argc, char **argv) {
    std::string game_dir = "extracted/app";
    std::string game_args;
    bool explicit_args = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--game-dir" && i + 1 < argc) game_dir = argv[++i];
        else if (a == "--trace") trace_imports = true;
        else if (a == "--") {
            explicit_args = true;
            for (i++; i < argc; i++) game_args += (game_args.empty() ? "" : " ") + std::string(argv[i]);
        } else {
            fprintf(stderr, "usage: %s [--game-dir DIR] [--trace] [-- game args]\n", argv[0]);
            return 2;
        }
    }
    if (!explicit_args) game_args = kDefaultGameArgs;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) fatal("SDL_Init failed: %s", SDL_GetError());
    memory_init();
    load_image(game_dir + "/game.EXE");
    if (chdir(game_dir.c_str()) != 0) fatal("cannot chdir to %s", game_dir.c_str());
    setup_thread_environment("game.EXE " + game_args);

    install_crash_handlers();
    // Recompiled code nests one host frame per guest call; give it a deep stack, but stay
    // on the main thread because Cocoa (and so SDL video/events) requires it.
    const size_t stack_size = 512u << 20;
    void *stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack == MAP_FAILED) fatal("cannot allocate host stack");
    call_on_stack(static_cast<char *>(stack) + stack_size, run_game);
    return 0;
}
