// Native versions of the hottest parts of the game's render-list flush (docs/24-native-render.md).
//
// 0x4af570 walks the depth-sorted polygon lists and builds Direct3D execute buffers through the
// execute-buffer object [0x624afc]. Its two per-vertex loops were ~30% of the game's x86 instructions
// in a race (docs/22-hot-functions.md). The versions here do exactly
// what the translated x86 does — same bytes in guest memory, same registers and stack slots where
// the following code reads them — so they can be checked against it at run time:
//   MOTORACER_NATIVE=0      translated code only
//   MOTORACER_NATIVE=check  run both on the same input and log any difference
//   (default)               native
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime.h"
// Guest memory through a local base pointer (MEM_LOCAL), as in the generated code: with the global M,
// every store would make the compiler reload M.
#include "recomp_mem_local.h"

namespace {

enum Mode { OFF, ON, CHECK };

Mode mode() {
    static const Mode m = [] {
        const char *e = getenv("MOTORACER_NATIVE");
        Mode r = !e ? ON : !strcmp(e, "0") ? OFF : !strcmp(e, "check") ? CHECK : ON;
        rt::log("native render: %s\n", r == OFF ? "off" : r == ON ? "on" : "check against translated code");
        return r;
    }();
    return m;
}

constexpr uint32_t EB_OBJ = 0x624afc;       // execute-buffer object; +4 vertex write pointer, +8 instruction pointer
constexpr uint32_t COLOR = 0x709370;        // current polygon colour (ARGB)
constexpr uint32_t SHADE_TABLE = 0x5d43b0;  // 32 rows of 256 bytes: intensity-scaled colour components
constexpr uint32_t Z_ON = 0x53bce4, Z_RAW = 0x4dc1cc;
constexpr uint32_t Z_SCALE = 0x4d1fd0;      // double
constexpr uint32_t HALF_LIGHT = 0x53bb38;   // gouraud intensity halved (some materials)
constexpr uint32_t LIGHT_EVERY_VERTEX = 0x4dc1d8;
constexpr uint32_t WHITE_A = 0x4dc1bc, WHITE_B = 0x4dc1c0;

inline float int_to_float(uint32_t v) {
    return static_cast<float>(static_cast<fpu_t>(static_cast<int32_t>(v)));
}

// ---- the untextured (gouraud, colour from the shade table) vertex loop, 0x4af89b..0x4af9a3 ----
// In: esi = polygon record, edi = vertex count (> 0), ebx = render node ([+0xc] transformed
// vertices, 28 bytes each). Out (as the translated loop leaves them): [esp+0x14] last colour,
// [esp+0x18] last vertex, [esp+0x1c] last shade row, [esp+0x20] count; eax = EB object, ecx, edx =
// vertex write pointer, esi past the indexes, edi = 0, ebp = last shade row.
struct LoopOut { uint32_t eax, ecx, edx, esi, edi, ebp, s14, s18, s1c, s20; };

// The loops only write the vertex buffer, so the globals they read (colour, z mode, the execute-buffer
// object) are read once.
struct ZMode { bool on, raw; fpu_t scale; };
ZMode z_mode() {
    MEM_LOCAL();
    return {RD32(Z_ON) != 0, RD32(Z_RAW) != 0, static_cast<fpu_t>(RDF64(Z_SCALE))};
}

LoopOut flat_vertices(uint32_t esi, uint32_t n, uint32_t ebx) {
    MEM_LOCAL();
    const uint32_t verts = RD32(ebx + 0xc), eb = RD32(EB_OBJ), col = RD32(COLOR);
    const uint32_t alpha = col & 0xff000000u, ri = (col >> 16) & 0xff, gi = (col >> 8) & 0xff, bi = col & 0xff;
    const ZMode z = z_mode();
    uint32_t idx = esi + 0x2c, v = 0, row = 0, rgb = 0, ecx = 0, out = RD32(eb + 4);
    for (uint32_t i = 0; i < n; i++, out += 0x20, idx += 2) {
        v = verts + RD16(idx) * 28u;
        WRF32(out, int_to_float(RD32(v + 0x10)));
        WRF32(out + 4, int_to_float(RD32(v + 0x14)));
        row = SHADE_TABLE + ((RD32(v + 0xc) & 0x3e000u) >> 5);
        rgb = static_cast<uint32_t>(RD8(row + ri)) << 16 | static_cast<uint32_t>(RD8(row + gi)) << 8 | RD8(row + bi);
        WR32(out + 0x14, rgb | 0xff000000u);
        WR32(out + 0x10, alpha | rgb);
        if (!z.on) {
            WR32(out + 8, 0x3f800000u);
            ecx = rgb;
        } else if (z.raw) {
            ecx = RD32(v + 8);
            WR32(out + 8, ecx);
        } else {
            WRF32(out + 8, static_cast<float>(static_cast<fpu_t>(RDF32(v + 8)) * z.scale));
            ecx = rgb;
        }
    }
    WR32(eb + 4, out);
    return {eb, ecx, out, idx, 0, row, rgb, v, row, n};
}

// ---- the textured vertex loop, 0x4af9af..0x4af9fc, with 0x4af420(vertex, uv, index) inlined ----
// Per vertex: x, y, z, rhw, uv; the grey intensity colour (lighting bits 10-17, halved when
// [0x53bb38]) only for the first vertex unless [0x4dc1d8] = 1; bit 31: white specular and the
// vertex colour; bit 30: white (opaque white when [0x4dc1bc] or [0x4dc1c0]).
// In: esi = polygon record (uv pairs at +4, indexes at +0x2c), edi = count (> 0), ebx = node.
// Out: [esp+0x14] uv pointer, [esp+0x18] last vertex, [esp+0x20] count; eax = count, ecx = uv
// pointer, esi past the indexes, edi = count (edx is dead after the loop; ebp unchanged).
LoopOut tex_vertices(uint32_t esi, uint32_t n, uint32_t ebx, uint32_t ebp, uint32_t esp) {
    MEM_LOCAL();
    const uint32_t verts = RD32(ebx + 0xc), eb = RD32(EB_OBJ), alpha = RD32(COLOR) & 0xff000000u;
    const bool every = RD32(LIGHT_EVERY_VERTEX) == 1, half = RD32(HALF_LIGHT) != 0;
    const bool white = RD32(WHITE_A) || RD32(WHITE_B);
    const ZMode z = z_mode();
    uint32_t uv = esi + 4, idx = esi + 0x2c, v = 0, out = RD32(eb + 4);
    for (uint32_t i = 0; i < n; i++, uv += 8, idx += 2, out += 0x20) {
        v = verts + RD16(idx) * 28u;
        WRF32(out + 4, int_to_float(RD32(v + 0x14)));
        WRF32(out, int_to_float(RD32(v + 0x10)));
        WR32(out + 0xc, RD32(v + 0x18));
        WR32(out + 0x18, RD32(uv));
        WR32(out + 0x1c, RD32(uv + 4));
        if (!z.on) WR32(out + 8, 0x3f800000u);
        else if (z.raw) WR32(out + 8, RD32(v + 8));
        else WRF32(out + 8, static_cast<float>(static_cast<fpu_t>(RDF32(v + 8)) * z.scale));
        if (i == 0 || every) {
            const uint32_t c = RD32(v + 0xc);
            if (c & 0x80000000u) {
                WR32(out + 0x14, 0xffffffffu);
                WR32(out + 0x10, c & 0xffffff);
            } else if (c & 0x40000000u) {
                WR32(out + 0x10, white ? 0xffffffffu : 0xffffffu);
            } else {
                uint32_t e = (c & 0x3fc00u) >> 10;
                if (half) e >>= 1;
                const uint32_t grey = e << 16 | e << 8 | e;
                WR32(out + 0x14, grey | 0xff000000u);
                WR32(out + 0x10, alpha | grey);
            }
        }
    }
    WR32(eb + 4, out);
    return {n, uv, 0, idx, n, ebp, uv, v, RD32(esp + 0x1c), n};
}

void apply(const LoopOut &o) {
    MEM_LOCAL();
    R.eax = o.eax; R.ecx = o.ecx; R.edx = o.edx; R.esi = o.esi; R.edi = o.edi; R.ebp = o.ebp;
    WR32(R.esp + 0x14, o.s14); WR32(R.esp + 0x18, o.s18); WR32(R.esp + 0x1c, o.s1c); WR32(R.esp + 0x20, o.s20);
}

// ---- check mode for the loops: run native first, keep its results, undo, let the translated loop
// run, compare at 0x4af9fe ----
struct Pending {
    bool active = false, textured = false;
    uint32_t out0 = 0, n = 0;
    std::vector<uint8_t> native_bytes;
    LoopOut native{};
} pending;
uint64_t checks = 0, mismatches = 0;

// Check mode also measures both versions on the same inputs (game thread instructions / cycles).
struct Cost { uint64_t insns = 0, cycles = 0, calls = 0; };
Cost cost_native[1], cost_translated[1];
struct Meter {
    uint64_t i0, c0;
    Meter() { rt::thread_counts(&i0, &c0); }
    void add(Cost &c) const { uint64_t i, y; rt::thread_counts(&i, &y); c.insns += i - i0; c.cycles += y - c0; c.calls++; }
};
uint64_t loop_t_i0 = 0, loop_t_c0 = 0;   // translated loop: from the hook's return to 0x4af9fe

void report(const char *what, uint32_t a, uint32_t b) {
    if (++mismatches <= 20)
        rt::log("native render: MISMATCH %s: native 0x%08x translated 0x%08x (check %llu)\n", what, a, b,
                static_cast<unsigned long long>(checks));
}

int run_loop(bool textured) {
    MEM_LOCAL();
    const Mode m = mode();
    if (m == OFF) return 0;
    const uint32_t esi = R.esi, n = R.edi, ebx = R.ebx, esp = R.esp;
    if (m == ON) {
        apply(textured ? tex_vertices(esi, n, ebx, R.ebp, esp) : flat_vertices(esi, n, ebx));
        return 1;
    }
    // CHECK
    const uint32_t eb = RD32(EB_OBJ), out0 = RD32(eb + 4);
    std::vector<uint8_t> before(n * 32u), stack(0x10);
    memcpy(before.data(), M + out0, before.size());
    memcpy(stack.data(), M + esp + 0x14, 0x10);
    Meter meter;
    LoopOut o = textured ? tex_vertices(esi, n, ebx, R.ebp, esp) : flat_vertices(esi, n, ebx);
    meter.add(cost_native[0]);
    pending.active = true;
    pending.textured = textured;
    pending.out0 = out0;
    pending.n = n;
    pending.native = o;
    pending.native_bytes.assign(M + out0, M + out0 + n * 32u);
    memcpy(M + out0, before.data(), before.size());   // undo: the translated loop runs on the same input
    memcpy(M + esp + 0x14, stack.data(), 0x10);
    WR32(eb + 4, out0);
    rt::thread_counts(&loop_t_i0, &loop_t_c0);
    return 0;
}

}  // namespace

extern "C" int native_flat_vertices(void) { return run_loop(false); }
extern "C" int native_tex_vertices(void) { return run_loop(true); }

// GAME_HOOK at 0x4af9fe (after both loops; also reached when a polygon has no vertices).
extern "C" void native_vertices_check(void) {
    MEM_LOCAL();
    if (!pending.active) return;
    {
        uint64_t i, c;
        rt::thread_counts(&i, &c);
        cost_translated[0].insns += i - loop_t_i0;
        cost_translated[0].cycles += c - loop_t_c0;
        cost_translated[0].calls++;
    }
    pending.active = false;
    checks++;
    const uint32_t out0 = pending.out0;
    if (memcmp(M + out0, pending.native_bytes.data(), pending.native_bytes.size())) {
        for (uint32_t i = 0; i < pending.native_bytes.size(); i += 4) {
            uint32_t a, b;
            memcpy(&a, pending.native_bytes.data() + i, 4);
            memcpy(&b, M + out0 + i, 4);
            if (a != b) { report(pending.textured ? "textured vertex byte" : "flat vertex byte", a, b); break; }
        }
    }
    const LoopOut &o = pending.native;
    if (RD32(RD32(EB_OBJ) + 4) != out0 + pending.n * 32u) report("vertex pointer", out0 + pending.n * 32u, RD32(RD32(EB_OBJ) + 4));
    if (o.eax != R.eax) report("eax", o.eax, R.eax);
    if (o.ecx != R.ecx) report("ecx", o.ecx, R.ecx);
    if (!pending.textured && o.edx != R.edx) report("edx", o.edx, R.edx);
    if (o.esi != R.esi) report("esi", o.esi, R.esi);
    if (o.edi != R.edi) report("edi", o.edi, R.edi);
    if (o.ebp != R.ebp) report("ebp", o.ebp, R.ebp);
    if (o.s14 != RD32(R.esp + 0x14)) report("[esp+0x14]", o.s14, RD32(R.esp + 0x14));
    if (o.s18 != RD32(R.esp + 0x18)) report("[esp+0x18]", o.s18, RD32(R.esp + 0x18));
    if (o.s1c != RD32(R.esp + 0x1c)) report("[esp+0x1c]", o.s1c, RD32(R.esp + 0x1c));
    if (o.s20 != RD32(R.esp + 0x20)) report("[esp+0x20]", o.s20, RD32(R.esp + 0x20));
    if (checks % 100000 == 0) {
        rt::log("native render: %llu checks, %llu mismatches\n", static_cast<unsigned long long>(checks),
                static_cast<unsigned long long>(mismatches));
        static const char *names[1] = {"vertex loops"};
        for (int k = 0; k < 1; k++)
            if (cost_native[k].calls && cost_translated[k].calls)
                rt::log("native render: %s (%llu calls): saved %.1f instructions, %.1f cycles per call "
                        "(native - translated, incl. measuring)\n", names[k],
                        static_cast<unsigned long long>(cost_native[k].calls),
                        static_cast<double>(cost_translated[k].insns) / cost_translated[k].calls -
                            static_cast<double>(cost_native[k].insns) / cost_native[k].calls,
                        static_cast<double>(cost_translated[k].cycles) / cost_translated[k].calls -
                            static_cast<double>(cost_native[k].cycles) / cost_native[k].calls);
    }
}

// GAME_HOOK at 0x4afd00 (bucket advance of the flush): the flush visits all ~8,000 depth buckets every
// frame (`head[b] = 0; b += step; until b == end`) to draw ~600 polygons — more than half of its
// instructions. Clear the current bucket, then move b to the last empty bucket before the next
// non-empty one (or the end): the translated block then takes its one final step as before. Not
// with the abort flag [esp+0x30] or a pending resume ([esp+0x44] with [esp+0x48] = 1).
extern "C" void native_bucket_skip(void) {
    MEM_LOCAL();
    if (mode() == OFF) return;
    constexpr uint32_t HEADS = 0x655d38;
    const uint32_t sp = R.esp, step = RD32(sp + 0x50), end = RD32(sp + 0x40);
    if (RD32(sp + 0x30) || (RD32(sp + 0x44) && RD32(sp + 0x48) == 1)) return;
    uint32_t b = RD32(sp + 0x28);
    WR32(HEADS + 4 * b, 0);
    for (uint32_t nb = b + step; nb != end && RD32(HEADS + 4 * nb) == 0; nb += step) b = nb;
    WR32(sp + 0x28, b);
}
