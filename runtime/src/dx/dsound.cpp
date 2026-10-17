// DirectSound (DirectX 3/5) with its own software mixer on one SDL3 audio stream.
//
// Secondary buffers keep their samples in guest memory so Lock returns real guest pointers.
// Buffers that have been played are voices of one mixer: on SDL's audio thread it produces float
// stereo at the device's rate, each voice stepped at its frequency (SetFrequency: the engines'
// pitch) with linear interpolation, DirectSound volume / pan applied. Until 2026-10-17 every
// buffer was an SDL stream of its own, and SDL resampled ~24 engine sounds separately: 12-18% of a
// PSP race frame (docs/26-psp-profile.md). The primary buffer is a formality.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include "com.h"
#include "host_audio.h"

using namespace rt;
using namespace dx;

namespace {

constexpr uint32_t DSERR_INVALIDPARAM = E_INVALIDARG;
constexpr uint32_t DSERR_NODRIVER = 0x88780078;
constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 0x1;
constexpr uint32_t DSBPLAY_LOOPING = 0x1;
constexpr uint32_t DSBSTATUS_PLAYING = 0x1, DSBSTATUS_LOOPING = 0x4;

const char *IID_IDirectSoundBuffer = "279AFA85-4981-11CE-A521-0020AF0BE560";

std::mutex mu;   // guards buffer state shared with the audio thread

struct Format {
    uint16_t tag = 1, channels = 2, block = 4, bits = 16;
    uint32_t rate = 22050;
};

struct Buffer : ComObj {
    bool primary = false;
    uint32_t flags = 0;
    Format fmt;
    uint32_t mem = 0, size = 0;   // guest sample memory (shared by duplicates)
    std::shared_ptr<int> mem_owner;
    uint32_t play_pos = 0;        // bytes, a multiple of the block size
    uint32_t frac = 0;            // position between two frames (16-bit fraction), the mixer's
    bool playing = false, looping = false, voice = false;
    int32_t volume = 0, pan = 0;
    uint32_t freq = 0;            // 0 = original
    ~Buffer() override;
};

std::vector<Buffer *> voices;     // buffers that have been played (under mu)
SDL_AudioStream *mix_stream = nullptr;
uint32_t mix_rate = 44100;

Buffer::~Buffer() {
    if (voice) {
        std::lock_guard<std::mutex> lock(mu);
        voices.erase(std::remove(voices.begin(), voices.end(), this), voices.end());
    }
    if (mem_owner && mem_owner.use_count() == 1 && mem) heap_free(mem);
}

struct Device : ComObj {
    uint32_t hwnd = 0, level = 0;
};

uint32_t vt_ds, vt_buf;

Format read_format(uint32_t p) {
    Format f;
    if (!p) return f;
    f.tag = RD16(p);
    f.channels = RD16(p + 2);
    f.rate = RD32(p + 4);
    f.block = RD16(p + 12);
    f.bits = RD16(p + 14);
    return f;
}

float db_to_gain(int32_t hundredths) { return hundredths <= -10000 ? 0.f : std::pow(10.f, hundredths / 2000.f); }

// One voice into `out` (float stereo, `frames` frames, added): steps through the buffer at its
// frequency relative to the mix rate (16.16 fixed point) with linear interpolation between frames.
template <int BITS, int CH>
void mix_voice(Buffer *b, float *out, int frames, float gl, float gr) {
    const uint32_t bs = b->fmt.block ? b->fmt.block : 1;
    const uint32_t count = b->size / bs;
    if (!count) { b->playing = false; return; }
    const uint32_t rate = b->freq ? b->freq : b->fmt.rate;
    const uint32_t step = static_cast<uint32_t>((static_cast<uint64_t>(rate) << 16) / mix_rate);
    const uint8_t *base = M + b->mem;
    auto sample = [&](uint32_t i, int c) -> float {
        const uint8_t *s = base + i * bs;
        if (BITS == 8) return (s[c] - 128) * (1.f / 128.f);
        int16_t v;
        memcpy(&v, s + 2 * c, 2);
        return v * (1.f / 32768.f);
    };
    uint32_t pos = b->play_pos / bs, frac = b->frac;
    if (pos >= count) pos = 0;
    for (int n = 0; n < frames; n++) {
        const uint32_t next = pos + 1 < count ? pos + 1 : (b->looping ? 0 : pos);
        const float t = static_cast<float>(frac) * (1.f / 65536.f);
        const float l0 = sample(pos, 0), l1 = sample(next, 0);
        const float l = l0 + (l1 - l0) * t;
        float r = l;
        if (CH == 2) {
            const float r0 = sample(pos, 1), r1 = sample(next, 1);
            r = r0 + (r1 - r0) * t;
        }
        out[2 * n] += l * gl;
        out[2 * n + 1] += r * gr;
        frac += step;
        pos += frac >> 16;
        frac &= 0xffff;
        if (pos >= count) {
            if (!b->looping) { b->playing = false; pos = 0; frac = 0; break; }
            pos %= count;
        }
    }
    b->play_pos = pos * bs;
    b->frac = frac;
}

// Audio thread: all playing voices, float stereo at the mix rate. 0 when nothing plays.
int mix(uint8_t *dst, int bytes) {
    std::lock_guard<std::mutex> lock(mu);
    const int frames = bytes / 8;
    auto *out = reinterpret_cast<float *>(dst);
    bool any = false;
    for (Buffer *b : voices) {
        if (!b->playing || !b->size) continue;
        if (!any) { memset(out, 0, static_cast<size_t>(frames) * 8); any = true; }
        const float vol = db_to_gain(b->volume) * host::sfx_gain();
        const float gl = vol * (b->pan > 0 ? db_to_gain(-b->pan) : 1.f);
        const float gr = vol * (b->pan < 0 ? db_to_gain(b->pan) : 1.f);
        const bool stereo = b->fmt.channels > 1;
        if (b->fmt.bits == 8) stereo ? mix_voice<8, 2>(b, out, frames, gl, gr) : mix_voice<8, 1>(b, out, frames, gl, gr);
        else stereo ? mix_voice<16, 2>(b, out, frames, gl, gr) : mix_voice<16, 1>(b, out, frames, gl, gr);
    }
    return any ? frames * 8 : 0;
}

// a played buffer becomes a voice (and the mixer's stream opens on the first one)
void make_voice(Buffer *b) {
    if (b->primary || b->voice) return;
    if (!mix_stream) {
        mix_rate = host::audio_device_rate();
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(mix_rate)};
        mix_stream = host::audio_open_source(spec, mix);
    }
    std::lock_guard<std::mutex> lock(mu);
    voices.push_back(b);
    b->voice = true;
}

Buffer *new_buffer(uint32_t desc) {
    auto *b = new Buffer;
    b->flags = RD32(desc + 4);
    b->primary = b->flags & DSBCAPS_PRIMARYBUFFER;
    if (!b->primary) {
        b->fmt = read_format(RD32(desc + 16));
        b->size = RD32(desc + 8);
        b->mem = heap_alloc(b->size + 16, true);
        b->mem_owner = std::make_shared<int>(0);
        if (b->fmt.bits == 8) memset(M + b->mem, 0x80, b->size);
    } else {
        b->size = 4096;
    }
    return b;
}

// =====================================================================================
// IDirectSound
// =====================================================================================
COM_METHOD(DS_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(DS_AddRef, 0) { return add_ref(self); }
COM_METHOD(DS_Release, 0) { return release(self); }

COM_METHOD(DS_CreateSoundBuffer, 3) {
    uint32_t desc = arg(1);
    if (!desc) return DSERR_INVALIDPARAM;
    Buffer *b = new_buffer(desc);
    if (!b->primary)
        log("CreateSoundBuffer %u bytes, %u Hz %u-bit %u ch, flags %x\n", b->size, b->fmt.rate, b->fmt.bits,
            b->fmt.channels, b->flags);
    WR32(arg(2), expose(b, "IDirectSoundBuffer", vt_buf));
    return S_OK;
}

COM_METHOD(DS_GetCaps, 1) {
    uint32_t p = arg(1);
    uint32_t size = RD32(p) >= 8 && RD32(p) <= 96 ? RD32(p) : 96;
    memset(M + p + 4, 0, size - 4);
    WR32(p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800);   // mono/stereo, 8/16, continuous rate
    WR32(p + 8, 100);
    WR32(p + 12, 100000);
    WR32(p + 16, 1);
    return S_OK;
}

COM_METHOD(DS_DuplicateSoundBuffer, 2) {
    Buffer *src = dynamic_cast<Buffer *>(lookup(arg(1)));
    if (!src) return DSERR_INVALIDPARAM;
    auto *b = new Buffer;
    {
        std::lock_guard<std::mutex> lock(mu);
        b->flags = src->flags;
        b->fmt = src->fmt;
        b->mem = src->mem;
        b->size = src->size;
        b->mem_owner = src->mem_owner;
        b->volume = src->volume;
        b->pan = src->pan;
        b->freq = src->freq;
    }
    WR32(arg(2), expose(b, "IDirectSoundBuffer", vt_buf));
    return S_OK;
}

COM_METHOD(DS_SetCooperativeLevel, 2) {
    Device *d = obj<Device>(self);
    d->hwnd = arg(1);
    d->level = arg(2);
    return S_OK;
}
COM_METHOD(DS_Compact, 0) { return S_OK; }
COM_METHOD(DS_GetSpeakerConfig, 1) { WR32(arg(1), 4); return S_OK; }   // DSSPEAKER_STEREO
COM_METHOD(DS_SetSpeakerConfig, 1) { return S_OK; }
COM_METHOD(DS_Initialize, 1) { return S_OK; }

// =====================================================================================
// IDirectSoundBuffer
// =====================================================================================
COM_METHOD(B_QueryInterface, 2) {
    Guid g = read_guid(arg(1));
    if (guid_eq(g, IID_IDirectSoundBuffer)) {
        add_ref(self);
        WR32(arg(2), self);
        return S_OK;
    }
    log("IDirectSoundBuffer::QueryInterface(%s) -> E_NOINTERFACE\n", guid_str(g).c_str());
    WR32(arg(2), 0);
    return E_NOINTERFACE;
}
COM_METHOD(B_AddRef, 0) { return add_ref(self); }
COM_METHOD(B_Release, 0) {
    // Stop under the lock; the destructor takes it again to leave the mixer's voices.
    if (Buffer *b = dynamic_cast<Buffer *>(lookup(self)); b && b->refs == 1) {
        std::lock_guard<std::mutex> lock(mu);
        b->playing = false;
    }
    return release(self);
}

COM_METHOD(B_GetCaps, 1) {
    Buffer *b = obj<Buffer>(self);
    WR32(arg(1) + 4, b->flags | 0x8);   // LOCSOFTWARE
    WR32(arg(1) + 8, b->size);
    WR32(arg(1) + 12, 0);
    WR32(arg(1) + 16, 0);
    return S_OK;
}

COM_METHOD(B_GetCurrentPosition, 2) {
    Buffer *b = obj<Buffer>(self);
    std::lock_guard<std::mutex> lock(mu);
    uint32_t lead = (b->fmt.rate * b->fmt.block / 100) & ~(static_cast<uint32_t>(b->fmt.block) - 1);   // ~10 ms
    if (arg(1)) WR32(arg(1), b->play_pos);
    if (arg(2)) WR32(arg(2), b->size ? (b->play_pos + lead) % b->size : 0);
    return S_OK;
}

COM_METHOD(B_GetFormat, 3) {
    Buffer *b = obj<Buffer>(self);
    if (arg(1) && arg(2) >= 16) {
        WR16(arg(1), b->fmt.tag);
        WR16(arg(1) + 2, b->fmt.channels);
        WR32(arg(1) + 4, b->fmt.rate);
        WR32(arg(1) + 8, b->fmt.rate * b->fmt.block);
        WR16(arg(1) + 12, b->fmt.block);
        WR16(arg(1) + 14, b->fmt.bits);
        if (arg(2) >= 18) WR16(arg(1) + 16, 0);
    }
    if (arg(3)) WR32(arg(3), 18);
    return S_OK;
}

COM_METHOD(B_GetVolume, 1) { WR32(arg(1), static_cast<uint32_t>(obj<Buffer>(self)->volume)); return S_OK; }
COM_METHOD(B_GetPan, 1) { WR32(arg(1), static_cast<uint32_t>(obj<Buffer>(self)->pan)); return S_OK; }
COM_METHOD(B_GetFrequency, 1) {
    Buffer *b = obj<Buffer>(self);
    WR32(arg(1), b->freq ? b->freq : b->fmt.rate);
    return S_OK;
}

COM_METHOD(B_GetStatus, 1) {
    Buffer *b = obj<Buffer>(self);
    std::lock_guard<std::mutex> lock(mu);
    WR32(arg(1), (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0));
    return S_OK;
}

COM_METHOD(B_Initialize, 2) { return S_OK; }

COM_METHOD(B_Lock, 7) {
    Buffer *b = obj<Buffer>(self);
    uint32_t off = arg(1), bytes = arg(2), flags = arg(7);
    if (flags & 0x2) bytes = b->size;                 // DSBLOCK_ENTIREBUFFER
    if (flags & 0x1) off = b->play_pos + 0;           // DSBLOCK_FROMWRITECURSOR
    if (!b->size) return DSERR_INVALIDPARAM;
    off %= b->size;
    if (bytes > b->size) bytes = b->size;
    uint32_t first = std::min(bytes, b->size - off);
    WR32(arg(3), b->mem + off);
    WR32(arg(4), first);
    if (arg(5)) WR32(arg(5), bytes > first ? b->mem : 0);
    if (arg(6)) WR32(arg(6), bytes - first);
    return S_OK;
}

COM_METHOD(B_Play, 3) {
    Buffer *b = obj<Buffer>(self);
    if (b->primary) return S_OK;
    make_voice(b);
    std::lock_guard<std::mutex> lock(mu);
    b->looping = arg(3) & DSBPLAY_LOOPING;
    b->playing = true;
    return S_OK;
}

COM_METHOD(B_SetCurrentPosition, 1) {
    Buffer *b = obj<Buffer>(self);
    std::lock_guard<std::mutex> lock(mu);
    const uint32_t bs = b->fmt.block ? b->fmt.block : 1;
    b->play_pos = b->size ? (arg(1) % b->size) / bs * bs : 0;
    b->frac = 0;
    return S_OK;
}

COM_METHOD(B_SetFormat, 1) {
    Buffer *b = obj<Buffer>(self);
    if (b->primary) b->fmt = read_format(arg(1));
    return S_OK;
}

COM_METHOD(B_SetVolume, 1) {
    std::lock_guard<std::mutex> lock(mu);
    obj<Buffer>(self)->volume = static_cast<int32_t>(arg(1));
    return S_OK;
}

COM_METHOD(B_SetPan, 1) {
    std::lock_guard<std::mutex> lock(mu);
    obj<Buffer>(self)->pan = static_cast<int32_t>(arg(1));
    return S_OK;
}

COM_METHOD(B_SetFrequency, 1) {
    std::lock_guard<std::mutex> lock(mu);
    obj<Buffer>(self)->freq = arg(1);
    return S_OK;
}

COM_METHOD(B_Stop, 0) {
    Buffer *b = obj<Buffer>(self);
    std::lock_guard<std::mutex> lock(mu);
    b->playing = false;
    return S_OK;
}

COM_METHOD(B_Unlock, 4) { return S_OK; }
COM_METHOD(B_Restore, 0) { return S_OK; }

void init_vtables() {
    if (vt_ds) return;
    vt_ds = make_vtable("IDirectSound", {DS_QueryInterface, DS_AddRef, DS_Release, DS_CreateSoundBuffer, DS_GetCaps,
                                         DS_DuplicateSoundBuffer, DS_SetCooperativeLevel, DS_Compact,
                                         DS_GetSpeakerConfig, DS_SetSpeakerConfig, DS_Initialize}, 11);
    vt_buf = make_vtable("IDirectSoundBuffer", {B_QueryInterface, B_AddRef, B_Release, B_GetCaps, B_GetCurrentPosition,
                                                B_GetFormat, B_GetVolume, B_GetPan, B_GetFrequency, B_GetStatus,
                                                B_Initialize, B_Lock, B_Play, B_SetCurrentPosition, B_SetFormat,
                                                B_SetVolume, B_SetPan, B_SetFrequency, B_Stop, B_Unlock, B_Restore}, 21);
}

}  // namespace

STDCALL(DSOUND, DirectSoundCreate, 3) {
    if (!host::audio_init()) {
        log("DirectSoundCreate -> DSERR_NODRIVER (no host audio device)\n");
        if (arg(1)) WR32(arg(1), 0);
        return DSERR_NODRIVER;
    }
    init_vtables();
    auto *d = new Device;
    WR32(arg(1), expose(d, "IDirectSound", vt_ds));
    log("DirectSoundCreate -> 0x%08x\n", RD32(arg(1)));
    return S_OK;
}
