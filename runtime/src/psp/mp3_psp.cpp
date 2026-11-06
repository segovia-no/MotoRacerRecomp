// CD-audio tracks as MP3 on the PSP (host::open_mp3_track): the firmware's sceMp3 library decodes on
// the Media Engine, so the main CPU only copies PCM. tools/psp/cd_mp3.sh writes the files (CBR, no
// ID3 / Xing header). The first track loads the AV modules (avcodec, mp3) and the decoder resource.
//
// Everything here runs on the audio thread, which has ~46 ms per block: it never waits for the
// memory stick (the game loads files at the same time). The file is read ahead in two 16 KB chunks
// with sceIoReadAsync (~1 s of music each); sceMp3 takes its stream data from them by memcpy. And
// it never writes the log file itself (rt::log_queued).
#ifdef __PSP__
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmp3.h>
#include <psputility.h>

#include "host_audio.h"
#include "runtime.h"

namespace {

constexpr int kMp3Buf = 16 * 1024;   // sceMp3's stream data (>= 8192, + workspace)
constexpr int kPcmBuf = 2 * 4608;    // two decoded frames (>= 9216)
constexpr int kChunk = 16 * 1024;    // read-ahead chunk (128 kbps: ~1 s)
constexpr uint32_t kFrameBytes = 1152 * 4;

// One track plays at a time: the buffers are allocated once and kept (no heap churn during loads).
struct Buffers {
    uint8_t *mp3 = nullptr, *pcm = nullptr, *chunk[2] = {nullptr, nullptr};
};
Buffers *buffers() {
    static Buffers b;
    if (!b.mp3) {
        b.mp3 = static_cast<uint8_t *>(memalign(64, kMp3Buf));
        b.pcm = static_cast<uint8_t *>(memalign(64, kPcmBuf));
        b.chunk[0] = static_cast<uint8_t *>(memalign(64, kChunk));
        b.chunk[1] = static_cast<uint8_t *>(memalign(64, kChunk));
    }
    return (b.mp3 && b.pcm && b.chunk[0] && b.chunk[1]) ? &b : nullptr;
}

bool decoder_ready() {
    static int state = 0;   // 0 untried, 1 ready, -1 unavailable
    if (state) return state > 0;
    const SceSize before = sceKernelMaxFreeMemSize();
    int a = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC), m = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    if ((a < 0 && a != static_cast<int>(0x80020139)) || (m < 0 && m != static_cast<int>(0x80020139))) {
        rt::log_queued("CD: MP3 modules not loaded (avcodec %x, mp3 %x): no music\n", a, m);
        state = -1;
        return false;
    }
    int r = sceMp3InitResource();
    if (r < 0) {
        rt::log_queued("CD: sceMp3InitResource %x: no music\n", r);
        state = -1;
        return false;
    }
    rt::log_queued("CD: MP3 decoder ready (kernel free %u -> %u KB)\n", static_cast<unsigned>(before >> 10),
                   static_cast<unsigned>(sceKernelMaxFreeMemSize() >> 10));
    state = 1;
    return true;
}

// The file in two read-ahead chunks; reads go to the memory stick in the background.
class Reader {
public:
    ~Reader() {
        if (fd < 0) return;
        wait();
        sceIoClose(fd);
    }

    bool open(const std::string &path, uint8_t *c0, uint8_t *c1) {
        // sceIo does not know libc's working directory (the game folder): absolute path
        std::string full = path;
        if (path.find(':') == std::string::npos) {
            char cwd[256];
            if (!getcwd(cwd, sizeof cwd)) return false;
            full = std::string(cwd) + "/" + path;
        }
        fd = sceIoOpen(full.c_str(), PSP_O_RDONLY, 0);
        if (fd < 0) return false;
        size = sceIoLseek(fd, 0, PSP_SEEK_END);
        fd_pos = sceIoLseek(fd, 0, PSP_SEEK_SET);
        buf[0] = c0;
        buf[1] = c1;
        return size > 0 && fd_pos == 0;
    }

    SceOff file_size() const { return size; }

    // up to `want` bytes at `at` into dst; 0 only at the end of the file or on an error
    int copy(uint8_t *dst, int want, SceOff at) {
        for (int tries = 0; tries < 3; tries++) {
            for (int n = 0; n < 2; n++) {
                if (pending == n || !has(n, at)) continue;
                const int k = static_cast<int>(std::min<SceOff>(want, start[n] + len[n] - at));
                memcpy(dst, buf[n] + (at - start[n]), static_cast<size_t>(k));
                prefetch_after(n);
                return k;
            }
            if (pending >= 0) { wait(); continue; }   // the arriving chunk may hold it
            if (at >= size) return 0;
            read_now(start[0] <= start[1] ? 0 : 1, at);   // a seek, or the read-ahead fell behind
        }
        return 0;
    }

private:
    bool has(int n, SceOff at) const { return len[n] > 0 && at >= start[n] && at < start[n] + len[n]; }

    void wait() {
        if (pending < 0) return;
        SceInt64 res = 0;
        sceIoWaitAsync(fd, &res);
        len[pending] = res > 0 ? static_cast<int>(res) : 0;
        fd_pos = start[pending] + len[pending];
        pending = -1;
    }

    void read_now(int n, SceOff at) {
        wait();
        if (fd_pos != at) fd_pos = sceIoLseek(fd, at, PSP_SEEK_SET);
        const int r = sceIoRead(fd, buf[n], kChunk);
        start[n] = at;
        len[n] = std::max(0, r);
        fd_pos = at + len[n];
    }

    // while chunk n is consumed, the other one loads what follows it
    void prefetch_after(int n) {
        const int o = n ^ 1;
        const SceOff next = start[n] + len[n];
        if (pending >= 0 || next >= size || (len[o] > 0 && start[o] == next)) return;
        if (fd_pos != next) fd_pos = sceIoLseek(fd, next, PSP_SEEK_SET);
        start[o] = next;
        len[o] = 0;
        if (sceIoReadAsync(fd, buf[o], kChunk) >= 0) pending = o;
    }

    SceUID fd = -1;
    SceOff size = 0, fd_pos = 0;
    uint8_t *buf[2] = {nullptr, nullptr};
    SceOff start[2] = {0, 0};
    int len[2] = {0, 0};
    int pending = -1;   // chunk an async read is filling
};

class Mp3Track final : public host::TrackDecoder {
public:
    ~Mp3Track() override {
        if (handle >= 0) sceMp3ReleaseMp3Handle(handle);
    }

    bool open(const std::string &path) {
        name = path.substr(path.find_last_of('/') + 1);
        Buffers *b = buffers();
        if (!b) { rt::log_queued("CD: no memory for the MP3 buffers\n"); return false; }
        if (!file.open(path, b->chunk[0], b->chunk[1])) { rt::log_queued("CD: cannot open %s\n", path.c_str()); return false; }
        SceMp3InitArg arg{};
        arg.mp3StreamStart = 0;
        arg.mp3StreamEnd = file.file_size();
        arg.mp3Buf = b->mp3;
        arg.mp3BufSize = kMp3Buf;
        arg.pcmBuf = b->pcm;
        arg.pcmBufSize = kPcmBuf;
        handle = sceMp3ReserveMp3Handle(&arg);
        if (handle < 0) { rt::log_queued("CD: sceMp3ReserveMp3Handle %x\n", handle); return false; }
        feed();
        int r = sceMp3Init(handle);
        if (r < 0) { rt::log_queued("CD: sceMp3Init %x (%s)\n", r, name.c_str()); return false; }
        sceMp3SetLoopNum(handle, 0);
        const int rate = sceMp3GetSamplingRate(handle), ch = sceMp3GetMp3ChannelNum(handle);
        if (rate != 44100 || ch != 2) {
            rt::log_queued("CD: %s is %d Hz, %d channels (needs 44100 stereo)\n", name.c_str(), rate, ch);
            return false;
        }
        return true;
    }

    int read(uint8_t *dst, int bytes) override {
        int w = 0;
        while (w < bytes) {
            if (!left && !decode()) break;
            uint32_t k = std::min<uint64_t>(left, skip ? skip : static_cast<uint64_t>(bytes - w));
            if (skip) skip -= k;
            else { memcpy(dst + w, src, k); w += static_cast<int>(k); }
            src += k;
            left -= k;
        }
        pos += static_cast<uint64_t>(w);
        return w;
    }

    // Track starts are the usual case (no work); elsewhere: restart at the MP3 frame and drop the rest.
    void seek(uint64_t pcm_byte) override {
        if (pcm_byte == pos) return;
        const uint32_t frame = static_cast<uint32_t>(pcm_byte / kFrameBytes);
        if (sceMp3ResetPlayPositionByFrame(handle, frame) >= 0) {
            skip = pcm_byte - static_cast<uint64_t>(frame) * kFrameBytes;
        } else {
            sceMp3ResetPlayPosition(handle);
            skip = pcm_byte;
        }
        left = 0;
        ended = false;
        pos = pcm_byte;
    }

private:
    void feed() {
        if (sceMp3CheckStreamDataNeeded(handle) <= 0) return;
        SceUChar8 *to = nullptr;
        SceInt32 want = 0, at = 0;
        if (sceMp3GetInfoToAddStreamData(handle, &to, &want, &at) < 0) return;
        int got = 0;
        while (got < want) {
            const int k = file.copy(to + got, want - got, static_cast<SceOff>(at) + got);
            if (k <= 0) break;
            got += k;
        }
        sceMp3NotifyAddStreamData(handle, got);
    }

    bool decode() {
        if (ended) return false;
        feed();
        SceShort16 *out = nullptr;
        const int n = sceMp3Decode(handle, &out);
        if (n <= 0 || !out) { ended = true; return false; }   // 0 or an error: the end of the stream
        src = reinterpret_cast<const uint8_t *>(out);
        left = static_cast<uint32_t>(n);
        if (!logged && ++frames == 40) {   // ~1 s in: a non-zero peak shows the decoder delivers sound
            logged = true;
            int peak = 0;
            for (int i = 0; i < n / 2; i++) peak = std::max(peak, std::abs(static_cast<int>(out[i])));
            rt::log_queued("CD: %s decoding (%d-byte frames, peak %d)\n", name.c_str(), n, peak);
        }
        return true;
    }

    Reader file;
    int handle = -1;
    const uint8_t *src = nullptr;   // decoded PCM not yet delivered
    uint32_t left = 0;
    uint64_t pos = 0, skip = 0;     // next PCM byte delivered; bytes still to drop after a seek
    bool ended = false;
    std::string name;
    int frames = 0;
    bool logged = false;
};

}  // namespace

namespace host {
std::unique_ptr<TrackDecoder> open_mp3_track(const std::string &path) {
    if (!decoder_ready()) return nullptr;
    auto t = std::make_unique<Mp3Track>();
    if (!t->open(path)) return nullptr;
    return t;
}
}  // namespace host
#endif
