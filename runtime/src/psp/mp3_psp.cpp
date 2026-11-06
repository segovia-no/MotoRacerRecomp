// CD-audio tracks as MP3 on the PSP (host::open_mp3_track): the firmware's sceMp3 library decodes on
// the Media Engine, so the main CPU only copies PCM. tools/psp/cd_mp3.sh writes the files (CBR, no
// ID3 / Xing header). The first track loads the AV modules (avcodec, mp3) and the decoder resource.
#ifdef __PSP__
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>

#include <pspkernel.h>
#include <pspmp3.h>
#include <psputility.h>

#include "host_audio.h"
#include "runtime.h"

namespace {

constexpr int kMp3Buf = 16 * 1024;   // stream data (>= 8192, + workspace)
constexpr int kPcmBuf = 2 * 4608;    // two decoded frames (>= 9216)
constexpr uint32_t kFrameBytes = 1152 * 4;

bool decoder_ready() {
    static int state = 0;   // 0 untried, 1 ready, -1 unavailable
    if (state) return state > 0;
    const SceSize before = sceKernelMaxFreeMemSize();
    rt::log("CD: loading the MP3 decoder (kernel free %u KB)\n", static_cast<unsigned>(before >> 10));
    int a = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC), m = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    if ((a < 0 && a != static_cast<int>(0x80020139)) || (m < 0 && m != static_cast<int>(0x80020139))) {
        rt::log("CD: MP3 modules not loaded (avcodec %x, mp3 %x): no music\n", a, m);
        state = -1;
        return false;
    }
    int r = sceMp3InitResource();
    if (r < 0) {
        rt::log("CD: sceMp3InitResource %x: no music\n", r);
        state = -1;
        return false;
    }
    rt::log("CD: MP3 decoder ready (kernel free %u -> %u KB)\n", static_cast<unsigned>(before >> 10),
            static_cast<unsigned>(sceKernelMaxFreeMemSize() >> 10));
    state = 1;
    return true;
}

class Mp3Track final : public host::TrackDecoder {
public:
    ~Mp3Track() override {
        if (handle >= 0) sceMp3ReleaseMp3Handle(handle);
        if (fd >= 0) close(fd);
        free(mp3_buf);
        free(pcm_buf);
    }

    bool open(const std::string &path) {
        name = path.substr(path.find_last_of('/') + 1);
        // libc, not sceIo: the path is relative to the game folder, a working directory only libc knows
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { rt::log("CD: cannot open %s\n", path.c_str()); return false; }
        const off_t size = lseek(fd, 0, SEEK_END);
        mp3_buf = static_cast<uint8_t *>(memalign(64, kMp3Buf));
        pcm_buf = static_cast<uint8_t *>(memalign(64, kPcmBuf));
        if (size <= 0 || !mp3_buf || !pcm_buf) { rt::log("CD: %s: empty or out of memory\n", path.c_str()); return false; }
        SceMp3InitArg arg{};
        arg.mp3StreamStart = 0;
        arg.mp3StreamEnd = size;
        arg.mp3Buf = mp3_buf;
        arg.mp3BufSize = kMp3Buf;
        arg.pcmBuf = pcm_buf;
        arg.pcmBufSize = kPcmBuf;
        handle = sceMp3ReserveMp3Handle(&arg);
        if (handle < 0) { rt::log("CD: sceMp3ReserveMp3Handle %x\n", handle); return false; }
        feed();
        int r = sceMp3Init(handle);
        if (r < 0) { rt::log("CD: sceMp3Init %x (%s)\n", r, path.c_str()); return false; }
        sceMp3SetLoopNum(handle, 0);
        const int rate = sceMp3GetSamplingRate(handle), ch = sceMp3GetMp3ChannelNum(handle);
        if (rate != 44100 || ch != 2) {
            rt::log("CD: %s is %d Hz, %d channels (needs 44100 stereo)\n", path.c_str(), rate, ch);
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
        if (want > 0 && lseek(fd, at, SEEK_SET) == at) got = std::max(0, static_cast<int>(::read(fd, to, static_cast<size_t>(want))));
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
            rt::log("CD: %s decoding (%d-byte frames, peak %d)\n", name.c_str(), n, peak);
        }
        return true;
    }

    int fd = -1;
    int handle = -1;
    uint8_t *mp3_buf = nullptr, *pcm_buf = nullptr;
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
