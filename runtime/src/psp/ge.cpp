// PSP GE context (see ge.h).
#include "ge.h"

#include <algorithm>
#include <vector>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include <SDL3/SDL.h>

#include <malloc.h>

#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace psp_ge {

namespace {

constexpr uint32_t FB_BYTES = FB_STRIDE * FB_H * 2;
constexpr size_t LIST_BYTES = 512 * 1024;
alignas(64) unsigned int list[LIST_BYTES / 4];   // commands and vertices; flushed early when half full
size_t list_used = 0;                            // vertex bytes taken from the open list
bool inited = false, open = false;
uint64_t wait_ns = 0;
int cur = 0;                                     // buffer being drawn (0/1); the other one is shown

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
    sceGuDrawBuffer(GU_PSM_5650, reinterpret_cast<void *>(fb_offset(0)), FB_STRIDE);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, reinterpret_cast<void *>(fb_offset(1) + VIEW_Y0 * FB_STRIDE * 2), FB_STRIDE);
    sceGuDepthBuffer(reinterpret_cast<void *>(fb_offset(2)), FB_STRIDE);
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
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void *>(fb_offset(cur)), FB_STRIDE);
    full_region();
    open = true;
    list_used = 0;
}

void finish() {
    if (!open) return;
    sceKernelDcacheWritebackAll();   // vertices, textures and converted frames are in cached RAM
    sceGuFinish();
    uint64_t t0 = SDL_GetTicksNS();
    sceGuSync(0, 0);
    wait_ns += SDL_GetTicksNS() - t0;
    open = false;
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

// Debug: MOTORACER_SNAPSHOT_DIR saves every MOTORACER_SNAPSHOT_EVERY-th (120) shown frame, the
// visible 480x272 picture, as gpu_NNNNNN.bmp (the desktop name for GPU frames).
static void maybe_snapshot(const uint8_t *shown) {
    static const char *dir = getenv("MOTORACER_SNAPSHOT_DIR");
    static const uint64_t every = getenv("MOTORACER_SNAPSHOT_EVERY") ? std::max(1, atoi(getenv("MOTORACER_SNAPSHOT_EVERY"))) : 120;
    static uint64_t n = 0;
    if (!dir || n++ % every) return;
    (void)shown;
    static uint16_t *buf = static_cast<uint16_t *>(memalign(64, FB_STRIDE * SCREEN_H * 2));
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
    cur ^= 1;
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
    sceGuTexScale(1.f, 1.f);
    sceGuTexOffset(0.f, 0.f);
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

void *list_memory(size_t n) {
    n = (n + 15) & ~size_t{15};
    if (open && list_used + n > LIST_BYTES / 2) finish();   // the rest of the frame goes into a new list
    begin();
    list_used += n;
    return sceGuGetMemory(static_cast<int>(n));
}

}  // namespace psp_ge
