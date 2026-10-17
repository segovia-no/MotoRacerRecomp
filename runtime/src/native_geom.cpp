// Native versions of the game's hottest geometry functions (docs/27-native-geometry.md).
//
// Whole guest functions replaced through NATIVE_FUNCS (translate.py): f_<va> calls the native
// function here, which returns 1 when it did the work (with the guest registers, stack and memory
// as the translated function leaves them) or 0 to run the translated body f_<va>_t. Floating point
// as the translated x87 code computes it (fpu_t, same operation order), so results are identical.
//   MOTORACER_NATIVE=0      translated code only
//   MOTORACER_NATIVE=check  run both on the same input and log any difference
//   (default)               native
#include <cstdlib>
#include <cstring>
#include <vector>

#include <SDL3/SDL_timer.h>

#include "runtime.h"
// Guest memory through a local base pointer (MEM_LOCAL), as in the generated code.
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_0049b2c0_t(void);
extern "C" void f_0049b7f0_t(void);
extern "C" void f_0049c590(void);
extern "C" void f_0049c590_t(void);
extern "C" void f_0049c110_t(void);
extern "C" void f_0049ca70(void);
extern "C" void f_00497080_t(void);

namespace {

using namespace native;

// x87 FCOM on a status word: C0 (0x100) below, C3 (0x4000) equal, C0|C2|C3 unordered
inline uint32_t fcom(uint32_t sw, fpu_t a, fpu_t b) {
    sw &= ~0x4500u;
    if (a < b) return sw | 0x100u;
    if (a == b) return sw | 0x4000u;
    if (a > b) return sw;
    return sw | 0x4500u;
}
#define BELOW(a, b) (((fsw = fcom(fsw, (a), (b))) & 0x100u) != 0)        /* fcomp; test ah, 1 */
#define BELOW_EQ(a, b) (((fsw = fcom(fsw, (a), (b))) & 0x4100u) != 0)    /* fcomp; test ah, 0x41 */

// Aligned accesses (one load on MIPS) where the guest data is: vertex pool / transformed vertices,
// matrices, globals, stack. Face records and source vertices of objects packed in the exe's data
// can sit at any address (seen: 0x58a51b): unaligned loads. Check mode verifies the others.
void check_aligned(const char *what, uint32_t a, uint32_t align = 4) {
    if (a & (align - 1)) rt::log("native geom: MISALIGNED %s 0x%08x\n", what, a);
}

// ---- MOTORACER_GEOM_TIME=1: time spent per function (native or translated), logged every 5 s ----
// (inclusive: 0x49b7f0's time includes its 0x49c590 calls)
struct Timing { const char *name; uint64_t ns, calls; };
Timing timings[] = {{"transform 0x49b2c0", 0, 0}, {"faces 0x49b7f0", 0, 0}, {"poly setup 0x49c590", 0, 0}, {"faces 0x49c110", 0, 0}, {"clip 0x497080", 0, 0}};
bool timing_on() {
    static const bool on = getenv("MOTORACER_GEOM_TIME") && strcmp(getenv("MOTORACER_GEOM_TIME"), "0");
    return on;
}
void timing_tick() {
    static uint64_t last = 0;
    const uint64_t now = SDL_GetTicks();
    if (!last) last = now;
    if (now - last < 5000) return;
    last = now;
    for (Timing &t : timings) {
        rt::log("geom time: %s %s: %llu calls, %llu ms per 5 s\n", t.name, mode() == OFF ? "translated" : "native",
                static_cast<unsigned long long>(t.calls), static_cast<unsigned long long>(t.ns / 1000000));
        t.ns = t.calls = 0;
    }
}
struct Timed {   // times one call when MOTORACER_GEOM_TIME is on
    Timing *t;
    uint64_t t0;
    explicit Timed(int k) : t(k >= 0 && timing_on() ? &timings[k] : nullptr), t0(t ? SDL_GetTicksNS() : 0) {}
    ~Timed() {
        if (!t) return;
        t->ns += SDL_GetTicksNS() - t0;
        t->calls++;
        if (t == &timings[0]) timing_tick();
    }
};

// ---- 0x49b2c0: transform vertices to the camera, project to the screen ----
// stdcall (matrix, src, count, flags), returns the output in eax (and edx).
//   matrix: 3x4 floats (x, y, z columns per row; row 3 = translation); x columns are scaled by the
//     viewport scale [0x6d20c4], y columns by [0x6d104c]
//   src: count vertices, 3 floats each
//   out: the next `count` entries of the transformed-vertex pool 0x6d20d8 (28 bytes each; the pool's
//     used count [0x68b014] grows by count): x, y, z (floats), flags << 13, screen x, screen y (int,
//     only written when z >= the near plane [0x625774]), 1/z (float)
//   screen x = centre x [0x60d3c4] + x/z, screen y = centre y [0x624f88] - y/z, rounded per the x87
//   rounding mode (fistp; the function only changes the precision control).
constexpr uint32_t POOL = 0x6d20d8, POOL_USED = 0x68b014;
constexpr uint32_t SCALE_X = 0x6d20c4, SCALE_Y = 0x6d104c, CENTRE_X = 0x60d3c4, CENTRE_Y = 0x624f88;
constexpr uint32_t NEAR_Z = 0x625774;

void transform() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, m = RD32A(sp + 4), n = RD32A(sp + 12), flags = RD32A(sp + 16) << 13;
    uint32_t src = RD32A(sp + 8);
    const uint32_t used = RD32A(POOL_USED), out0 = POOL + used * 28u;
    WR32A(POOL_USED, used + n);
    const fpu_t sx = RDF32A(SCALE_X), sy = RDF32A(SCALE_Y);
    // the matrix as the function keeps it (stored to its stack frame as floats)
    const float c0 = static_cast<float>(RDF32A(m) * sx), c1 = static_cast<float>(RDF32A(m + 4) * sy), c2 = RDF32A(m + 8);
    const float c3 = static_cast<float>(RDF32A(m + 12) * sx), c4 = static_cast<float>(RDF32A(m + 16) * sy), c5 = RDF32A(m + 20);
    const float c6 = static_cast<float>(RDF32A(m + 24) * sx), c7 = static_cast<float>(RDF32A(m + 28) * sy), c8 = RDF32A(m + 32);
    const float c9 = static_cast<float>(RDF32A(m + 36) * sx), c10 = static_cast<float>(RDF32A(m + 40) * sy), c11 = RDF32A(m + 44);
    const fpu_t cy = static_cast<float>(static_cast<fpu_t>(static_cast<int32_t>(RD32A(CENTRE_Y))));
    const fpu_t cx = static_cast<float>(static_cast<fpu_t>(static_cast<int32_t>(RD32A(CENTRE_X))));
    const float near_z = RDF32A(NEAR_Z);
    uint32_t fsw = F.sw;
    uint32_t o = out0;
    for (uint32_t i = 0; i < n; i++, src += 12, o += 28) {
        // source vertices can be 2-byte aligned (objects packed in the exe's data): unaligned loads
        const fpu_t vx = RDF32(src), vy = RDF32(src + 4), vz = RDF32(src + 8);
        const float x = static_cast<float>(((vx * c0 + vy * c3) + vz * c6) + c9);
        const float y = static_cast<float>(((vx * c1 + vy * c4) + vz * c7) + c10);
        const float z = static_cast<float>(((vx * c2 + vy * c5) + vz * c8) + c11);
        const float rhw = static_cast<float>(static_cast<fpu_t>(1.0) / static_cast<fpu_t>(z));
        WRF32A(o, x);
        WRF32A(o + 4, y);
        WRF32A(o + 8, z);
        WR32A(o + 12, flags);
        WRF32A(o + 24, rhw);
        if (BELOW(static_cast<fpu_t>(z), static_cast<fpu_t>(near_z))) continue;   // nearer than the near plane (or NaN): no screen position
        WR32A(o + 20, static_cast<uint32_t>(F_TO_I32(cy - static_cast<fpu_t>(rhw) * y)));
        WR32A(o + 16, static_cast<uint32_t>(F_TO_I32(static_cast<fpu_t>(rhw) * x + cx)));
    }
    F.sw = static_cast<uint16_t>(fsw);
    R.eax = R.edx = out0;
    R.ecx = 0;
    R.esp = sp + 20;   // ret 0x10
}

// ---- 0x49b7f0 / 0x49c110: visible faces of an object -> polygon setup ----
// thiscall (ecx = renderer), stdcall args (verts, faces, count, arg4):
//   verts: the object's transformed vertices (28 bytes each, as 0x49b2c0 writes them)
//   faces: count records of 64 bytes: +0 type, +0x2c..+0x32 four vertex indexes, +0x36 special
//     index, +0x38 flags (bit 0: may be type 0xd, bit 1: depth bias)
// Per face (0x49b7f0 skips type 0xd, 0x49c110 type 0xd with flag bit 0): [0x51a404]++; a reference
// vertex — 0x49b7f0: arg4 ? the farthest : the nearest; 0x49c110: the farthest (x87 compares, NaN
// rules included) — must be beyond the near plane. All four beyond it: screen-space test — outcode
// counts against the screen rectangle reject faces entirely off one side, then the 2D cross
// product (as float) must be > 1. Otherwise ("near" face, flags bit 0): 3D normal
// (v1 - v2) x (v1 - v0) dotted with the reference vertex must be > 1. With [0x4dc06c] set and
// [0x4dc070] clear, faces not entirely inside the rectangle get flags bit 1 (clip).
// Visible faces: 0x49b7f0: 0x49c590(face, depth, verts, flags, 0), depth = reference z
// (+ [0x4d1f94] with the bias bit); 0x49c110: faces whose special entry [0x4f9300 + 824 * index]
// is set go to 0x49ca70(face, z, verts, arg4, flags), the others to 0x49c590(face, z, verts, flags, 0).
constexpr uint32_t FACE_COUNTER = 0x51a404, SCR_XMIN = 0x624374, SCR_XMAX = 0x62e448, SCR_YMIN = 0x6d1040,
                   SCR_YMAX = 0x625784, CLIP_ON = 0x4dc06c, CLIP_OFF = 0x4dc070, DEPTH_BIAS = 0x4d1f94,
                   SPECIALS = 0x4f9300;

struct PolyCall { uint32_t target, face, depth, verts, a4, a5, ecx, eax, edx, esp; };
bool recording = false;            // check mode: calls to 0x49c590 / 0x49ca70 are recorded
std::vector<PolyCall> recorded;
constexpr uint32_t EDX_UNKNOWN = 0xdeadbeefu;   // dry run: edx as left by a call not made

void record_call(uint32_t target) {   // at a callee's entry (guest stack: return address, arguments)
    MEM_LOCAL();
    recorded.push_back({target, RD32A(R.esp + 4), RD32A(R.esp + 8), RD32A(R.esp + 12), RD32A(R.esp + 16), RD32A(R.esp + 20),
                        R.ecx, R.eax, R.edx, R.esp});
}

inline uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

enum Variant { FACES_7F0, FACES_110 };

// dry: record the calls instead of making them (check mode)
void faces(Variant var, bool dry, std::vector<PolyCall> *calls) {
    MEM_LOCAL();
    const uint32_t sp = R.esp, self = R.ecx, verts = RD32A(sp + 4), count = RD32A(sp + 12), arg4 = RD32A(sp + 16);
    uint32_t face = RD32A(sp + 8);
    R.esp = sp + 20;   // ret 0x10
    if (!verts) { R.eax = 0; return; }
    if (static_cast<int32_t>(count) <= 0) { R.eax = count; return; }
    uint32_t fsw = F.sw, edx = R.edx;
    const uint32_t frame = sp - (var == FACES_7F0 ? 0x3c : 0x40);   // esp inside the function
    for (uint32_t i = 1;; i++, face += 0x40) {
        const bool skip = var == FACES_7F0 ? RD16(face) == 0xd : (RD8(face + 0x38) & 1) && RD16(face) == 0xd;
        if (!skip) {
            WR32A(FACE_COUNTER, RD32A(FACE_COUNTER) + 1);
            const uint32_t v0 = verts + RD16(face + 0x2c) * 28u, v1 = verts + RD16(face + 0x2e) * 28u;
            const uint32_t v2 = verts + RD16(face + 0x30) * 28u, v3 = verts + RD16(face + 0x32) * 28u;
            edx = v0;
            const float z0 = RDF32A(v0 + 8), z1 = RDF32A(v1 + 8), z2 = RDF32A(v2 + 8), z3 = RDF32A(v3 + 8);
            uint32_t ref = v0;
            if (var == FACES_110) {
                if (BELOW(z0, z1)) ref = v1;
                if (BELOW(RDF32A(ref + 8), z2)) ref = v2;
                if (BELOW(RDF32A(ref + 8), z3)) ref = v3;
            } else if (arg4) {
                if (!BELOW_EQ(z1, z0)) ref = v1;
                if (BELOW(RDF32A(ref + 8), z2)) ref = v2;
                if (BELOW(RDF32A(ref + 8), z3)) ref = v3;
            } else {
                if (BELOW(z1, z0)) ref = v1;
                if (!BELOW_EQ(RDF32A(ref + 8), z2)) ref = v2;
                if (!BELOW_EQ(RDF32A(ref + 8), z3)) ref = v3;
            }
            const float near_z = RDF32A(NEAR_Z);
            if (BELOW_EQ(RDF32A(ref + 8), near_z)) goto next;
            {
                uint32_t flags = 0, vis = 0;
                bool rejected = false;   // off one side of the screen
                if (BELOW_EQ(z0, near_z) || BELOW_EQ(z1, near_z) || BELOW_EQ(z2, near_z) || BELOW_EQ(z3, near_z)) {
                    // near face: 3D normal
                    const float ax = static_cast<float>(static_cast<fpu_t>(RDF32A(v1)) - RDF32A(v2));
                    const float ay = static_cast<float>(static_cast<fpu_t>(RDF32A(v1 + 4)) - RDF32A(v2 + 4));
                    const float az = static_cast<float>(static_cast<fpu_t>(z1) - z2);
                    const float bx = static_cast<float>(static_cast<fpu_t>(RDF32A(v1)) - RDF32A(v0));
                    const float by = static_cast<float>(static_cast<fpu_t>(RDF32A(v1 + 4)) - RDF32A(v0 + 4));
                    const float bz = static_cast<float>(static_cast<fpu_t>(z1) - z0);
                    const fpu_t nx = static_cast<fpu_t>(az) * by - static_cast<fpu_t>(ay) * bz;
                    const fpu_t ny = static_cast<fpu_t>(ax) * bz - static_cast<fpu_t>(az) * bx;
                    const fpu_t nz = static_cast<fpu_t>(ay) * bx - static_cast<fpu_t>(ax) * by;
                    flags = 1;
                    vis = float_bits(static_cast<float>((RDF32A(ref) * nx + ny * RDF32A(ref + 4)) + nz * RDF32A(ref + 8)));
                } else {
                    // screen space: off one side entirely?
                    const uint32_t vs[4] = {v0, v1, v2, v3};
                    const int32_t xmin = static_cast<int32_t>(RD32A(SCR_XMIN)), ymin = static_cast<int32_t>(RD32A(SCR_YMIN));
                    const uint32_t xmax = RD32A(SCR_XMAX), ymax = RD32A(SCR_YMAX);
                    uint32_t cx = 3, cy = 3;
                    for (uint32_t v : vs) {
                        const uint32_t x = RD32A(v + 0x10);
                        if (x > xmax) cx += static_cast<int32_t>(x) < xmin ? -1 : 1;
                    }
                    if (cx < 7) {
                        for (uint32_t v : vs) {
                            const uint32_t y = RD32A(v + 0x14);
                            if (y > ymax) cy += static_cast<int32_t>(y) < ymin ? -1 : 1;
                        }
                    }
                    if (cx < 7 && cy < 7) {
                        const uint32_t sx0 = RD32A(v0 + 0x10), sy0 = RD32A(v0 + 0x14), sx1 = RD32A(v1 + 0x10),
                                       sy1 = RD32A(v1 + 0x14), sx2 = RD32A(v2 + 0x10), sy2 = RD32A(v2 + 0x14);
                        const uint32_t cross = (sy2 - sy1) * (sx1 - sx0) + (sy0 - sy1) * (sx2 - sx1);
                        vis = float_bits(static_cast<float>(static_cast<fpu_t>(static_cast<int32_t>(cross))));
                    } else {
                        rejected = true;
                    }
                }
                // 0x49b7f0 tests the visibility first, 0x49c110 the clip rectangle first (also for
                // rejected faces); only edx (scratch) differs
                if (var == FACES_7F0 && static_cast<int32_t>(vis) <= 0x3f800000) goto next;   // not > 1.0
                (void)rejected;
                if (RD32A(CLIP_ON) && !RD32A(CLIP_OFF)) {
                    const int32_t xmin = static_cast<int32_t>(RD32A(SCR_XMIN)), xmax = static_cast<int32_t>(RD32A(SCR_XMAX));
                    const int32_t ymin = static_cast<int32_t>(RD32A(SCR_YMIN)), ymax = static_cast<int32_t>(RD32A(SCR_YMAX));
                    const uint32_t vs[4] = {v0, v1, v2, v3};
                    for (int k = 0; k < 4; k++) {
                        const int32_t x = static_cast<int32_t>(RD32A(vs[k] + 0x10)), y = static_cast<int32_t>(RD32A(vs[k] + 0x14));
                        // edx as 0x49c110 leaves it: v0's y bounds are compared without edx, the
                        // others' with edx = ymin; x bounds with edx = xmin, then xmax (v3: in ecx)
                        if (k > 0 && k < 3) edx = static_cast<uint32_t>(ymin);
                        if (k == 3) edx = static_cast<uint32_t>(ymin);
                        if (y < ymin || ymax < y) { flags |= 2; break; }
                        if (k < 3) edx = static_cast<uint32_t>(xmin);
                        if (x < xmin) { flags |= 2; break; }
                        if (k < 3) edx = static_cast<uint32_t>(xmax);
                        if (xmax < x) { flags |= 2; break; }
                    }
                }
                if (var == FACES_110 && static_cast<int32_t>(vis) <= 0x3f800000) goto next;
                PolyCall c;
                c.face = face;
                c.verts = verts;
                c.ecx = self;
                c.esp = frame - 0x18;   // 5 arguments + the return address
                if (var == FACES_7F0) {
                    c.target = 0x49c590;
                    c.a4 = flags;
                    c.a5 = 0;
                    c.edx = ref;
                    if (RD8(face + 0x38) & 2) {
                        c.depth = float_bits(static_cast<float>(static_cast<fpu_t>(RDF32A(ref + 8)) + RDF32A(DEPTH_BIAS)));
                        c.eax = face;
                    } else {
                        c.depth = RD32A(ref + 8);
                        c.eax = c.depth;
                    }
                } else {
                    c.depth = RD32A(ref + 8);
                    c.eax = c.depth;
                    if (RD32A(SPECIALS + RD16(face + 0x36) * 824u)) {
                        c.target = 0x49ca70;
                        c.a4 = arg4;
                        c.a5 = flags;
                        c.eax = ref;
                        c.edx = verts;
                    } else {
                        c.target = 0x49c590;
                        c.a4 = flags;
                        c.a5 = 0;
                        c.edx = ref;
                    }
                }
                if (dry) {
                    calls->push_back(c);
                    edx = EDX_UNKNOWN;
                } else {
                    WR32A(c.esp, var == FACES_7F0 ? 0x49bc73u : c.target == 0x49ca70 ? 0x49c542u : 0x49c562u);
                    WR32A(c.esp + 4, c.face);
                    WR32A(c.esp + 8, c.depth);
                    WR32A(c.esp + 12, c.verts);
                    WR32A(c.esp + 16, c.a4);
                    WR32A(c.esp + 20, c.a5);
                    R.esp = c.esp;
                    R.ecx = c.ecx;
                    R.eax = c.eax;
                    R.edx = c.edx;
                    F.sw = static_cast<uint16_t>(fsw);
                    if (c.target == 0x49ca70) f_0049ca70(); else f_0049c590();
                    fsw = F.sw;
                    edx = R.edx;
                }
            }
        }
    next:
        if (static_cast<int32_t>(count) <= static_cast<int32_t>(i)) {
            R.eax = i;
            R.ecx = i + 1;
            R.edx = var == FACES_7F0 ? count : edx;
            break;
        }
    }
    R.esp = sp + 20;
    F.sw = static_cast<uint16_t>(fsw);
}

// ---- 0x49c590: polygon setup — depth bucket, material, render node ----
// thiscall, stdcall args (face, depth, verts, flags, mode). Bucket: mode 1 -> 0, mode 2 -> 0x1f3f,
// else _ftol(depth * [0x4d1f9c]) (negative: nothing drawn), at most 0x1f3e; min / max used bucket
// [0x68b030] / [0x65e1d0]. face+0x3a = material (face+0): with a material override active
// ([0x4dc0c4] -> 0, not material flag 0x40, [0x4f1a18] clear) the material flags get the "near"
// bit 0x20 (per [0x4dc098] mode, depth < 3072 / 6144) and lose 0x85, and face+0x3a =
// [0x624b58 + flags*4]. Then (pool [0x655d14] < 0x1d4b) a 24-byte render node (face, depth, next,
// verts, 0, -1) is pushed onto the bucket's list [0x655d38 + bucket*4]. Faces to clip (flags with
// [0x4dc06c] set, [0x4dc070] clear) go through the clipper: left to the translated code.
constexpr uint32_t BUCKET_SCALE = 0x4d1f9c, BUCKET_MAX = 0x65e1d0, BUCKET_MIN = 0x68b030, MATERIALS = 0x6d1f88,
                   MAT_OVERRIDE = 0x4dc0c4, MAT_OVERRIDE_OFF = 0x4f1a18, MAT_MODE = 0x4dc098, MAT_JUMPS = 0x49ca54,
                   MAT_REMAP = 0x624b58, NODES = 0x65f0e0, NODE_COUNT = 0x655d14, HEADS = 0x655d38;
constexpr uint32_t NODE_LIMIT = 0x1d4b;

// the material jump table's targets (checked once against the code this was written from)
enum MatCase { CASE_A, CASE_B, CASE_C, CASE_D, CASE_NONE, CASE_UNKNOWN };
MatCase mat_case(uint32_t mode) {
    MEM_LOCAL();
    switch (RD32A(MAT_JUMPS + mode * 4)) {
    case 0x49c669: return CASE_A;
    case 0x49c687: return CASE_B;
    case 0x49c6ab: return CASE_C;
    case 0x49c6c0: return CASE_D;
    case 0x49c6d6: return CASE_NONE;
    default: return CASE_UNKNOWN;
    }
}
bool poly_supported() {
    static const bool ok = [] {
        for (uint32_t k = 0; k <= 5; k++)
            if (mat_case(k) == CASE_UNKNOWN) { rt::log("native geom: unexpected material jump table, 0x49c590 not native\n"); return false; }
        return true;
    }();
    return ok;
}

// Returns false (nothing done) for the clipping path.
bool poly_setup() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, face = RD32A(sp + 4), depth = RD32A(sp + 8), verts = RD32A(sp + 12), flags = RD32A(sp + 16),
                   mode = RD32A(sp + 20);
    if (flags && RD32A(CLIP_ON) && !RD32A(CLIP_OFF)) return false;
    R.esp = sp + 0x18;   // ret 0x14
    uint32_t bucket;
    if (mode == 1) {
        bucket = 0;
    } else if (mode == 2) {
        bucket = 0x1f3f;
    } else {
        const int64_t r = FTOL(static_cast<fpu_t>(RDF32A(sp + 8)) * static_cast<fpu_t>(RDF32A(BUCKET_SCALE)));
        const uint32_t lo = static_cast<uint32_t>(r);
        if (static_cast<int32_t>(lo) < 0) {
            R.eax = lo;
            R.edx = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
            return true;
        }
        bucket = static_cast<int32_t>(lo) >= 0x1f3f ? 0x1f3e : lo;
    }
    if (static_cast<int32_t>(bucket) > static_cast<int32_t>(RD32A(BUCKET_MAX))) WR32A(BUCKET_MAX, bucket);
    if (static_cast<int32_t>(RD32A(BUCKET_MIN)) > static_cast<int32_t>(bucket)) WR32A(BUCKET_MIN, bucket);
    const uint16_t material = RD16(face);
    WR16(face + 0x3a, material);
    uint32_t mat = RD32A(MATERIALS + material * 4u), edx = RD32A(MAT_OVERRIDE);
    if (edx) {
        edx = RD32A(edx);
        if (!edx && !(mat & 0x40) && !RD32A(MAT_OVERRIDE_OFF)) {
            const uint32_t mm = RD32A(MAT_MODE);
            const int32_t d = static_cast<int32_t>(depth);
            switch (mm <= 5 ? mat_case(mm) : CASE_NONE) {
            case CASE_A:
                if ((mat & 1) && d < 0x45400000) mat |= 0x20;
                mat &= 0xffffff7au;
                break;
            case CASE_B:
                if ((mat & 1) && d < 0x45400000) mat |= 0x20;
                if (!(RD8(face + 0x38) & 8)) mat &= 0xffffff7au;
                break;
            case CASE_C:
                if (d < 0x45400000 && (mat & 1)) mat |= 0x20;
                break;
            case CASE_D:
                if (d < 0x45c00000 && (mat & 1)) mat |= 0x20;
                break;
            default: break;
            }
            WR16(face + 0x3a, static_cast<uint16_t>(RD32A(MAT_REMAP + mat * 4u)));
        }
    }
    const uint32_t n = RD32A(NODE_COUNT);
    if (static_cast<int32_t>(n) >= static_cast<int32_t>(NODE_LIMIT)) {
        R.eax = n;
        R.ecx = mat;
        R.edx = edx;
        return true;
    }
    const uint32_t node = NODES + n * 24u, head = HEADS + bucket * 4u;
    WR32A(node, face);
    WR32A(node + 8, RD32A(head));
    WR32A(node + 12, verts);
    WR32A(node + 4, depth);
    WR32A(head, node);
    WR32A(node + 16, 0);
    WR32A(node + 20, 0xffffffffu);
    WR32A(NODE_COUNT, n + 1);
    R.eax = n + 1;
    R.ecx = head;
    R.edx = node;
    return true;
}

// the bucket poly_setup would use (check mode: which list head to compare)
uint32_t poly_bucket() {
    MEM_LOCAL();
    const uint32_t mode = RD32A(R.esp + 20);
    if (mode == 1) return 0;
    if (mode == 2) return 0x1f3f;
    const int32_t lo = static_cast<int32_t>(FTOL(static_cast<fpu_t>(RDF32A(R.esp + 8)) * static_cast<fpu_t>(RDF32A(BUCKET_SCALE))));
    return lo < 0 ? 0 : lo >= 0x1f3f ? 0x1f3e : static_cast<uint32_t>(lo);
}


// ---- 0x497080: clip a triangle against the screen rectangle (Sutherland-Hodgman) ----
// stdcall (poly, verts): poly = a triangle record (+0x2c three vertex indexes, +4 three (u, v)
// pairs), verts = transformed vertices (28 bytes). Clip vertices (28 bytes): screen x, y (int), the
// light value (int, flags << 13), u/z, v/z, 1/z (floats), z (float, final pass). Lists of up to 8
// (closed: the first vertex repeated at the end) in two buffers that swap after each pass, count at
// +0xe0. Passes: x <= xmax + 1, y <= ymax + 1, x > xmin, y > ymin. A new vertex on an edge gets the
// limit as its clipped coordinate, the other ints as _ftol(v + (n - v) * d / e) and the floats as
// (n - v) * t + v, with d = limit - v, e = n - v, t = d / e (its +0x18 is left as it was). Returns
// the final list (eax); its u, v are divided back by 1/z.
constexpr uint32_t CLIP_SRC = 0x60d3bc, CLIP_DST = 0x60d3b8, CLIP_WRITE = 0x60d1e0;
constexpr uint32_t CLIP_BUF_A = 0x60d2d0, CLIP_BUF_B = 0x60d1e8;

void copy7(uint32_t to, uint32_t from) {
    MEM_LOCAL();
    for (uint32_t k = 0; k < 28; k += 4) WR32A(to + k, RD32A(from + k));
}

void clip_intersect(uint32_t w, uint32_t v, uint32_t n, uint32_t coord, int32_t lim, fpu_t d, fpu_t e) {
    MEM_LOCAL();
    const fpu_t t = d / e;
    const uint32_t other = coord ? 0 : 4;
    const int32_t vo = static_cast<int32_t>(RD32A(v + other)), v8 = static_cast<int32_t>(RD32A(v + 8));
    const fpu_t ido = static_cast<fpu_t>(static_cast<int32_t>(RD32A(n + other) - RD32A(v + other)));
    const fpu_t id8 = static_cast<fpu_t>(static_cast<int32_t>(RD32A(n + 8) - RD32A(v + 8)));
    WR32A(w + other, static_cast<uint32_t>(FTOL(static_cast<fpu_t>(vo) + ido * d / e)));
    WR32A(w + coord, static_cast<uint32_t>(lim));
    WR32A(w + 8, static_cast<uint32_t>(FTOL(static_cast<fpu_t>(v8) + id8 * d / e)));
    for (uint32_t k = 0xc; k <= 0x14; k += 4) {
        const float fv = RDF32A(v + k);
        WRF32A(w + k, static_cast<float>((static_cast<fpu_t>(RDF32A(n + k)) - fv) * t + fv));
    }
}

void clip_pass(uint32_t coord, int32_t lim, bool is_max) {
    MEM_LOCAL();
    const uint32_t src = RD32A(CLIP_SRC), dst = RD32A(CLIP_DST);
    uint32_t w = RD32A(CLIP_WRITE), cnt = RD32A(dst + 0xe0);
    const int32_t count = static_cast<int32_t>(RD32A(src + 0xe0));
    for (int32_t i = 0; i < count; i++) {
        const uint32_t v = src + 0x1c * static_cast<uint32_t>(i), n = v + 0x1c;
        const int32_t vc = static_cast<int32_t>(RD32A(v + coord)), nc = static_cast<int32_t>(RD32A(n + coord));
        const fpu_t e = static_cast<fpu_t>(static_cast<int32_t>(static_cast<uint32_t>(nc) - static_cast<uint32_t>(vc)));
        const fpu_t d = static_cast<fpu_t>(static_cast<int32_t>(static_cast<uint32_t>(lim) - static_cast<uint32_t>(vc)));
        const bool v_out = is_max ? vc > lim : lim >= vc, n_out = is_max ? nc > lim : lim >= nc;
        if (!v_out) {
            copy7(w, v);
            w += 0x1c;
            cnt++;
            if (n_out) {
                clip_intersect(w, v, n, coord, lim, d, e);
                w += 0x1c;
                cnt++;
            }
        } else if (!n_out) {
            clip_intersect(w, v, n, coord, lim, d, e);
            w += 0x1c;
            cnt++;
        }
    }
    WR32A(dst + 0xe0, cnt);
    WR32A(CLIP_WRITE, w);
}

void clip_close_and_swap() {
    MEM_LOCAL();
    const uint32_t src = RD32A(CLIP_SRC), dst = RD32A(CLIP_DST);
    copy7(RD32A(CLIP_WRITE), dst);
    WR32A(CLIP_SRC, dst);
    WR32A(CLIP_DST, src);
    WR32A(CLIP_WRITE, src);
    WR32A(src + 0xe0, 0);
}

void clip_triangle() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, poly = RD32A(sp + 4), verts = RD32A(sp + 8);
    WR32A(CLIP_SRC, CLIP_BUF_A);
    WR32A(CLIP_DST, CLIP_BUF_B);
    for (uint32_t k = 0; k < 3; k++) {
        const uint32_t vp = verts + RD16(poly + 0x2c + 2 * k) * 28u, o = CLIP_BUF_A + 0x1c * k;
        WR32A(o, RD32A(vp + 0x10));
        WR32A(o + 4, RD32A(vp + 0x14));
        WR32A(o + 0x14, RD32A(vp + 0x18));
        const fpu_t rhw = RDF32A(o + 0x14);
        WRF32A(o + 0xc, static_cast<float>(rhw * RDF32(poly + 4 + 8 * k)));
        WRF32A(o + 0x10, static_cast<float>(rhw * RDF32(poly + 8 + 8 * k)));
        WR32A(o + 8, RD32A(vp + 0xc));
    }
    copy7(CLIP_BUF_A + 0x54, CLIP_BUF_A);
    WR32A(CLIP_BUF_A + 0xe0, 3);
    WR32A(CLIP_WRITE, CLIP_BUF_B);
    WR32A(CLIP_BUF_B + 0xe0, 0);
    clip_pass(0, static_cast<int32_t>(RD32A(SCR_XMAX) + 1), true);
    clip_close_and_swap();
    clip_pass(4, static_cast<int32_t>(RD32A(SCR_YMAX) + 1), true);
    clip_close_and_swap();
    clip_pass(0, static_cast<int32_t>(RD32A(SCR_XMIN)), false);
    clip_close_and_swap();
    clip_pass(4, static_cast<int32_t>(RD32A(SCR_YMIN)), false);
    const uint32_t out = RD32A(CLIP_DST);
    const int32_t n = static_cast<int32_t>(RD32A(out + 0xe0));
    uint32_t i = 0;
    for (; static_cast<int32_t>(i) < n; i++) {
        const uint32_t v = out + 0x1c * i;
        WRF32A(v + 0x18, static_cast<float>(static_cast<fpu_t>(1.0) / static_cast<fpu_t>(RDF32A(v + 0x14))));
        WRF32A(v + 0xc, static_cast<float>(static_cast<fpu_t>(RDF32A(v + 0x18)) * RDF32A(v + 0xc)));
        WRF32A(v + 0x10, static_cast<float>(static_cast<fpu_t>(RDF32A(v + 0x18)) * RDF32A(v + 0x10)));
    }
    R.eax = out;
    R.ecx = n > 0 ? i : 0;
    R.edx = n > 0 ? i * 0x1c : 0;
    R.esp = sp + 12;   // ret 8
}

}  // namespace

extern "C" int native_poly_setup(void) {   // 0x49c590
    MEM_LOCAL();
    if (recording) record_call(0x49c590);
    const Mode md = mode();
    Timed timed(-1);   // ~100k calls per 5 s: timed inside the face loops only (the clock reads would dominate)
    if (md == OFF || !poly_supported()) {
        if (timed.t) { f_0049c590_t(); return 1; }
        return 0;
    }
    if (md == ON) {
        if (poly_setup()) return 1;
        if (timed.t) { f_0049c590_t(); return 1; }
        return 0;
    }
    const uint32_t face = RD32A(R.esp + 4), n = RD32A(NODE_COUNT);
    const std::vector<Region> regions = {{BUCKET_MIN, 4}, {BUCKET_MAX, 4}, {NODE_COUNT, 4}, {face + 0x3a, 2},
                                         {NODES + n * 24u, 24}, {HEADS + poly_bucket() * 4u, 4}};
    Snapshot before = take(regions);
    if (!poly_setup()) return 0;   // clipping: translated only
    Snapshot native = take(regions);
    restore(before);
    f_0049c590_t();
    compare("poly setup 0x49c590", native, take(regions));
    return 1;
}

namespace {

int run_faces(Variant var, int timing, void (*translated)(void), const char *name) {
    MEM_LOCAL();
    const Mode md = mode();
    Timed timed(timing);
    if (md == OFF) {
        if (timed.t) { translated(); return 1; }
        return 0;
    }
    if (md == ON) {
        faces(var, false, nullptr);
        return 1;
    }
    check_aligned("verts", RD32A(R.esp + 4));   // transformed vertices
    // check: the native version records its calls (dry run); the translated one runs for real and
    // its calls are recorded at the callees' entries; calls, registers and the face counter compared
    Snapshot before = take({{FACE_COUNTER, 4}});
    std::vector<PolyCall> mine;
    faces(var, true, &mine);
    Snapshot native = take({{FACE_COUNTER, 4}});
    restore(before);
    recorded.clear();
    recording = true;
    translated();
    recording = false;
    Snapshot translated_state = take({{FACE_COUNTER, 4}});
    native.f.sw = translated_state.f.sw;   // set by the callees, which the dry run does not call
    if (native.r.edx == EDX_UNKNOWN) native.r.edx = translated_state.r.edx;
    compare(name, native, translated_state);
    if (mine.size() != recorded.size()) {
        if (++mismatches <= 20) rt::log("native geom: MISMATCH %s: %zu calls, translated %zu\n", name, mine.size(), recorded.size());
    } else {
        for (size_t k = 0; k < mine.size(); k++) {
            const PolyCall &a = mine[k], &b = recorded[k];
            if (memcmp(&a, &b, sizeof a)) {
                if (++mismatches <= 20)
                    rt::log("native geom: MISMATCH %s call %zu: target %x/%x face %x/%x depth %x/%x a4 %x/%x a5 %x/%x "
                            "ecx %x/%x eax %x/%x edx %x/%x esp %x/%x\n", name, k, a.target, b.target, a.face, b.face,
                            a.depth, b.depth, a.a4, b.a4, a.a5, b.a5, a.ecx, b.ecx, a.eax, b.eax, a.edx, b.edx, a.esp, b.esp);
                break;
            }
        }
    }
    return 1;
}

}  // namespace

extern "C" int native_faces(void) { return run_faces(FACES_7F0, 1, f_0049b7f0_t, "faces 0x49b7f0"); }
extern "C" int native_faces2(void) { return run_faces(FACES_110, 3, f_0049c110_t, "faces 0x49c110"); }
extern "C" int native_poly_special(void) {   // 0x49ca70: check mode records the calls
    if (recording) record_call(0x49ca70);
    return 0;
}

extern "C" int native_transform(void) {
    MEM_LOCAL();
    const Mode md = mode();
    Timed timed(0);
    if (md == OFF || RD32A(R.esp + 12) == 0) {   // count 0: the x86 loop would run 2^32 times
        if (timed.t) { f_0049b2c0_t(); return 1; }
        return 0;
    }
    if (md == ON) {
        transform();
        return 1;
    }
    const uint32_t used = RD32A(POOL_USED), n = RD32A(R.esp + 12);
    check_aligned("matrix", RD32A(R.esp + 4));
    check("transform 0x49b2c0", transform, f_0049b2c0_t, {{POOL_USED, 4}, {POOL + used * 28u, n * 28u}});
    return 1;
}

extern "C" int native_clip(void) {
    MEM_LOCAL();
    const Mode md = mode();
    Timed timed(4);
    if (md == OFF) {
        if (timed.t) { f_00497080_t(); return 1; }
        return 0;
    }
    if (md == ON) {
        clip_triangle();
        return 1;
    }
    check("clip 0x497080", clip_triangle, f_00497080_t,
          {{CLIP_BUF_B, 0xe8}, {CLIP_BUF_A, 0xe8}, {CLIP_SRC, 4}, {CLIP_DST, 4}, {CLIP_WRITE, 4}});
    return 1;
}
