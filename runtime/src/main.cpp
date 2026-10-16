// MotoRacer host entry point.
//
// usage: motoracer [--game-dir DIR] [--trace] [-- game arguments...]
//
// The game directory (GOG install contents: game.EXE, data/, NN.wav) is searched in order:
//   --game-dir DIR, $MOTORACER_GAME_DIR, <per-user data dir>/game (plat::HostPaths: on macOS
//   ~/Library/Application Support/MotoRacerRecomp/game), ./extracted/app (development checkout).
// Game arguments default to Params= from <game dir>/motoracer.ini (GOG's launcher config).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   // Windows: WinMain entry for a GUI program, UTF-8 argv

#include "platform/platform.h"
#include "runtime.h"
#include "settings.h"

namespace rt {
void install_crash_handlers();
}

using namespace rt;

namespace {

void run_game() {
    PUSH32(RETURN_MAGIC);   // the CRT entry never returns, but give it somewhere to go
    call_indirect(image.entry);
    exit_process(R.eax);
}

bool has_game(const std::string &dir) {
    return !dir.empty() && plat::host().fs.stat(dir + "/game.EXE").exists;
}

std::string find_game_dir(const std::string &explicit_dir) {
    std::vector<std::string> candidates;
    if (!explicit_dir.empty()) return explicit_dir;
    if (const char *e = getenv("MOTORACER_GAME_DIR")) candidates.push_back(e);
    if (std::string data = plat::host().paths.data_dir(); !data.empty()) candidates.push_back(data + "/game");
    candidates.push_back("extracted/app");
    for (auto &c : candidates)
        if (has_game(c)) return c;
    return "";
}

[[noreturn]] void no_game() {
    const char *msg =
        "Moto Racer game files were not found.\n\n"
        "Extract your GOG installer with:\n"
        "  tools/install_game.sh setup_moto_racer_2.1.0.11.exe\n\n"
        "or start with --game-dir <folder containing game.EXE>.";
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Moto Racer", msg, nullptr);
    exit(1);
}

}  // namespace

int main(int argc, char **argv) {
    open_log_file();
    std::string game_dir_arg, game_args;
    bool explicit_args = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--game-dir" && i + 1 < argc) game_dir_arg = argv[++i];
        else if (a == "--trace") trace_imports = true;
        else if (a == "--") {
            explicit_args = true;
            for (i++; i < argc; i++) game_args += (game_args.empty() ? "" : " ") + std::string(argv[i]);
        } else if (a.rfind("-psn_", 0) == 0) {
            // Finder process serial number on older macOS launches; ignore.
        } else {
            fprintf(stderr, "usage: %s [--game-dir DIR] [--trace] [-- game args]\n", argv[0]);
            return 2;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) fatal("SDL_Init failed: %s", SDL_GetError());
    // Gamepads from the start: SDL then reports each connected pad (SDL_EVENT_GAMEPAD_ADDED) and
    // the runtime opens it, so the menus react to it before the game itself polls the joystick.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log("gamepad: init failed: %s\n", SDL_GetError());
    std::string game_dir = find_game_dir(game_dir_arg);
    if (!has_game(game_dir)) no_game();
    settings::load(game_dir);
    if (!explicit_args) game_args = settings::get().game_args;
    log("game dir: %s\nargs: %s\n", game_dir.c_str(), game_args.c_str());

    memory_init();
    load_image(game_dir + "/game.EXE");
    if (!plat::host().fs.set_current_dir(game_dir)) fatal("cannot chdir to %s", game_dir.c_str());
    setup_thread_environment("game.EXE " + game_args);

    install_crash_handlers();
    // Recompiled code nests one host frame per guest call; give it a deep stack, but stay
    // on the main thread because Cocoa (and so SDL video/events) requires it.
    if (!plat::host().execution.run_on_large_stack(HOST_STACK_BYTES, run_game)) fatal("cannot allocate host stack");
    return 0;
}
