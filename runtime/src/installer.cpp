// First start of a release build (docs/28-easy-install.md, E4): the program ships without any code
// translated from game.EXE, plus a "kit" of tools (Python + the translator, zig, innoextract). Here it
// finds the player's GOG installer, unpacks the game files, and builds the game-code module with the
// kit, behind a small progress screen. The module is rebuilt by itself when the kit changes (a new
// release: kit/BUILD_ID differs from the one the module was built with).
//
// Layout: the kit is <program folder>/kit (macOS app: Contents/Resources/kit, or MOTORACER_KIT);
// the game files go to <data>/game, the module to <data>/gamecode/gamecode.<so|dll>.
// Developer builds skip all this: MOTORACER_GAME_MODULE, or a module next to the program.
#ifdef RECOMP_GAME_MODULE
#include "installer.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "platform/platform.h"
#include "runtime.h"

namespace fs = std::filesystem;
using rt::log;

namespace installer {
namespace {

#ifdef _WIN32
constexpr const char *EXE = ".exe";
constexpr const char *PYTHON = "python/python.exe";
#else
constexpr const char *EXE = "";
constexpr const char *PYTHON = "python/bin/python3";
#endif
constexpr const char *SETUP_NAME = "setup_moto_racer_2.1.0.11.exe";

// UTF-8 std::string <-> path (Windows paths are UTF-16)
fs::path P(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string S(const fs::path &p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool present(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::string read_text(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string find_kit() {
    if (const char *e = getenv("MOTORACER_KIT"); e && *e) return e;
    const char *b = SDL_GetBasePath();
    std::string base = b ? b : "";
    for (const char *rel : {"kit", "../Resources/kit"})
        if (present(P(base + rel) / "BUILD_ID")) return S((P(base + rel)).lexically_normal());
    return "";
}

// The GOG installer in the player's Downloads folder, if there is one
std::string find_setup() {
    const char *dl = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
    if (!dl) return "";
    fs::path exact = P(dl) / SETUP_NAME;
    if (present(exact)) return S(exact);
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(P(dl), ec)) {
        std::string n = S(e.path().filename());
        if (n.rfind("setup_moto_racer", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".exe") return S(e.path());
    }
    return "";
}

std::atomic<SDL_Process *> running{nullptr};   // the kit tool being run (killed if the player quits)

// Runs a kit tool, passing each output line (stdout + stderr) to on_line; returns the exit code.
int run_tool(const std::vector<std::string> &args, const std::function<void(const std::string &)> &on_line) {
    std::vector<const char *> argv;
    for (const auto &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!proc) {
        on_line(std::string("cannot start ") + args[0] + ": " + SDL_GetError());
        return -1;
    }
    running = proc;
    SDL_IOStream *out = SDL_GetProcessOutput(proc);
    std::string line;
    char buf[4096];
    for (;;) {
        size_t n = SDL_ReadIO(out, buf, sizeof buf);
        if (n == 0) {
            if (SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) {
                SDL_Delay(20);
                continue;
            }
            break;
        }
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (!line.empty()) on_line(line);
                line.clear();
            } else {
                line += buf[i];
            }
        }
    }
    if (!line.empty()) on_line(line);
    int code = -1;
    SDL_WaitProcess(proc, true, &code);
    running = nullptr;
    SDL_DestroyProcess(proc);
    return code;
}

// ---------- the work (a thread), reported through Status ----------
struct Status {
    std::mutex mu;
    std::string step, detail, error;
    int step_no = 0, steps = 0;
    float progress = -1;   // 0..1, or < 0: unknown
    std::atomic<bool> done{false};
    void set(const std::string &s, int no, float p = -1) {
        std::lock_guard<std::mutex> l(mu);
        step = s;
        step_no = no;
        progress = p;
        detail.clear();
    }
};

struct Job {
    std::string kit, data, game_dir, setup, build_id;
    bool need_game = false, need_build = false;
};

// Unpacks the installer into game_dir (keeping the player's settings and best times, as
// tools/install_game.sh does)
bool unpack(const Job &job, Status &st, int step_no) {
    st.set("Unpacking the game files", step_no);
    fs::path tmp = P(job.data) / "install-tmp", dest = P(job.game_dir);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::string last;
    int code = run_tool({job.kit + "/innoextract" + EXE, "--gog", "--silent", "-d", S(tmp), job.setup},
                        [&](const std::string &l) { log("innoextract: %s\n", l.c_str()); last = l; });
    fs::path app = tmp / "app";
    if (code != 0 || !present(app / "game.EXE")) {
        std::lock_guard<std::mutex> l(st.mu);
        st.error = code != 0 ? "Could not unpack the installer (" + last + ")."
                             : "This is not the GOG Moto Racer installer (no game.EXE inside).";
        return false;
    }
    st.set("Copying the game files", step_no);
    const char *keep[] = {"data/CONFIG.BPT", "data/bestscor.dat", "motoracer.ini"};   // the player's
    fs::create_directories(dest, ec);
    for (auto it = fs::recursive_directory_iterator(app, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        fs::path rel = it->path().lexically_relative(app), to = dest / rel;
        if (it->is_directory()) {
            fs::create_directories(to, ec);
            continue;
        }
        bool players = false;
        for (const char *k : keep) players |= rel.generic_string() == k;
        if (players && present(to)) continue;
        fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::lock_guard<std::mutex> l(st.mu);
            st.error = "Could not copy " + S(rel) + ": " + ec.message();
            return false;
        }
    }
    fs::remove_all(tmp, ec);
    // the port's settings with good defaults, once (setup.sh does the same)
    fs::path ini = dest / "motoracer.ini", defaults = P(job.kit) / "default_settings.ini";
    if (present(ini) && present(defaults) && read_text(ini).find("[Recomp]") == std::string::npos) {
        std::ofstream out(ini, std::ios::app | std::ios::binary);
        out << "\n" << read_text(defaults) << "\n";
    }
    return true;
}

bool build(const Job &job, Status &st, int step_no) {
    st.set("Translating game.EXE", step_no);
    fs::path work = P(job.data) / "gamecode-build", final_dir = P(job.data) / "gamecode";
    // -B: no __pycache__ inside the kit (a signed, maybe read-only app bundle)
    std::vector<std::string> args = {job.kit + "/" + PYTHON, "-I", "-B", job.kit + "/tools/build_gamecode.py",
                                     "--exe", job.game_dir + "/game.EXE", "--out", S(work),
                                     "--zig", job.kit + "/zig/zig" + EXE, "--progress"};
#ifdef _WIN32
    args.push_back("--host-lib");
    args.push_back(job.kit + "/libmotoracer.dll.a");
#endif
    std::vector<std::string> tail;
    int code = run_tool(args, [&](const std::string &l) {
        log("build: %s\n", l.c_str());
        if (l.rfind("@step ", 0) == 0) {
            std::string s = l.substr(6);
            if (s == "translate") st.set("Translating game.EXE", step_no);
            else if (s == "compile") st.set("Compiling the game code", step_no + 1, 0);
            else if (s == "link") st.set("Linking", step_no + 1, 1);
        } else if (l.rfind("@compiled ", 0) == 0) {
            int done = 0, total = 1;
            if (sscanf(l.c_str() + 10, "%d %d", &done, &total) == 2 && total > 0) {
                std::lock_guard<std::mutex> lk(st.mu);
                st.progress = static_cast<float>(done) / total;
                st.detail = std::to_string(done) + " of " + std::to_string(total) + " files";
            }
        } else {
            tail.push_back(l);
            if (tail.size() > 6) tail.erase(tail.begin());
        }
    });
    fs::path built = work / ("gamecode" + std::string(plat::host().modules.suffix()));
    if (code != 0 || !present(built)) {
        std::string msg = "Building the game code failed.";
        for (const auto &t : tail) msg += "\n" + t;
        std::lock_guard<std::mutex> l(st.mu);
        st.error = msg;
        return false;
    }
    std::error_code ec;
    fs::create_directories(final_dir, ec);
    fs::path module = final_dir / built.filename();
    fs::remove(module, ec);
    fs::rename(built, module, ec);
    if (ec) fs::copy_file(built, module, fs::copy_options::overwrite_existing, ec);
    std::ofstream(final_dir / "build_id.txt", std::ios::binary) << job.build_id << "\n";
    fs::remove_all(work / "gen", ec);   // keep zig's cache for the next rebuild, not the sources
    fs::remove_all(work / "obj", ec);
    return true;
}

void work(Job job, Status *st) {
    int no = 1;
    bool ok = true;
    if (job.need_game) ok = unpack(job, *st, no++);
    if (ok && job.need_build) ok = build(job, *st, no);
    st->done = true;
}

// ---------- the screen ----------
struct Screen {
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    std::string title = "MOTO RACER - FIRST START";
    static constexpr float W = 480, H = 270;   // logical size; SDL's debug font is 8x8

    bool open() {
        win = SDL_CreateWindow("Moto Racer", 960, 540, SDL_WINDOW_RESIZABLE);
        if (!win) return false;
        ren = SDL_CreateRenderer(win, nullptr);
        if (!ren) return false;
        SDL_SetRenderLogicalPresentation(ren, static_cast<int>(W), static_cast<int>(H), SDL_LOGICAL_PRESENTATION_LETTERBOX);
        SDL_SetRenderVSync(ren, 1);
        return true;
    }
    void close() {
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        ren = nullptr;
        win = nullptr;
    }
    void color(uint8_t r, uint8_t g, uint8_t b) { SDL_SetRenderDrawColor(ren, r, g, b, 255); }
    void text(float x, float y, std::string s, uint8_t r = 230, uint8_t g = 230, uint8_t b = 230) {
        const size_t max = static_cast<size_t>((W - x - 8) / 8);
        if (s.size() > max) s = "..." + s.substr(s.size() - (max - 3));   // keep a path's end
        color(r, g, b);
        SDL_RenderDebugText(ren, x, y, s.c_str());
    }
    void begin() {
        color(16, 20, 48);
        SDL_RenderClear(ren);
        color(250, 200, 0);
        SDL_FRect bar{0, 0, W, 34};
        SDL_RenderFillRect(ren, &bar);
        color(16, 20, 48);
        SDL_RenderDebugText(ren, 16, 13, title.c_str());
    }
    void end() {
        snapshot();
        SDL_RenderPresent(ren);
    }
    // MOTORACER_SNAPSHOT_DIR (tests): the screen every 3 s as installer_<n>.bmp
    void snapshot() {
        static const char *dir = getenv("MOTORACER_SNAPSHOT_DIR");
        static uint64_t last = 0;
        static int n = 0;
        if (!dir || (last && SDL_GetTicks() - last < 3000)) return;
        last = SDL_GetTicks();
        if (SDL_Surface *shot = SDL_RenderReadPixels(ren, nullptr)) {
            char name[64];
            snprintf(name, sizeof name, "/installer_%03d.bmp", n++);
            SDL_SaveBMP(shot, (std::string(dir) + name).c_str());
            SDL_DestroySurface(shot);
        }
    }
};

struct Button {
    SDL_FRect r;
    std::string label;
};

void draw_buttons(Screen &s, const std::vector<Button> &bs, int sel) {
    for (size_t i = 0; i < bs.size(); i++) {
        const bool on = static_cast<int>(i) == sel;
        s.color(on ? 250 : 60, on ? 200 : 70, on ? 0 : 120);
        SDL_RenderFillRect(s.ren, &bs[i].r);
        s.text(bs[i].r.x + (bs[i].r.w - 8 * bs[i].label.size()) / 2, bs[i].r.y + 8, bs[i].label, on ? 16 : 230,
               on ? 20 : 230, on ? 48 : 230);
    }
}

std::vector<Button> row(const std::vector<std::string> &labels) {
    std::vector<Button> bs;
    float w = 0;
    for (const auto &l : labels) w += 8 * l.size() + 24;
    w += 12 * (labels.size() - 1);
    float x = (Screen::W - w) / 2;
    for (const auto &l : labels) {
        float bw = 8 * l.size() + 24;
        bs.push_back({{x, 222, bw, 24}, l});
        x += bw + 12;
    }
    return bs;
}

std::vector<std::string> wrap(const std::string &s, size_t width) {
    std::vector<std::string> out;
    std::stringstream in(s);
    std::string para;
    while (std::getline(in, para)) {
        while (para.size() > width) {
            size_t cut = para.rfind(' ', width);
            if (cut == std::string::npos || cut == 0) cut = width;
            out.push_back(para.substr(0, cut));
            para = para.substr(cut + (para[cut] == ' ' ? 1 : 0));
        }
        out.push_back(para);
    }
    return out;
}

// Input: activate (Enter / A / click), other (Tab / O / X), quit (Esc / B / window close),
// move left / right
enum class Key { None, Activate, Choose, Quit, Left, Right, Click };

Key poll(Screen &s, const std::vector<Button> &bs, int &sel) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        SDL_ConvertEventToRenderCoordinates(s.ren, &e);
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            log("installer: %s\n", e.type == SDL_EVENT_QUIT ? "quit event" : "window closed");
            return Key::Quit;
        case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
        case SDL_EVENT_KEY_DOWN:
            switch (e.key.key) {
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return Key::Activate;
            case SDLK_ESCAPE: log("installer: Esc\n"); return Key::Quit;
            case SDLK_LEFT: return Key::Left;
            case SDLK_RIGHT: case SDLK_TAB: return Key::Right;
            case SDLK_O: return Key::Choose;
            default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: return Key::Activate;
            case SDL_GAMEPAD_BUTTON_EAST: log("installer: gamepad B\n"); return Key::Quit;
            case SDL_GAMEPAD_BUTTON_WEST: return Key::Choose;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Key::Left;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Key::Right;
            default: break;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            for (size_t i = 0; i < bs.size(); i++) {
                SDL_FPoint p{e.motion.x, e.motion.y};
                if (SDL_PointInRectFloat(&p, &bs[i].r)) sel = static_cast<int>(i);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            for (size_t i = 0; i < bs.size(); i++) {
                SDL_FPoint p{e.button.x, e.button.y};
                if (SDL_PointInRectFloat(&p, &bs[i].r)) {
                    sel = static_cast<int>(i);
                    return Key::Click;
                }
            }
            break;
        default: break;
        }
    }
    return Key::None;
}

struct Picked {
    std::mutex mu;
    bool answered = false;
    std::string path;
};

void SDLCALL on_file(void *userdata, const char *const *files, int) {
    auto *p = static_cast<Picked *>(userdata);
    std::lock_guard<std::mutex> l(p->mu);
    p->answered = true;
    p->path = files && files[0] ? files[0] : "";
}

// The worker thread may still be running a tool: stop that one (its next start notices nothing was
// finished: no stamp) and leave without running destructors under the worker's feet.
[[noreturn]] void quit() {
    log("installer: quit by the player\n");
    if (SDL_Process *p = running.load()) SDL_KillProcess(p, true);
    std::_Exit(0);
}

// Asks for the installer; returns its path
std::string choose_setup(Screen &s) {
    std::string found = find_setup();
    Picked picked;
    bool dialog_open = false;
    int sel = 0;
    std::string note;
    for (;;) {
        std::vector<std::string> labels = found.empty() ? std::vector<std::string>{"Choose installer...", "Quit"}
                                                        : std::vector<std::string>{"Install", "Choose another...", "Quit"};
        auto bs = row(labels);
        Key k = poll(s, bs, sel);
        if (k == Key::Left) sel = (sel + static_cast<int>(bs.size()) - 1) % static_cast<int>(bs.size());
        if (k == Key::Right) sel = (sel + 1) % static_cast<int>(bs.size());
        if (k == Key::Quit) quit();
        std::string action;
        if (k == Key::Activate || k == Key::Click) action = labels[sel];
        if (k == Key::Choose) action = "Choose";
        if (action == "Install") return found;
        if (action == "Quit") quit();
        if (!action.empty() && !dialog_open) {   // a choose button
            static const SDL_DialogFileFilter filter{"GOG installer", "exe"};
            const char *dl = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
            picked.answered = false;
            dialog_open = true;
            SDL_ShowOpenFileDialog(on_file, &picked, s.win, &filter, 1, dl, false);
        }
        {
            std::lock_guard<std::mutex> l(picked.mu);
            if (picked.answered) {
                picked.answered = false;
                dialog_open = false;
                if (!picked.path.empty()) {
                    std::string n = S(P(picked.path).filename());
                    if (n.size() > 4 && (n.substr(n.size() - 4) == ".exe" || n.substr(n.size() - 4) == ".EXE")) return picked.path;
                    note = "That is not an .exe file.";
                }
            }
        }
        s.begin();
        float y = 50;
        for (const char *l : {"Moto Racer is built from your own copy of the game.",
                              "It needs the GOG offline installer:"})
            s.text(16, y, l), y += 12;
        s.text(32, y, SETUP_NAME, 250, 200, 0), y += 20;
        if (!found.empty()) {
            s.text(16, y, "Found in your Downloads folder:"), y += 12;
            s.text(32, y, found, 160, 220, 160), y += 20;
        } else {
            s.text(16, y, "It is not in your Downloads folder. Choose it, or put it"), y += 12;
            s.text(16, y, "there and start Moto Racer again."), y += 20;
        }
        s.text(16, y, "The game files are unpacked and the game code is built on"), y += 12;
        s.text(16, y, "this computer once (about a minute)."), y += 12;
        if (!note.empty()) s.text(16, 200, note, 255, 120, 120);
        draw_buttons(s, bs, sel);
        s.end();
    }
}

void progress_screen(Screen &s, Status &st) {
    int sel = 0;
    std::vector<Button> none;
    uint64_t t0 = SDL_GetTicks();
    while (!st.done) {
        if (poll(s, none, sel) == Key::Quit) quit();   // the tools stop with the program
        std::string step, detail;
        int no, steps;
        float p;
        {
            std::lock_guard<std::mutex> l(st.mu);
            step = st.step;
            detail = st.detail;
            no = st.step_no;
            steps = st.steps;
            p = st.progress;
        }
        s.begin();
        s.text(16, 60, "Step " + std::to_string(no) + " of " + std::to_string(steps) + ": " + step);
        SDL_FRect frame{16, 84, Screen::W - 32, 14}, fill = frame;
        s.color(60, 70, 120);
        SDL_RenderFillRect(s.ren, &frame);
        s.color(250, 200, 0);
        if (p >= 0) {
            fill.w = frame.w * std::min(1.f, p);
        } else {   // unknown length: a block going back and forth
            const float t = (SDL_GetTicks() - t0) / 1000.f;
            fill.w = frame.w / 5;
            fill.x = frame.x + (frame.w - fill.w) * (0.5f + 0.5f * std::sin(t * 2.5f));
        }
        SDL_RenderFillRect(s.ren, &fill);
        s.text(16, 106, detail, 180, 180, 200);
        s.text(16, 140, "Only on the first start, and once after an update.", 180, 180, 200);
        s.text(16, 152, "Elapsed: " + std::to_string((SDL_GetTicks() - t0) / 1000) + " s", 180, 180, 200);
        s.end();
    }
}

[[noreturn]] void error_screen(Screen &s, const std::string &error) {
    log("installer: %s\n", error.c_str());
    int sel = 0;
    auto bs = row({"Quit"});
    for (;;) {
        Key k = poll(s, bs, sel);
        if (k != Key::None && k != Key::Left && k != Key::Right) quit();
        s.begin();
        float y = 50;
        s.text(16, y, "Something went wrong:", 255, 120, 120), y += 16;
        for (const auto &l : wrap(error, 56)) {
            if (y > 190) break;
            s.text(16, y, l), y += 11;
        }
        s.text(16, 200, "Details: " + rt::log_file_path(), 180, 180, 200);
        draw_buttons(s, bs, sel);
        s.end();
    }
}

}  // namespace

std::string prepare(const std::string &explicit_game_dir) {
    plat::Modules &mods = plat::host().modules;
    const char *base = SDL_GetBasePath();
    const std::string dev_module = std::string(base ? base : "") + "gamecode" + mods.suffix();
    const char *env_module = getenv("MOTORACER_GAME_MODULE");
    const std::string kit = find_kit();
    if ((env_module && *env_module) || present(P(dev_module)) || kit.empty()) {
        if (kit.empty() && !(env_module && *env_module) && !present(P(dev_module)))
            log("installer: no kit next to the program and no game-code module\n");
        return "";   // developer build: as before
    }
    Job job;
    job.kit = kit;
    // MOTORACER_DATA_DIR (tests): another data folder than the player's
    job.data = getenv("MOTORACER_DATA_DIR") ? getenv("MOTORACER_DATA_DIR") : plat::host().paths.data_dir();
    job.build_id = read_text(P(kit) / "BUILD_ID");
    job.game_dir = !explicit_game_dir.empty() ? explicit_game_dir
                 : getenv("MOTORACER_GAME_DIR") ? getenv("MOTORACER_GAME_DIR") : job.data + "/game";
    const fs::path module = P(job.data) / "gamecode" / ("gamecode" + std::string(mods.suffix()));
    job.need_game = !present(P(job.game_dir) / "game.EXE");
    job.need_build = job.need_game || !present(module) ||
                     read_text(P(job.data) / "gamecode" / "build_id.txt") != job.build_id;
    log("installer: kit %s (%s), game %s%s, module %s%s\n", kit.c_str(), job.build_id.c_str(), job.game_dir.c_str(),
        job.need_game ? " (missing)" : "", S(module).c_str(), job.need_build ? " (to build)" : "");
    if (!job.need_game && !job.need_build) {
        rt::set_game_module(S(module));
        return job.game_dir;
    }
    Screen screen;
    if (!job.need_game) screen.title = "MOTO RACER - UPDATING";
    if (!screen.open()) rt::fatal("cannot open the first-start window: %s", SDL_GetError());
    // MOTORACER_SETUP (tests, scripted installs): the installer, without asking
    if (job.need_game) job.setup = getenv("MOTORACER_SETUP") ? getenv("MOTORACER_SETUP") : choose_setup(screen);
    Status st;
    st.steps = (job.need_game ? 1 : 0) + (job.need_build ? 2 : 0);
    std::thread worker(work, job, &st);
    progress_screen(screen, st);
    worker.join();
    if (!st.error.empty()) error_screen(screen, st.error);
    screen.close();
    rt::set_game_module(S(module));
    return job.game_dir;
}

}  // namespace installer
#endif  // RECOMP_GAME_MODULE
