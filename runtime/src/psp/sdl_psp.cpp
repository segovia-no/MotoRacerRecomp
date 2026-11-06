// The SDL3 subset the runtime uses, on the PSP's own APIs (docs/25-psp-port.md, PSP-1000 memory).
// Linking SDL3 statically brought its video, renderer and blitters (~1 MB code) and the PSP
// renderer's 1 MB display list, none of which the GE backend uses. The runtime keeps calling the
// SDL functions it uses (same declarations from SDL's headers); this file defines them:
//   - entry: SDL_RunApp (module info, exit callback), init / errors / message boxes;
//   - time: ticks from sceKernelGetSystemTimeWide, delays;
//   - window / display: a fixed 480x272 screen (the GE backend presents);
//   - events: a queue fed by SDL_PushEvent and by polling sceCtrl as one gamepad ("PSP builtin
//     joypad", SDL's own mapping: Cross south, Circle east, Square west, Triangle north, L / R
//     shoulders, Select back, Start, D-pad, nub = left stick);
//   - audio: one PSP channel and thread per bound stream, pulling through the stream's get callback;
//     F32 or S16 stereo in, S16 stereo 44.1 kHz out;
//   - surfaces: SDL_CreateSurfaceFrom + SDL_SaveBMP for the dev snapshots.
#ifdef __PSP__
#include <SDL3/SDL.h>
#define SDL_MAIN_NOIMPL   // declarations only (SDL_main_func, SDL_RunApp)
#include <SDL3/SDL_main.h>

#include <pspaudio.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <psputils.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "runtime.h"
#include "fpu.h"

PSP_MODULE_INFO("Moto Racer", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

namespace {

char g_error[256] = "";
uint64_t t0 = 0;

uint64_t now_us() {
    const uint64_t t = sceKernelGetSystemTimeWide();
    if (!t0) t0 = t;
    return t - t0;
}

// ---------- exit callback (the HOME menu's "Exit") ----------
int exit_callback(int, int, void *) {
    sceKernelExitGame();
    return 0;
}
int callback_thread(SceSize, void *) {
    int cb = sceKernelCreateCallback("Exit Callback", exit_callback, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

// ---------- events ----------
std::mutex ev_mutex;
std::deque<SDL_Event> events;
bool pad_announced = false;
SceCtrlData pad_now{}, pad_prev{};
bool ctrl_ready = false;

struct PadButton { unsigned mask; SDL_GamepadButton button; };
constexpr PadButton kButtons[] = {
    {PSP_CTRL_CROSS, SDL_GAMEPAD_BUTTON_SOUTH},        {PSP_CTRL_CIRCLE, SDL_GAMEPAD_BUTTON_EAST},
    {PSP_CTRL_SQUARE, SDL_GAMEPAD_BUTTON_WEST},        {PSP_CTRL_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH},
    {PSP_CTRL_LTRIGGER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {PSP_CTRL_RTRIGGER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {PSP_CTRL_SELECT, SDL_GAMEPAD_BUTTON_BACK},        {PSP_CTRL_START, SDL_GAMEPAD_BUTTON_START},
    {PSP_CTRL_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},         {PSP_CTRL_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {PSP_CTRL_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},     {PSP_CTRL_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};
constexpr SDL_JoystickID kPadId = 1;

void ctrl_init() {
    if (ctrl_ready) return;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    ctrl_ready = true;
}

int16_t stick(unsigned char v) {   // 0..255 (128 centre) -> SDL's -32768..32767
    const int s = (static_cast<int>(v) - 128) * 256;
    return static_cast<int16_t>(std::clamp(s, -32768, 32767));
}

void push(const SDL_Event &e) {
    std::lock_guard<std::mutex> lock(ev_mutex);
    events.push_back(e);
}

void poll_pad() {
    ctrl_init();
    const uint64_t ns = now_us() * 1000;
    if (!pad_announced) {
        pad_announced = true;
        SDL_Event e{};
        e.gdevice.type = SDL_EVENT_GAMEPAD_ADDED;
        e.gdevice.timestamp = ns;
        e.gdevice.which = kPadId;
        push(e);
    }
    pad_prev = pad_now;
    sceCtrlPeekBufferPositive(&pad_now, 1);
    for (const PadButton &b : kButtons) {
        const bool was = pad_prev.Buttons & b.mask, is = pad_now.Buttons & b.mask;
        if (was == is) continue;
        SDL_Event e{};
        e.gbutton.type = is ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.timestamp = ns;
        e.gbutton.which = kPadId;
        e.gbutton.button = static_cast<Uint8>(b.button);
        e.gbutton.down = is;
        push(e);
    }
    const unsigned char now_xy[2] = {pad_now.Lx, pad_now.Ly}, prev_xy[2] = {pad_prev.Lx, pad_prev.Ly};
    for (int a = 0; a < 2; a++) {
        if (now_xy[a] == prev_xy[a]) continue;
        SDL_Event e{};
        e.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        e.gaxis.timestamp = ns;
        e.gaxis.which = kPadId;
        e.gaxis.axis = static_cast<Uint8>(a == 0 ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_LEFTY);
        e.gaxis.value = stick(now_xy[a]);
        push(e);
    }
}

// ---------- audio ----------
// per output block (~46 ms at 44.1 kHz): the next block is mixed while this one plays, so a
// memory-stick read or a busy frame on the way must fit in it (1024 crackled on a PSP-1000)
constexpr int kSamples = 2048;
SDL_AudioSpec device_spec{SDL_AUDIO_S16LE, 2, 44100};
SDL_AudioPostmixCallback postmix = nullptr;
void *postmix_user = nullptr;
float device_gain = 1.f;

}  // namespace

struct SDL_AudioStream {
    SDL_AudioSpec src{};
    SDL_AudioStreamCallback get = nullptr;
    void *user = nullptr;
    std::vector<uint8_t> data;   // queued input (src format)
    int channel = -1;
    SceUID thread = -1;
    volatile bool running = false;
};

namespace {

int bytes_per_frame(const SDL_AudioSpec &s) { return SDL_AUDIO_BYTESIZE(s.format) * s.channels; }

int audio_thread(SceSize, void *argp) {
    SDL_AudioStream *s = *static_cast<SDL_AudioStream **>(argp);
    psp_fpu::quiet();   // the mixer's float code
    std::vector<int16_t> out(kSamples * 2);
    const int need = kSamples * bytes_per_frame(s->src);
    while (s->running) {
        const int have = static_cast<int>(s->data.size());
        if (have < need && s->get) s->get(s->user, s, need - have, need);
        const int n = std::min(need, static_cast<int>(s->data.size()));
        if (s->src.format == SDL_AUDIO_F32) {
            const float *f = reinterpret_cast<const float *>(s->data.data());
            const int vals = n / 4;
            for (int i = 0; i < kSamples * 2; i++)
                out[i] = i < vals ? static_cast<int16_t>(std::clamp(f[i] * 32767.f, -32768.f, 32767.f)) : 0;
        } else {
            std::memcpy(out.data(), s->data.data(), static_cast<size_t>(n));
            std::memset(reinterpret_cast<uint8_t *>(out.data()) + n, 0, static_cast<size_t>(kSamples * 4 - n));
        }
        s->data.erase(s->data.begin(), s->data.begin() + n);
        if (postmix) postmix(postmix_user, &device_spec, nullptr, 0);
        const int vol = static_cast<int>(PSP_AUDIO_VOLUME_MAX * std::clamp(device_gain, 0.f, 1.f));
        sceAudioOutputPannedBlocking(s->channel, vol, vol, out.data());
    }
    return 0;
}

// ---------- surfaces ----------
uint32_t read_pixel(const SDL_Surface *s, int x, int y) {   // -> 0xRRGGBB
    const uint8_t *row = static_cast<const uint8_t *>(s->pixels) + static_cast<size_t>(y) * s->pitch;
    auto x5 = [](unsigned v) { return (v << 3) | (v >> 2); };
    auto x6 = [](unsigned v) { return (v << 2) | (v >> 4); };
    auto x4 = [](unsigned v) { return v * 17; };
    if (SDL_BYTESPERPIXEL(s->format) == 4) {
        const uint8_t *p = row + x * 4;
        if (s->format == SDL_PIXELFORMAT_ABGR8888) return p[0] << 16 | p[1] << 8 | p[2];   // bytes R G B A
        return p[2] << 16 | p[1] << 8 | p[0];                                               // bytes B G R A
    }
    const unsigned v = reinterpret_cast<const uint16_t *>(row)[x];
    switch (s->format) {
    case SDL_PIXELFORMAT_RGB565: return x5(v >> 11) << 16 | x6((v >> 5) & 63) << 8 | x5(v & 31);
    case SDL_PIXELFORMAT_BGR565: return x5(v & 31) << 16 | x6((v >> 5) & 63) << 8 | x5(v >> 11);
    case SDL_PIXELFORMAT_ARGB1555: return x5((v >> 10) & 31) << 16 | x5((v >> 5) & 31) << 8 | x5(v & 31);
    case SDL_PIXELFORMAT_ABGR1555: return x5(v & 31) << 16 | x5((v >> 5) & 31) << 8 | x5((v >> 10) & 31);
    case SDL_PIXELFORMAT_ARGB4444: return x4((v >> 8) & 15) << 16 | x4((v >> 4) & 15) << 8 | x4(v & 15);
    case SDL_PIXELFORMAT_ABGR4444: return x4(v & 15) << 16 | x4((v >> 4) & 15) << 8 | x4((v >> 8) & 15);
    default: return 0;
    }
}

void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

}  // namespace

extern "C" {

// ---------- entry, init, errors ----------
int SDL_RunApp(int argc, char *argv[], SDL_main_func mainFunction, void *) {
    SceUID th = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
    static char name[] = "motoracer";
    static char *default_argv[] = {name, nullptr};
    if (argc <= 0 || !argv) { argc = 1; argv = default_argv; }
    const int r = mainFunction(argc, argv);
    sceKernelExitGame();
    return r;
}

bool SDL_Init(SDL_InitFlags flags) { return SDL_InitSubSystem(flags); }
bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (flags & (SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) ctrl_init();
    return true;
}
const char *SDL_GetError(void) { return g_error; }
bool SDL_SetError(SDL_PRINTF_FORMAT_STRING const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
    return false;
}
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags, const char *title, const char *message, SDL_Window *) {
    rt::log("%s: %s\n", title ? title : "", message ? message : "");
    return true;
}
void SDL_free(void *p) { free(p); }

// ---------- time ----------
Uint64 SDL_GetTicks(void) { return now_us() / 1000; }
Uint64 SDL_GetTicksNS(void) { return now_us() * 1000; }
void SDL_Delay(Uint32 ms) { sceKernelDelayThread(ms * 1000); }
void SDL_DelayPrecise(Uint64 ns) { sceKernelDelayThread(static_cast<SceUInt>(ns / 1000)); }

// ---------- window / display ----------
static SDL_DisplayMode g_mode = [] {
    SDL_DisplayMode m{};
    m.displayID = 1;
    m.format = SDL_PIXELFORMAT_BGR565;
    m.w = 480;
    m.h = 272;
    m.pixel_density = 1.f;
    m.refresh_rate = 60.f;
    return m;
}();
static int g_window_tag;
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *) { return 1; }
const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID) { return &g_mode; }
SDL_Window *SDL_CreateWindow(const char *, int, int, SDL_WindowFlags) { return reinterpret_cast<SDL_Window *>(&g_window_tag); }
void SDL_DestroyWindow(SDL_Window *) {}
bool SDL_SetWindowFullscreen(SDL_Window *, bool) { return true; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *) { return SDL_WINDOW_FULLSCREEN; }
SDL_WindowID SDL_GetWindowID(SDL_Window *) { return 1; }
bool SDL_GetWindowSize(SDL_Window *, int *w, int *h) {
    if (w) *w = 480;
    if (h) *h = 272;
    return true;
}

// ---------- events ----------
void SDL_PumpEvents(void) { poll_pad(); }
bool SDL_PollEvent(SDL_Event *event) {
    poll_pad();
    std::lock_guard<std::mutex> lock(ev_mutex);
    if (events.empty()) return false;
    if (event) *event = events.front();
    events.pop_front();
    return true;
}
bool SDL_PushEvent(SDL_Event *event) {
    if (!event) return false;
    push(*event);
    return true;
}

// ---------- gamepad (the built-in pad) ----------
static int g_pad_tag;
bool SDL_HasGamepad(void) { return true; }
SDL_JoystickID *SDL_GetGamepads(int *count) {
    auto *ids = static_cast<SDL_JoystickID *>(malloc(2 * sizeof(SDL_JoystickID)));
    ids[0] = kPadId;
    ids[1] = 0;
    if (count) *count = 1;
    return ids;
}
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    ctrl_init();
    return id == kPadId ? reinterpret_cast<SDL_Gamepad *>(&g_pad_tag) : nullptr;
}
void SDL_CloseGamepad(SDL_Gamepad *) {}
bool SDL_GamepadConnected(SDL_Gamepad *g) { return g != nullptr; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *g) { return g ? kPadId : 0; }
const char *SDL_GetGamepadName(SDL_Gamepad *) { return "PSP builtin joypad"; }
bool SDL_GetGamepadButton(SDL_Gamepad *, SDL_GamepadButton button) {
    for (const PadButton &b : kButtons)
        if (b.button == button) return pad_now.Buttons & b.mask;
    return false;
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *, SDL_GamepadAxis axis) {
    if (axis == SDL_GAMEPAD_AXIS_LEFTX) return stick(pad_now.Lx);
    if (axis == SDL_GAMEPAD_AXIS_LEFTY) return stick(pad_now.Ly);
    return 0;
}

// ---------- audio ----------
SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID, const SDL_AudioSpec *) { return 2; }
const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID) { return "PSP audio"; }
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID, SDL_AudioSpec *spec, int *sample_frames) {
    if (spec) *spec = device_spec;
    if (sample_frames) *sample_frames = kSamples;
    return true;
}
bool SDL_SetAudioPostmixCallback(SDL_AudioDeviceID, SDL_AudioPostmixCallback cb, void *user) {
    postmix = cb;
    postmix_user = user;
    return true;
}
bool SDL_SetAudioDeviceGain(SDL_AudioDeviceID, float gain) {
    device_gain = gain;
    return true;
}
SDL_AudioStream *SDL_CreateAudioStream(const SDL_AudioSpec *src, const SDL_AudioSpec *dst) {
    if (!src || src->channels != 2 || src->freq != device_spec.freq ||
        (src->format != SDL_AUDIO_S16LE && src->format != SDL_AUDIO_F32)) {
        SDL_SetError("PSP audio: unsupported stream format %x/%d/%d", src ? src->format : 0,
                     src ? src->channels : 0, src ? src->freq : 0);
        return nullptr;
    }
    (void)dst;
    auto *s = new SDL_AudioStream;
    s->src = *src;
    return s;
}
bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *s, SDL_AudioStreamCallback cb, void *user) {
    s->get = cb;
    s->user = user;
    return true;
}
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len) {
    const auto *p = static_cast<const uint8_t *>(buf);
    s->data.insert(s->data.end(), p, p + len);
    return true;
}
bool SDL_BindAudioStream(SDL_AudioDeviceID, SDL_AudioStream *s) {
    s->channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, kSamples, PSP_AUDIO_FORMAT_STEREO);
    if (s->channel < 0) return SDL_SetError("sceAudioChReserve failed: %x", s->channel);
    s->running = true;
    s->thread = sceKernelCreateThread("audio", audio_thread, 0x12, 0x8000, THREAD_ATTR_USER, nullptr);
    if (s->thread < 0) {
        sceAudioChRelease(s->channel);
        s->running = false;
        return SDL_SetError("audio thread: %x", s->thread);
    }
    SDL_AudioStream *arg = s;
    sceKernelStartThread(s->thread, sizeof arg, &arg);
    rt::log("audio: stream on channel %d (%s)\n", s->channel, s->src.format == SDL_AUDIO_F32 ? "f32" : "s16");
    return true;
}
void SDL_UnbindAudioStream(SDL_AudioStream *s) {
    if (!s || s->thread < 0) return;
    s->running = false;
    sceKernelWaitThreadEnd(s->thread, nullptr);
    sceKernelDeleteThread(s->thread);
    s->thread = -1;
    sceAudioChRelease(s->channel);
    s->channel = -1;
}
void SDL_DestroyAudioStream(SDL_AudioStream *s) {
    if (!s) return;
    SDL_UnbindAudioStream(s);
    delete s;
}

// ---------- surfaces (dev snapshots) ----------
SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch) {
    auto *s = static_cast<SDL_Surface *>(calloc(1, sizeof(SDL_Surface)));
    s->format = format;
    s->w = width;
    s->h = height;
    s->pitch = pitch;
    s->pixels = pixels;
    s->refcount = 1;
    return s;
}
void SDL_DestroySurface(SDL_Surface *s) { free(s); }
bool SDL_SaveBMP(SDL_Surface *s, const char *file) {   // 24-bit bottom-up BMP
    FILE *f = fopen(file, "wb");
    if (!f) return SDL_SetError("cannot write %s", file);
    const uint32_t row = static_cast<uint32_t>((s->w * 3 + 3) & ~3), size = row * static_cast<uint32_t>(s->h);
    fwrite("BM", 1, 2, f);
    put32(f, 54 + size); put32(f, 0); put32(f, 54);
    put32(f, 40); put32(f, static_cast<uint32_t>(s->w)); put32(f, static_cast<uint32_t>(s->h));
    put16(f, 1); put16(f, 24); put32(f, 0); put32(f, size); put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);
    std::vector<uint8_t> line(row, 0);
    for (int y = s->h - 1; y >= 0; y--) {
        for (int x = 0; x < s->w; x++) {
            const uint32_t c = read_pixel(s, x, y);
            line[x * 3] = c & 255;
            line[x * 3 + 1] = (c >> 8) & 255;
            line[x * 3 + 2] = (c >> 16) & 255;
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

}  // extern "C"
#endif
