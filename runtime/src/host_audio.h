// Host audio: one SDL playback device; every sound source is an SDL_AudioStream bound to it
// (SDL mixes them). Sources pull data through a callback on SDL's audio thread.
#pragma once
#include <SDL3/SDL_audio.h>

#include <cstdint>
#include <functional>

namespace host {

bool audio_init();   // false if no audio device (game then runs silent)

// Creates a stream whose source format is `spec`; `pull(dst, bytes)` must fill up to `bytes`
// and return how many it wrote (fewer = source exhausted for now). Runs on the audio thread.
SDL_AudioStream *audio_open_source(const SDL_AudioSpec &spec,
                                   std::function<int(uint8_t *dst, int bytes)> pull);
void audio_close_source(SDL_AudioStream *s);

}  // namespace host
