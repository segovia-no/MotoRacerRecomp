// MCI "cdaudio" device emulation, the same approach as GOG's winmm.dll wrapper: the Moto Racer
// CD is a virtual disc in drive D: whose audio tracks 2..13 are the shipped NN.wav files.
//
// Disc layout (CD frames, 75 per second): 2 s lead-in, track 1 = data, 2 s pregap, then the
// audio tracks back to back. Track lengths come from the WAV sizes (2352 bytes per frame).
#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "host_audio.h"
#include "user32.h"
#include "win32.h"

using namespace rt;
using namespace w32;

namespace {

// ---------- MCI constants (mmsystem.h) ----------
constexpr uint32_t MCI_OPEN = 0x803, MCI_CLOSE = 0x804, MCI_PLAY = 0x806, MCI_SEEK = 0x807, MCI_STOP = 0x808,
                   MCI_PAUSE = 0x809, MCI_INFO = 0x80A, MCI_GETDEVCAPS = 0x80B, MCI_SET = 0x80D,
                   MCI_STATUS = 0x814, MCI_RESUME = 0x855;
constexpr uint32_t MCI_NOTIFY = 0x1, MCI_FROM = 0x4, MCI_TO = 0x8, MCI_TRACK = 0x10;
constexpr uint32_t MCI_OPEN_TYPE = 0x2000, MCI_OPEN_TYPE_ID = 0x1000, MCI_OPEN_ELEMENT = 0x200;
constexpr uint32_t MCI_STATUS_ITEM = 0x100, MCI_STATUS_START = 0x200;
constexpr uint32_t MCI_SET_DOOR_OPEN = 0x100, MCI_SET_TIME_FORMAT = 0x400;
constexpr uint32_t MCI_SEEK_TO_START = 0x100, MCI_SEEK_TO_END = 0x200;
constexpr uint32_t MCI_FORMAT_MILLISECONDS = 0, MCI_FORMAT_MSF = 2, MCI_FORMAT_TMSF = 10;
constexpr uint32_t MCI_MODE_STOP = 525, MCI_MODE_PLAY = 526, MCI_MODE_PAUSE = 529;
constexpr uint32_t MCI_CDA_TRACK_AUDIO = 1088, MCI_CDA_TRACK_OTHER = 1089;
constexpr uint32_t MCI_DEVTYPE_CD_AUDIO = 516;
constexpr uint32_t MCIERR_INVALID_DEVICE_ID = 257, MCIERR_DEVICE_TYPE_REQUIRED = 263,
                   MCIERR_UNRECOGNIZED_COMMAND = 261, MCIERR_OUTOFRANGE = 282, MCIERR_UNSUPPORTED_FUNCTION = 274;
constexpr uint32_t MM_MCINOTIFY = 0x3B9, MCI_NOTIFY_SUCCESSFUL = 1, MCI_NOTIFY_SUPERSEDED = 2,
                   MCI_NOTIFY_ABORTED = 4;
constexpr uint32_t DEVICE_ID = 1;
constexpr uint32_t BYTES_PER_FRAME = 2352;

struct Track {
    uint32_t start = 0, length = 0;   // CD frames
    bool audio = false;
    std::string path;
    uint32_t data_offset = 44;        // byte offset of PCM data in the WAV
};

std::vector<Track> tracks;            // index 0 = track 1
bool disc_loaded = false;
bool is_open = false;
uint32_t time_format = MCI_FORMAT_MSF;

// Playback state (shared with the audio thread).
std::mutex mu;
SDL_AudioStream *stream = nullptr;
uint32_t pos = 0, play_end = 0;       // absolute CD frames
uint32_t frame_carry = 0;             // bytes already consumed of frame `pos`
FILE *cur_file = nullptr;
int cur_track = -1;
std::atomic<bool> playing{false}, finished{false};
bool paused = false;
uint32_t notify_hwnd = 0;

uint32_t disc_end() { return tracks.empty() ? 0 : tracks.back().start + tracks.back().length; }

void load_disc() {
    if (disc_loaded) return;
    disc_loaded = true;
    const uint32_t data_track_frames = 25 * 60 * 75;   // nominal data track length
    tracks.push_back({150, data_track_frames, false, "", 0});
    uint32_t next = 150 + data_track_frames + 150;
    for (int t = 2; t <= 99; t++) {
        char name[16];
        snprintf(name, sizeof name, "%02d.wav", t);
        std::string p = host_path(name);
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) break;
        uint8_t hdr[12];
        fread(hdr, 1, 12, f);
        uint32_t data_off = 0, data_len = 0;
        for (;;) {   // walk RIFF chunks to "data"
            uint8_t ch[8];
            if (fread(ch, 1, 8, f) != 8) break;
            uint32_t len = ch[4] | ch[5] << 8 | ch[6] << 16 | static_cast<uint32_t>(ch[7]) << 24;
            if (!memcmp(ch, "data", 4)) { data_off = static_cast<uint32_t>(ftell(f)); data_len = len; break; }
            fseek(f, len + (len & 1), SEEK_CUR);
        }
        fclose(f);
        Track tr{next, data_len / BYTES_PER_FRAME, true, p, data_off};
        tracks.push_back(tr);
        next += tr.length;
    }
    log("CD: virtual disc with %zu tracks (%u audio), %u frames\n", tracks.size(), static_cast<unsigned>(tracks.size() - 1), disc_end());
}

int track_at(uint32_t frame) {
    for (size_t i = 0; i < tracks.size(); i++)
        if (frame >= tracks[i].start && frame < tracks[i].start + tracks[i].length) return static_cast<int>(i);
    return -1;
}

// ---------- time format conversion ----------
uint32_t msf(uint32_t frames) { return (frames / 4500) | ((frames / 75 % 60) << 8) | ((frames % 75) << 16); }

uint32_t encode(uint32_t abs) {
    switch (time_format) {
    case MCI_FORMAT_MILLISECONDS: return abs * 1000 / 75;
    case MCI_FORMAT_TMSF: {
        int t = track_at(abs);
        if (t < 0) t = abs >= disc_end() ? static_cast<int>(tracks.size()) - 1 : 0;
        uint32_t rel = abs >= tracks[t].start ? abs - tracks[t].start : 0;
        return static_cast<uint32_t>(t + 1) | (msf(rel) << 8);
    }
    default: return msf(abs);
    }
}

uint32_t encode_length(uint32_t frames) {   // lengths are durations: MSF even in TMSF mode
    return time_format == MCI_FORMAT_MILLISECONDS ? frames * 1000 / 75 : msf(frames);
}

uint32_t decode(uint32_t v) {
    switch (time_format) {
    case MCI_FORMAT_MILLISECONDS: return v * 75 / 1000;
    case MCI_FORMAT_TMSF: {
        uint32_t t = v & 0xFF;
        if (t < 1 || t > tracks.size()) return 0;
        return tracks[t - 1].start + ((v >> 8) & 0xFF) * 4500 + ((v >> 16) & 0xFF) * 75 + ((v >> 24) & 0xFF);
    }
    default: return (v & 0xFF) * 4500 + ((v >> 8) & 0xFF) * 75 + ((v >> 16) & 0xFF);
    }
}

// ---------- playback (audio thread pulls) ----------
int pull(uint8_t *dst, int bytes) {
    std::lock_guard<std::mutex> lock(mu);
    int written = 0;
    while (written < bytes && playing && !paused) {
        if (pos >= play_end) { playing = false; finished = true; break; }
        int t = track_at(pos);
        if (t < 0 || !tracks[t].audio) {   // data track / gaps: play silence
            int n = std::min(bytes - written, static_cast<int>(BYTES_PER_FRAME - frame_carry));
            memset(dst + written, 0, static_cast<size_t>(n));
            written += n;
            frame_carry += static_cast<uint32_t>(n);
        } else {
            if (t != cur_track) {
                if (cur_file) fclose(cur_file);
                cur_file = fopen(tracks[t].path.c_str(), "rb");
                cur_track = t;
            }
            if (!cur_file) { playing = false; finished = true; break; }
            uint32_t off = tracks[t].data_offset + (pos - tracks[t].start) * BYTES_PER_FRAME + frame_carry;
            fseek(cur_file, off, SEEK_SET);
            uint32_t frames_left_track = tracks[t].start + tracks[t].length - pos;
            uint32_t frames_left = std::min(frames_left_track, play_end - pos);
            uint64_t avail = static_cast<uint64_t>(frames_left) * BYTES_PER_FRAME - frame_carry;
            int n = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(bytes - written), avail));
            size_t got = fread(dst + written, 1, static_cast<size_t>(n), cur_file);
            if (got < static_cast<size_t>(n)) memset(dst + written + got, 0, static_cast<size_t>(n) - got);
            written += n;
            frame_carry += static_cast<uint32_t>(n);
        }
        pos += frame_carry / BYTES_PER_FRAME;
        frame_carry %= BYTES_PER_FRAME;
    }
    return written;
}

void notify(uint32_t code) {
    if (notify_hwnd) post_message(notify_hwnd, MM_MCINOTIFY, code, DEVICE_ID);
    notify_hwnd = 0;
}

void stop_playback(bool aborted) {
    {
        std::lock_guard<std::mutex> lock(mu);
        playing = false;
        paused = false;
    }
    if (notify_hwnd) notify(aborted ? MCI_NOTIFY_ABORTED : MCI_NOTIFY_SUPERSEDED);
}

void start_playback(uint32_t from, uint32_t to) {
    if (!stream) {
        SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 44100};
        stream = host::audio_open_source(spec, pull);
    }
    std::lock_guard<std::mutex> lock(mu);
    pos = from;
    play_end = std::min(to, disc_end());
    frame_carry = 0;
    paused = false;
    finished = false;
    playing = true;
}

uint32_t current_mode() { return playing ? (paused ? MCI_MODE_PAUSE : MCI_MODE_PLAY) : MCI_MODE_STOP; }

uint32_t callback_hwnd(uint32_t parms) { return parms ? RD32(parms) & 0xFFFF : 0; }

uint32_t do_command(uint32_t dev, uint32_t msg, uint32_t flags, uint32_t parms) {
    if (msg == MCI_OPEN) {
        bool cd = false;
        if (flags & MCI_OPEN_TYPE_ID) cd = (RD32(parms + 8) & 0xFFFF) == MCI_DEVTYPE_CD_AUDIO;
        else if (flags & MCI_OPEN_TYPE) cd = strcasecmp(read_cstr(RD32(parms + 8)).c_str(), "cdaudio") == 0;
        std::string elem = (flags & MCI_OPEN_ELEMENT) ? read_cstr(RD32(parms + 12)) : "";
        log("MCI_OPEN type %s element \"%s\"\n",
            (flags & MCI_OPEN_TYPE_ID) ? std::to_string(RD32(parms + 8) & 0xFFFF).c_str() : read_cstr(RD32(parms + 8)).c_str(),
            elem.c_str());
        if (!cd) return MCIERR_DEVICE_TYPE_REQUIRED;
        // Like GOG's winmm.dll wrapper, the virtual disc answers on whichever drive the game
        // asks for (it derives the letter from its install path, normally C:).
        load_disc();
        is_open = true;
        time_format = MCI_FORMAT_MSF;
        WR32(parms + 4, DEVICE_ID);
        return 0;
    }
    if (dev != DEVICE_ID || !is_open) return MCIERR_INVALID_DEVICE_ID;
    switch (msg) {
    case MCI_CLOSE:
        stop_playback(true);
        is_open = false;
        return 0;
    case MCI_SET:
        if (flags & MCI_SET_TIME_FORMAT) time_format = RD32(parms + 4);
        if (flags & MCI_SET_DOOR_OPEN) stop_playback(true);
        return 0;
    case MCI_STATUS: {
        if (!(flags & MCI_STATUS_ITEM)) return MCIERR_UNSUPPORTED_FUNCTION;
        uint32_t item = RD32(parms + 8), track = RD32(parms + 12), r = 0;
        bool has_track = flags & MCI_TRACK;
        if (has_track && (track < 1 || track > tracks.size())) return MCIERR_OUTOFRANGE;
        switch (item) {
        case 1: r = has_track ? encode_length(tracks[track - 1].length) : encode_length(disc_end() - 150); break;
        case 2:
            if (has_track) r = encode(tracks[track - 1].start);
            else if (flags & MCI_STATUS_START) r = encode(150);
            else r = encode(pos);
            break;
        case 3: r = static_cast<uint32_t>(tracks.size()); break;
        case 4: r = current_mode(); break;
        case 5: r = 1; break;                                  // media present
        case 6: r = time_format; break;
        case 7: r = 1; break;                                  // ready
        case 8: { int t = track_at(pos); r = t < 0 ? 1 : static_cast<uint32_t>(t + 1); break; }
        case 0x4001: r = (has_track && tracks[track - 1].audio) ? MCI_CDA_TRACK_AUDIO : MCI_CDA_TRACK_OTHER; break;
        default: return MCIERR_UNSUPPORTED_FUNCTION;
        }
        WR32(parms + 4, r);
        return 0;
    }
    case MCI_PLAY: {
        uint32_t from = (flags & MCI_FROM) ? decode(RD32(parms + 4)) : pos;
        uint32_t to = (flags & MCI_TO) ? decode(RD32(parms + 8)) : disc_end();
        if (from >= disc_end() || to <= from) return MCIERR_OUTOFRANGE;
        if (notify_hwnd) notify(MCI_NOTIFY_SUPERSEDED);
        start_playback(from, to);
        if (flags & MCI_NOTIFY) notify_hwnd = callback_hwnd(parms);
        return 0;
    }
    case MCI_STOP:
        stop_playback(true);
        return 0;
    case MCI_PAUSE: { std::lock_guard<std::mutex> lock(mu); paused = true; return 0; }
    case MCI_RESUME: { std::lock_guard<std::mutex> lock(mu); paused = false; return 0; }
    case MCI_SEEK:
        stop_playback(true);
        if (flags & MCI_SEEK_TO_START) pos = 150;
        else if (flags & MCI_SEEK_TO_END) pos = disc_end();
        else if (flags & MCI_TO) pos = decode(RD32(parms + 4));
        return 0;
    case MCI_INFO:
        if (RD32(parms + 8) && RD32(parms + 12)) write_cstr(RD32(parms + 8), "CD Audio", RD32(parms + 12));
        return 0;
    case MCI_GETDEVCAPS:
        WR32(parms + 4, 1);
        return 0;
    default:
        return MCIERR_UNRECOGNIZED_COMMAND;
    }
}

}  // namespace

namespace w32 {
// Called from the message pump: delivers MM_MCINOTIFY when a play request completes.
void cd_audio_tick() {
    if (finished.exchange(false)) notify(MCI_NOTIFY_SUCCESSFUL);
}
}  // namespace w32

STDCALL(WINMM, mciSendCommandA, 4) {
    uint32_t dev = arg(0), msg = arg(1), flags = arg(2), parms = arg(3);
    uint32_t r = do_command(dev, msg, flags, parms);
    if (trace_imports || r || msg == MCI_OPEN || msg == MCI_PLAY)
        log("mciSendCommandA(dev %u, msg 0x%x, flags 0x%x) -> %u\n", dev, msg, flags, r);
    if (!r && msg == MCI_STATUS) {
        static int status_logs = 0;
        if (status_logs++ < 40)
            log("  status item %u track %u -> 0x%08x\n", RD32(parms + 8), RD32(parms + 12), RD32(parms + 4));
    }
    return r;
}

STDCALL(WINMM, mciGetErrorStringA, 3) {
    char b[64];
    snprintf(b, sizeof b, "MCI error %u", arg(0));
    write_cstr(arg(1), b, arg(2));
    return 1;
}
