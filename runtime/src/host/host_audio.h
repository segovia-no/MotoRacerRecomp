// Host audio: one SDL playback device; each sound source (DirectSound's mixer, the CD music) is an
// SDL_AudioStream bound to it. Sources pull data through a callback on SDL's audio thread.
#pragma once
#include <SDL3/SDL_audio.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace host {

bool audio_init();   // false if no audio device (game then runs silent)

// Creates a stream whose source format is `spec`; `pull(dst, bytes)` must fill up to `bytes`
// and return how many it wrote (fewer = source exhausted for now). Runs on the audio thread.
SDL_AudioStream *audio_open_source(const SDL_AudioSpec &spec,
                                   std::function<int(uint8_t *dst, int bytes)> pull);
void audio_close_source(SDL_AudioStream *s);
uint32_t audio_device_rate();   // the playback device's sample rate (44100 without a device)

// Volumes from the game's Options page (WINMM mixer), 0..1.
void set_master_gain(float g);
void set_sfx_gain(float g);     // DirectSound
void set_music_gain(float g);   // CD audio
float sfx_gain();
float music_gain();

// A compressed CD-audio track, decoded to 16-bit stereo 44.1 kHz PCM (the WAV tracks' format).
// Used from the audio thread only.
class TrackDecoder {
public:
    virtual ~TrackDecoder() = default;
    virtual int read(uint8_t *dst, int bytes) = 0;   // fewer than `bytes` at the end of the track
    virtual void seek(uint64_t pcm_byte) = 0;
};
// NN.mp3 (scripts/psp/cd_mp3.sh): the PSP decodes it on the Media Engine (runtime/src/psp/mp3_psp.cpp);
// nullptr on other hosts (they play the WAVs) or when the decoder is unavailable.
std::unique_ptr<TrackDecoder> open_mp3_track(const std::string &path);

}  // namespace host
