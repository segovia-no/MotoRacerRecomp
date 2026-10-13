#include "host_audio.h"

#include <SDL3/SDL.h>

#include <map>
#include <memory>
#include <vector>

#include "runtime.h"

using namespace rt;

namespace host {

namespace {
SDL_AudioDeviceID device = 0;
bool tried = false;

struct Source {
    std::function<int(uint8_t *, int)> pull;
    std::vector<uint8_t> scratch;
};
std::map<SDL_AudioStream *, std::unique_ptr<Source>> sources;

void SDLCALL feed(void *userdata, SDL_AudioStream *stream, int additional, int) {
    auto *src = static_cast<Source *>(userdata);
    if (additional <= 0) return;
    src->scratch.resize(static_cast<size_t>(additional));
    int n = src->pull(src->scratch.data(), additional);
    if (n > 0) SDL_PutAudioStreamData(stream, src->scratch.data(), n);
}
}  // namespace

bool audio_init() {
    if (tried) return device != 0;
    tried = true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log("audio: SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 44100};
    device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!device) {
        log("audio: cannot open playback device: %s\n", SDL_GetError());
        return false;
    }
    log("audio: device opened (%s)\n", SDL_GetAudioDeviceName(device));
    return true;
}

SDL_AudioStream *audio_open_source(const SDL_AudioSpec &spec, std::function<int(uint8_t *, int)> pull) {
    if (!audio_init()) return nullptr;
    SDL_AudioSpec out{};
    SDL_GetAudioDeviceFormat(device, &out, nullptr);
    SDL_AudioStream *s = SDL_CreateAudioStream(&spec, &out);
    if (!s) return nullptr;
    auto src = std::make_unique<Source>();
    src->pull = std::move(pull);
    SDL_SetAudioStreamGetCallback(s, feed, src.get());
    sources[s] = std::move(src);
    SDL_BindAudioStream(device, s);
    return s;
}

void audio_close_source(SDL_AudioStream *s) {
    if (!s) return;
    SDL_UnbindAudioStream(s);
    SDL_DestroyAudioStream(s);
    sources.erase(s);
}

}  // namespace host
