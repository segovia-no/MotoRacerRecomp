// PSP GE context (see ge.h).
#include "ge.h"

#include <algorithm>
#include <vector>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspsysmem.h>

#include <SDL3/SDL.h>

#include <malloc.h>

#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include "runtime.h"

namespace psp_ge {

void *outside_heap(size_t bytes) {
    static uint8_t *base = nullptr, *end = nullptr, *next = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const size_t want = 512u << 10, avail = sceKernelMaxFreeMemSize();
        const size_t size = std::min(want, avail);
        if (size >= (64u << 10)) {
            SceUID id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "motoracer frames", PSP_SMEM_Low, size, nullptr);
            if (id >= 0) {
                base = static_cast<uint8_t *>(sceKernelGetBlockHeadAddr(id));
                end = base + size;
                next = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(base) + 63) & ~uintptr_t(63));
                rt::log("ge: %zu KB outside the C heap for frame buffers\n", size >> 10);
            }
        }
    }
    if (!next || static_cast<size_t>(end - next) < bytes) return nullptr;
    void *p = next;
    next = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(next) + bytes + 63) & ~uintptr_t(63));
    return p;
}

// pspgu's own display list state (guInternal.h): draw_array / list_memory write the same words as
// sceGuDrawArray / sceGuGetMemory, without their stall-address update.
extern "C" {
struct GuDisplayList {
    unsigned int *start, *current;
    int parent_context;
};
extern GuDisplayList *gu_list;
extern int ge_list_executed[2];
}

namespace {

constexpr uint32_t FB_BYTES = FB_STRIDE * FB_H * 2;
// The display list: two halves of 64 KB (commands and vertex data). When the open half is full it is
// closed and handed to the GE without waiting, and drawing goes on in the other half (waiting only if
// the GE has not finished the list there yet): the GE draws the first part of a frame while the CPU
// builds the rest. Before, a full list (about twice per race frame) was finished and waited for, the
// CPU idle until the GE caught up with everything. (PSP-1000 memory: 512 KB before.)
constexpr size_t LIST_BYTES = 128 * 1024, HALF_BYTES = LIST_BYTES / 2;
constexpr size_t LIST_MARGIN = 6 * 1024;   // room left for the commands written between two checks
uint32_t early_flush_count = 0, list_peak = 0;
alignas(64) unsigned int list[LIST_BYTES / 4];
int half = 0;                    // the half in use
int half_qid[2] = {-1, -1};      // the GE list id of each half while the GE may still read it
// In a direct list the GE runs up to the stall address. pspgu moves it (a kernel call) after every
// sceGuDrawArray and sceGuGetMemory: ~1,200 times per race frame. Here: after a draw once the list has
// grown by MOTORACER_GE_STALL_STEP bytes since the last move (default 0: every draw; list memory never).
// A 4 KB step (2026-11-06) made a real PSP wait ~20 ms per race frame for the GE at the frame's end
// ("present" + "wait"): the GE has to run alongside the CPU.
ptrdiff_t stall_step_words() {
    static const ptrdiff_t w = getenv("MOTORACER_GE_STALL_STEP") ? atoi(getenv("MOTORACER_GE_STALL_STEP")) / 4 : 0;
    return w;
}
unsigned int *stall_at = nullptr;
bool inited = false, open = false;
uint32_t generation = 0;   // display lists started
uint64_t wait_ns = 0;
// Three colour buffers: one drawn, one waiting for the display to switch to it at the next vertical
// blank, one shown. With two, the next frame started drawing into the buffer still being scanned out
// until that blank: a cleared (black) band at the bottom of the screen in the menus on a real PSP.
constexpr int COLOR_BUFFERS = 3, DEPTH_BUFFER = 3, SCRATCH_BUFFER = 4;
int cur = 0;                                     // buffer being drawn

uint32_t fb_offset(int i) { return FB_BYTES * static_cast<uint32_t>(i); }

// pspgu sizes the drawing region from sceGuDispBuffer (480x272); the buffers have 360 rows.
void full_region() {
    sceGuSendCommandi(0x15, 0);                                      // REGION1: x1, y1 = 0
    sceGuSendCommandi(0x16, (FB_W - 1) | ((FB_H - 1) << 10));       // REGION2: x2, y2
    sceGuScissor(0, 0, FB_W, FB_H);
}
uint8_t *vram_cpu() {
    return reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(sceGeEdramGetAddr()) | 0x40000000u);
}

}  // namespace

void init() {
    if (inited) return;
    inited = true;
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    stall_at = gu_list->current;
    sceGuDrawBuffer(GU_PSM_5650, reinterpret_cast<void *>(fb_offset(0)), FB_STRIDE);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, reinterpret_cast<void *>(fb_offset(1) + VIEW_Y0 * FB_STRIDE * 2), FB_STRIDE);
    sceGuDepthBuffer(reinterpret_cast<void *>(fb_offset(DEPTH_BUFFER)), FB_STRIDE);
    sceGuOffset(2048 - FB_W / 2, 2048 - FB_H / 2);
    sceGuViewport(2048, 2048, FB_W, FB_H);
    full_region();
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthRange(65535, 0);
    sceGuClearColor(0);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void begin() {
    init();
    if (open) return;
    generation++;
    if (half_qid[half] >= 0) {   // the GE is (or was) running the list last built in this half
        prof::Scope prof_wait(prof::WAIT);
        sceGeListSync(half_qid[half], 0);
        half_qid[half] = -1;
    }
    sceGuStart(GU_DIRECT, list + half * (HALF_BYTES / 4));
    half_qid[half] = ge_list_executed[0];
    sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void *>(fb_offset(cur)), FB_STRIDE);
    full_region();
    open = true;
}

void finish() {
    if (!open) return;
    sceKernelDcacheWritebackAll();   // vertices, textures and converted frames are in cached RAM
    sceGuFinish();
    uint64_t t0 = SDL_GetTicksNS();
    prof::Scope prof_wait(prof::WAIT);
    sceGuSync(0, 0);   // every list queued
    wait_ns += SDL_GetTicksNS() - t0;
    half_qid[0] = half_qid[1] = -1;
    open = false;
}

// The open half is full: hand it to the GE and continue in the other half (no wait here).
static void flush_early() {
    if (!open) return;
    sceKernelDcacheWritebackAll();
    sceGuFinish();
    open = false;
    half ^= 1;
    early_flush_count++;
}

// bytes in the open list (commands and data)
static size_t list_bytes() {
    return open ? static_cast<size_t>(gu_list->current - gu_list->start) * 4 : 0;
}

static uint8_t *vram_abs(uint32_t offset) {   // the address the GE uses for transfers
    return reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(sceGeEdramGetAddr()) + offset);
}

void read_rect(uint16_t *ram, int ram_stride, int x, int y, int w, int h) {
    begin();
    sceGuCopyImage(GU_PSM_5650, x, y, w, h, FB_STRIDE, vram_abs(fb_offset(cur)), 0, 0, ram_stride, ram);
    sceGuTexSync();
    finish();
    sceKernelDcacheInvalidateRange(ram, ram_stride * h * 2);
}

void write_rect(const uint16_t *ram, int ram_stride, int x, int y, int w, int h) {
    sceKernelDcacheWritebackRange(ram, ram_stride * h * 2);
    begin();
    sceGuCopyImage(GU_PSM_5650, 0, 0, w, h, ram_stride, const_cast<uint16_t *>(ram), x, y, FB_STRIDE,
                   vram_abs(fb_offset(cur)));
    sceGuTexSync();
}

uint16_t *staging() {
    static uint16_t *buf = [] {
        const size_t n = FB_STRIDE * FB_H * 2;
        void *p = outside_heap(n);
        return static_cast<uint16_t *>(p ? p : memalign(64, n));
    }();
    return buf;
}

// Debug: MOTORACER_SNAPSHOT_DIR saves every MOTORACER_SNAPSHOT_EVERY-th (120) shown frame, the
// visible 480x272 picture, as gpu_NNNNNN.bmp (the desktop name for GPU frames).
static void maybe_snapshot(const uint8_t *shown) {
    static const char *dir = getenv("MOTORACER_SNAPSHOT_DIR");
    static const uint64_t every = getenv("MOTORACER_SNAPSHOT_EVERY") ? std::max(1, atoi(getenv("MOTORACER_SNAPSHOT_EVERY"))) : 120;
    static uint64_t n = 0;
    if (!dir || n++ % every) return;
    (void)shown;
    uint16_t *buf = staging();   // free between the D3D backend's transfers (no extra 272 KB)
    read_rect(buf, FB_STRIDE, 0, VIEW_Y0, SCREEN_W, SCREEN_H);
    SDL_Surface *s = SDL_CreateSurfaceFrom(SCREEN_W, SCREEN_H, SDL_PIXELFORMAT_BGR565, buf, FB_STRIDE * 2);
    if (!s) return;
    char path[512];
    snprintf(path, sizeof path, "%s/gpu_%06llu.bmp", dir, static_cast<unsigned long long>(n));
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
}

void show() {
    init();
    finish();
    maybe_snapshot(vram_cpu() + fb_offset(cur) + VIEW_Y0 * FB_STRIDE * 2);
    sceDisplaySetFrameBuf(vram_cpu() + fb_offset(cur) + VIEW_Y0 * FB_STRIDE * 2, FB_STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_NEXTFRAME);
    cur = (cur + 1) % COLOR_BUFFERS;   // the one shown two frames ago (a vertical blank has passed)
}

void downscale_565(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint16_t *dst, uint32_t dstride,
                   uint32_t dw, uint32_t dh) {
    // source column per output column, integer, computed once per size
    static std::vector<uint16_t> cols;
    static uint32_t cols_w = 0, cols_dw = 0;
    if (cols_w != w || cols_dw != dw) {
        cols.resize(dw);
        for (uint32_t x = 0; x < dw; x++) cols[x] = static_cast<uint16_t>(std::min<uint32_t>(x * w / dw, w - 2));
        cols_w = w;
        cols_dw = dw;
    }
    // all three channels in one 32-bit word: green moved to bits 21-26, red / blue stay at 11-15 /
    // 0-4; a sum of four fits below the next field
    auto spread = [](uint32_t q) { return (q | q << 16) & 0x07E0F81Fu; };
    for (uint32_t y = 0; y < dh; y++) {
        const uint32_t sy = std::min<uint32_t>(y * h / dh, h - 2);
        const uint16_t *r0 = reinterpret_cast<const uint16_t *>(src + static_cast<size_t>(sy) * pitch);
        const uint16_t *r1 = reinterpret_cast<const uint16_t *>(src + static_cast<size_t>(sy + 1) * pitch);
        uint16_t *out = dst + y * dstride;
        for (uint32_t x = 0; x < dw; x++) {
            const uint32_t sx = cols[x];
            const uint32_t sum = spread(r0[sx]) + spread(r0[sx + 1]) + spread(r1[sx]) + spread(r1[sx + 1]);
            const uint32_t a = (sum >> 2) & 0x07E0F81Fu;
            out[x] = static_cast<uint16_t>((a >> 11 & 31) | (a >> 16 & 0x7E0) | (a & 31) << 11);   // red <-> blue
        }
    }
}

namespace {
struct SpriteVertex { float u, v, x, y, z; };

// Through-mode textured sprites copying a w x h 5650 image (stride in pixels) to (x0, y0) scaled to
// dw x dh, bilinear filtered. In vertical strips of 32 source texels: the GE's texture cache is tiny,
// and wide sprites over an unswizzled texture in RAM made a real PSP wait ~17 ms per menu frame.
void draw_image(const uint16_t *img, uint32_t w, uint32_t h, uint32_t stride, float x0, float y0, float dw, float dh) {
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDepthMask(GU_TRUE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    writeback(img, static_cast<size_t>(stride) * h * 2);   // the CPU just drew it
    uint32_t th = 1;
    while (th < h) th <<= 1;
    const float sx_scale = dw / static_cast<float>(w);
    // the texture: up to 512 texels from `base`; strips address it by u (no per-strip TexImage)
    for (uint32_t base = 0; base < w; base += 512) {
        const uint32_t bw = w - base < 512 ? w - base : 512;
        uint32_t tw = 1;
        while (tw < bw) tw <<= 1;
        sceGuTexImage(0, static_cast<int>(tw), static_cast<int>(th), static_cast<int>(stride), img + base);
        sceGuTexFlush();
        constexpr uint32_t STRIP = 32;
        const uint32_t n = (bw + STRIP - 1) / STRIP;
        auto *v = static_cast<SpriteVertex *>(list_memory(2 * n * sizeof(SpriteVertex)));
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t u0 = i * STRIP, u1 = std::min(bw, u0 + STRIP);
            v[2 * i] = {static_cast<float>(u0), 0.f, x0 + (base + u0) * sx_scale, y0, 0.f};
            v[2 * i + 1] = {static_cast<float>(u1), static_cast<float>(h), x0 + (base + u1) * sx_scale, y0 + dh, 0.f};
        }
        draw_array(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, static_cast<int>(2 * n), nullptr, v);
    }
    sceGuDisable(GU_TEXTURE_2D);
}
}  // namespace

bool draw_halved_565(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, int dx, int dy, int dw, int dh) {
    const uint32_t hw = w / 2, hh = h / 2;
    if ((w | h) & 1 || hw > 512 || hh > static_cast<uint32_t>(FB_H) || (pitch & 31) ||
        (reinterpret_cast<uintptr_t>(src) & 15))
        return false;
    // pass 1: the 2x2 box average into the scratch buffer behind the depth buffer (red / blue are
    // swapped all along: both 5-bit fields average the same way)
    const uint32_t scratch = fb_offset(SCRATCH_BUFFER);
    begin();
    sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void *>(scratch), FB_STRIDE);
    sceGuScissor(0, 0, static_cast<int>(hw), static_cast<int>(hh));
    draw_image(reinterpret_cast<const uint16_t *>(src), w, h, pitch / 2, 0.f, 0.f, static_cast<float>(hw), static_cast<float>(hh));
    // back to RAM, red / blue swapped (two pixels per word)
    uint16_t *half = staging();
    const int hstride = static_cast<int>((hw + 7) & ~7u);
    sceGuCopyImage(GU_PSM_5650, 0, 0, static_cast<int>(hw), static_cast<int>(hh), FB_STRIDE, vram_abs(scratch), 0, 0, hstride, half);
    sceGuTexSync();
    finish();
    sceKernelDcacheInvalidateRange(half, hstride * hh * 2);
    uint32_t *pairs = reinterpret_cast<uint32_t *>(half);
    for (uint32_t i = 0, n = static_cast<uint32_t>(hstride) * hh / 2; i < n; i++) {
        const uint32_t p = pairs[i];
        pairs[i] = ((p >> 11) & 0x001F001Fu) | (p & 0x07E007E0u) | ((p << 11) & 0xF800F800u);
    }
    // pass 2: scaled into the draw buffer
    begin();      // a new list: the draw buffer is the current one again
    full_region();   // pass 1 left a 320x240 scissor (the GE keeps it across lists): a black bar below
    draw_image(half, hw, hh, static_cast<uint32_t>(hstride), static_cast<float>(dx), static_cast<float>(dy),
               static_cast<float>(dw), static_cast<float>(dh));
    touch_state();
    return true;
}

// Projection for 3D-mode draws (column-major: clip = P.x*X + P.y*Y + P.z*Z + P.w). With the viewport
// (centre 2048, FB_W x FB_H) and depth range 65535..0 set in init, a vertex (x*w, y*w, w) lands on
// pixel (x, y) with depth 65535 * w0 / w: clip = (2X/FB_W - Z, Z - 2Y/FB_H, Z - 2*w0, Z).
static float cur_w0 = -1.f;
static void send_projection(float w0) {
    ScePspFMatrix4 p{};
    p.x.x = 2.f / FB_W;
    p.y.y = -2.f / FB_H;
    p.z.x = -1.f;
    p.z.y = 1.f;
    p.z.z = 1.f;
    p.z.w = 1.f;
    p.w.z = -2.f * w0;
    sceGuSetMatrix(GU_PROJECTION, &p);
    ScePspFMatrix4 id{};
    id.x.x = id.y.y = id.z.z = id.w.w = 1.f;
    sceGuSetMatrix(GU_VIEW, &id);
    sceGuSetMatrix(GU_MODEL, &id);
    cur_w0 = w0;
}
void set_near_w(float w0) {
    if (w0 == cur_w0 && open) return;
    begin();
    send_projection(w0);
}

static uint32_t epoch = 0;
uint32_t state_epoch() { return epoch; }
void touch_state() { epoch++; }

uint64_t take_wait_ns() {
    uint64_t w = wait_ns;
    wait_ns = 0;
    return w;
}

uint16_t *draw_buffer() {
    init();
    return reinterpret_cast<uint16_t *>(vram_cpu() + fb_offset(cur));
}

uint32_t take_list_stats(uint32_t *peak) {
    const uint32_t n = early_flush_count;
    *peak = list_peak;
    early_flush_count = 0;
    list_peak = 0;
    return n;
}

uint32_t list_generation() { return generation; }

void writeback(const void *p, size_t n) { sceKernelDcacheWritebackRange(p, static_cast<unsigned int>(n)); }

void reserve(size_t n) {
    n = (n + 15) & ~size_t{15};
    if (open && list_bytes() + n + LIST_MARGIN > HALF_BYTES) flush_early();
    begin();
}

void *list_memory(size_t n) {
    n = (n + 15) & ~size_t{15};
    if (open && list_bytes() + n + LIST_MARGIN > HALF_BYTES) flush_early();   // the rest of the frame: the other half
    begin();
    list_peak = std::max(list_peak, static_cast<uint32_t>(list_bytes() + n));
    // sceGuGetMemory: a jump over the block (BASE + JUMP), the block after it
    unsigned int *cur = gu_list->current, *data = cur + 2;
    auto *next = reinterpret_cast<unsigned int *>(reinterpret_cast<uint8_t *>(data) + n);
    const auto a = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(next));
    cur[0] = (0x10u << 24) | ((a >> 8) & 0xf0000u);
    cur[1] = (0x08u << 24) | (a & 0xffffffu);
    gu_list->current = next;
    return data;
}

void draw_array(int prim, int vtype, int count, const void *indices, const void *vertices) {
    begin();
    unsigned int *c = gu_list->current;   // sceGuDrawArray's words
    if (vtype) *c++ = (0x12u << 24) | (static_cast<unsigned int>(vtype) & 0xffffffu);
    if (indices) {
        const auto a = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(indices));
        *c++ = (0x10u << 24) | ((a >> 8) & 0xf0000u);
        *c++ = (0x02u << 24) | (a & 0xffffffu);
    }
    if (vertices) {
        const auto a = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(vertices));
        *c++ = (0x10u << 24) | ((a >> 8) & 0xf0000u);
        *c++ = (0x01u << 24) | (a & 0xffffffu);
    }
    *c++ = (0x04u << 24) | ((static_cast<unsigned int>(prim) << 16 | static_cast<unsigned int>(count)) & 0xffffffu);
    gu_list->current = c;
    if (c - stall_at >= stall_step_words()) {
        static const uint8_t region = prof_register("ge: stall address (kernel call)");
        const uint8_t prev = prof_region;
        prof_region = region;
        sceGeListUpdateStallAddr(ge_list_executed[0], c);
        prof_region = prev;
        stall_at = c;
    }
}

}  // namespace psp_ge
