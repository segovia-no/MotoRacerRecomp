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

// Volumes from the game's Options page (WINMM mixer), 0..1.
void set_master_gain(float g);
void set_sfx_gain(float g);     // DirectSound
void set_music_gain(float g);   // CD audio
float sfx_gain();
float music_gain();

}  // namespace host
