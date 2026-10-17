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
#include "native_check.h"

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

// ---- one polygon of the render-list flush, 0x4af6cb..0x4afcd3 (docs/24-native-render.md) ----
// Jump hook at 0x4af6cb with three exits: 1 = next node (0x4afcd3), 2 = no room left in the execute
// buffer (0x4afcd8), 3 = an emitter reported a full buffer (0x4afd3e). The flush's stack slots and
// the globals are left as the translated code leaves them; registers other than ebx are dead at
// the exits. The execute-buffer object's room check (0x41ad80) and texture-state reset (0x41e730) are
// inlined; its render-state methods and the triangle / line emitters stay translated (guest calls).
extern "C" void f_0041e2b0(void);
extern "C" void f_0041e380(void);
extern "C" void f_0041e000(void);
extern "C" void f_0041e1b0(void);
extern "C" void f_0041dde0(void);
extern "C" void f_00419d60(void);
extern "C" void f_00419ee0(void);

namespace {

// thiscall / stdcall guest call with the callee popping its arguments; returns eax
uint32_t guest_call(void (*fn)(void), uint32_t ret_va, uint32_t self, std::initializer_list<uint32_t> args) {
    MEM_LOCAL();
    const uint32_t sp = R.esp;
    uint32_t a = sp - 4 * static_cast<uint32_t>(args.size());
    for (uint32_t v : args) { WR32(a, v); a += 4; }
    R.esp = sp - 4 * static_cast<uint32_t>(args.size()) - 4;
    WR32(R.esp, ret_va);
    R.ecx = self;
    fn();
    R.esp = sp;
    return R.eax;
}

constexpr uint32_t TEX_STATE = 0x4e4f3c, TEX_RESET = 0xffffff9cu;
constexpr uint32_t ROOM_VERTS = 0x53bdf8, ROOM_VERTS_MAX = 0x53bcf4, ROOM_INS_END = 0x53bcec;


// ---- execute-buffer object methods: triangle 0x419d60, line 0x419ee0, blend states 0x41dde0 ----
// eb = the object (+8 instruction write pointer). Same bytes, globals and return value as the
// translated methods (registers other than eax are not used by the callers).
constexpr uint32_t INS_END = 0x53bcec, PRIM_HEADER = 0x53bcf0, BLEND_ON = 0x4e4f2c;

// the texture STATERENDER + a new primitive header when the texture changed (both emitters)
void emit_texture_header(uint32_t eb, uint32_t id, uint32_t flags, uint32_t var, uint8_t op, uint8_t size) {
    MEM_LOCAL();
    uint32_t ip = RD32(eb + 8);
    WR8(ip, 8);
    WR8(ip + 1, 8);
    WR16(ip + 2, 1);
    ip += 4;
    WR32(eb + 8, ip);
    const int32_t sid = static_cast<int32_t>(id);
    uint32_t h = 0xffffffffu;
    if ((flags & 1) && sid >= 0 && sid < 0x2d) h = RD32(0x4f9550 + 4 * (id * 206 + var));
    WR32(ip, 1);
    WR32(ip + 4, h != 0xffffffffu ? RD32(0x53c538 + 4 * h) : 0);
    WR32(eb + 8, ip + 8);
    WR32(0x4e4f3c, id);
    ip = RD32(eb + 8);
    WR32(PRIM_HEADER, ip);
    WR8(ip, op);
    WR8(ip + 1, size);
    WR16(ip + 2, 0);
    WR32(eb + 8, ip + 4);
}

uint32_t emit_triangle(uint32_t eb, uint32_t first, uint32_t shape, uint32_t id, uint32_t flags, uint32_t var) {
    MEM_LOCAL();
    if (!(RD32(eb + 8) + 4 < RD32(INS_END))) return 0;
    const uint32_t cur = RD32(0x4e4f3c);
    if (cur != id || cur == 0xffffff9cu) emit_texture_header(eb, id, flags, var, 3, 8);
    const uint32_t hdr = RD32(PRIM_HEADER);
    WR16(hdr + 2, static_cast<uint16_t>(RD16(hdr + 2) + 1));
    const uint32_t p = RD32(eb + 8);
    const uint16_t f = static_cast<uint16_t>(first), sh = static_cast<uint16_t>(shape);
    WR16(p, f);
    if (sh == 2) { WR16(p + 2, f + 1); WR16(p + 4, f + 2); WR16(p + 6, 0x300); }
    else if (sh == 1) { WR16(p + 2, f + 2); WR16(p + 4, f + 3); WR16(p + 6, 0x1f); }
    else { WR16(p + 2, f + 1); WR16(p + 4, f + 2); WR16(p + 6, 0x700); }
    WR32(eb + 8, p + 8);
    return 1;
}

uint32_t emit_line(uint32_t eb, uint32_t first, uint32_t id, uint32_t flags, uint32_t var) {
    MEM_LOCAL();
    if (!(RD32(eb + 8) + 4 < RD32(INS_END))) return 0;
    const uint32_t cur = RD32(0x4e4f3c);
    if (id != cur || cur == 0xffffff9cu) emit_texture_header(eb, id, flags, var, 2, 4);
    const uint32_t hdr = RD32(PRIM_HEADER);
    WR16(hdr + 2, static_cast<uint16_t>(RD16(hdr + 2) + 1));
    const uint32_t p = RD32(eb + 8);
    WR16(p, static_cast<uint16_t>(first));
    WR16(p + 2, static_cast<uint16_t>(first + 1));
    WR32(eb + 8, p + 4);
    return 1;
}

void put_state(uint32_t eb, uint32_t type, uint32_t value) {
    MEM_LOCAL();
    const uint32_t ip = RD32(eb + 8);
    WR32(ip, type);
    WR32(ip + 4, value);
    WR32(eb + 8, ip + 8);
}

// 0x41dde0(enable, additive): alpha-blend render states; disabling when already off emits nothing
void blend_states(uint32_t eb, uint32_t enable, uint32_t additive) {
    MEM_LOCAL();
    const bool player_flag = RD32(RD32(0x53be30) * 404u + 0x53c770) != 0;
    if (!enable && RD32(BLEND_ON) == 0) return;
    WR32(BLEND_ON, enable ? 1 : 0);
    const uint32_t ip = RD32(eb + 8);
    WR8(ip, 8);
    WR8(ip + 1, 8);
    WR16(ip + 2, enable ? (player_flag ? 5 : 6) : (player_flag ? 3 : 4));
    WR32(eb + 8, ip + 4);
    if (enable) {
        if (additive) {
            put_state(eb, 0x13, RD32(0x4dc1cc) ? 5 : 6);
            put_state(eb, 0x14, 6);
        } else {
            put_state(eb, 0x13, 2);
            put_state(eb, 0x14, 3);
        }
        put_state(eb, 0x1b, RD32(0x4e4f10));
        if (!player_flag) put_state(eb, 0x21, RD32(0x4e4f10));
    } else {
        put_state(eb, 0x1b, 0);
        if (!player_flag) put_state(eb, 0x21, 0);
    }
    put_state(eb, 0xf, 0);
    put_state(eb, 0x15, RD32(0x53bc10));
    WR32(0x4e4f3c, 0xffffff9cu);
}

// 0x41e2b0(on): render states 0x11 / 0x12 = 2 (on) or 1 (off), cached in [0x4e4f34]; off when [0x53be0c] is clear
void wrap_states(uint32_t eb, uint32_t on) {
    MEM_LOCAL();
    uint32_t want;
    if (RD32(0x53be0c)) want = on;
    else if (!RD32(0x4e4f34)) return;
    else want = 0;
    if (want == RD32(0x4e4f34)) return;
    WR32(0x4e4f34, want ? 1 : 0);
    const uint32_t ip = RD32(eb + 8), v = want ? 2 : 1;
    WR8(ip, 8);
    WR8(ip + 1, 8);
    WR16(ip + 2, 2);
    WR32(eb + 8, ip + 4);
    put_state(eb, 0x11, v);
    put_state(eb, 0x12, v);
    WR32(0x4e4f3c, 0xffffff9cu);
}

// 0x41e380(value, force): render state 0x1a = value, cached in [0x4e4f38] unless force
void state_1a(uint32_t eb, uint32_t value, uint32_t force) {
    MEM_LOCAL();
    if (RD32(0x4e4f38) == value && !force) return;
    WR32(0x4e4f38, value);
    const uint32_t ip = RD32(eb + 8);
    WR8(ip, 8);
    WR8(ip + 1, 8);
    WR16(ip + 2, 1);
    WR32(eb + 8, ip + 4);
    put_state(eb, 0x1a, value);
    WR32(0x4e4f3c, 0xffffff9cu);
}

int flush_polygon() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, node = R.ebx, eb = RD32(EB_OBJ);
    WR32(0x624370, RD32(0x624370) + 1);
    WR32(0x51bfd0, RD32(0x51bfd0) + 1);
    // 0x41ad80: room for another polygon?
    if (static_cast<int32_t>(RD32(ROOM_VERTS) + 4) >= static_cast<int32_t>(RD32(ROOM_VERTS_MAX)) ||
        RD32(eb + 8) + 0x70 >= RD32(ROOM_INS_END)) {
        R.eax = 1;
        return 2;
    }
    WR32(sp + 0x4c, RD32(sp + 0x4c) + 1);
    uint32_t poly = RD32(node);
    const uint32_t ctab = RD32(node + 0x10);
    WR32(sp + 0x14, 0);
    bool untex = false;
    const uint16_t type = RD16(poly + 0x3a);
    if (type == 0xc || type == 5 || type == 0) {
        untex = true;
    } else {
        const uint32_t id = RD16(poly + 0x36);
        if (id >= 0x2e) {
            untex = true;
        } else {
            const uint32_t var = RD32(node + 0x14);
            if (RD32(0x4f9550 + 4 * (id * 206 + (var == 0xffffffffu ? 0 : var))) == 0xffffffffu) untex = true;
        }
    }
    if (untex) WR32(sp + 0x14, 1);
    // colour: from the object's 16-bit colour table (untextured), else the record's
    uint32_t c16;
    if (ctab && untex && (c16 = RD16(poly + 2)) < 0x100) {
        const uint32_t dx = RD16(ctab + 2 * c16);
        uint32_t c = ((RD32(0x4ee9d0) & dx) >> (RD8(0x4ee9c4) & 31)) << ((RD8(0x4ee9b8) + 0x10) & 31);
        c |= ((RD32(0x4ee9d4) & dx) >> (RD8(0x4ee9c8) & 31)) << ((RD8(0x4ee9bc) + 8) & 31);
        c |= (RD32(0x4ee9d8) & dx) << (RD8(0x4ee9c0) & 31);
        WR32(COLOR, c);
    } else {
        WR32(COLOR, RD32(poly + 0x3c));
    }
    WR32(HALF_LIGHT, (type == 0x15 || type == 0x27 || type == 0x20) ? 1 : 0);
    WR8(poly + 0x3f, 0xff);
    bool half_alpha = type == 0x2c || type == 0x3e || type == 0xc;
    if (!half_alpha && type == 0x44) half_alpha = RD32(RD32(0x53be30) * 404u + 0x53c76c) != 0;
    if (half_alpha) WR8(COLOR + 3, 0x80);
    // shape: line (2 vertices), triangle (3), quad (4, two triangles)
    WR32(sp + 0x20, 0);
    const bool line = (RD8(poly + 0x38) & 0x10) != 0;
    uint32_t n;
    if (line) { WR32(sp + 0x38, 0); n = 2; }
    else if (RD16(poly + 0x34) == 0xffff) { WR32(sp + 0x38, 0); n = 3; }
    else { WR32(sp + 0x38, 2); n = 4; }
    if (n) {
        WR32(sp + 0x20, n);   // the untextured loop's counter (the textured loop leaves it at n too)
        const LoopOut o = untex ? flat_vertices(poly, n, node) : tex_vertices(poly, n, node, R.ebp, sp);
        WR32(sp + 0x14, o.s14);
        WR32(sp + 0x18, o.s18);
        WR32(sp + 0x1c, o.s1c);
        WR32(sp + 0x20, o.s20);
    }
    // 0x4af9fe
    WR32(ROOM_VERTS, RD32(ROOM_VERTS) + RD32(sp + 0x20));
    const uint32_t lflags = RD32(RD32(sp + 0x18) + 0xc);
    poly = RD32(node);
    WR32(sp + 0x1c, 0);
    WR32(sp + 0x14, (lflags & 0x40000000u) ? 1 : 0);
    const uint32_t id = RD16(poly + 0x36);
    if (RD32(0x4f9300 + id * 824u)) WR32(sp + 0x1c, RD32(node + 0x10));
    const uint16_t cx = RD16(poly + 0x3a);
    const uint32_t mat = RD32(0x6d1f88 + 4u * cx);
    if (RD32(0x53be0c)) wrap_states(eb, (mat & 8) ? 0u : 1u);
    if (RD32(0x4e4f14) != 0xffffffffu) state_1a(eb, RD32(sp + 0x14) ? 0u : RD32(0x4e4f14), 0u);
    // render flags (edi) and the blend / alpha states
    uint32_t flags = 0;
    bool to_l4b = false, done = false;
    if (!(cx == 5 || cx == 0)) {
        if (cx == 0xc) to_l4b = true;
        else flags = 1;
    }
    if (!to_l4b) {
        if (cx == 0xc || cx == 0x2c || cx == 0x3e) {
            to_l4b = true;
        } else if (cx == 0x44 || cx == 0x45) {
            guest_call(f_0041e000, 0x4afb49, eb, {1u});
            done = true;
        } else {
            if (RD32(0x4dc1bc) || RD32(0x4dc1c0)) {
                const uint32_t tf = RD16(0x4f954c + id * 824u);
                if ((tf & 0x10) && (tf & 2) && (flags & 1)) {
                    guest_call(f_0041e1b0, 0x4afb34, eb, {1u});
                    done = true;
                }
            }
            if (!done) {
                blend_states(eb, 0, 0);
                done = true;
            }
        }
    }
    if (to_l4b) {
        flags |= 8;
        blend_states(eb, 1, cx != 0xc ? 1u : 0u);
    }
    // texture variant (special entries)
    const uint32_t var = RD32(node + 0x14);
    if (RD32(sp + 0x1c) && var != 0xffffffffu) {
        flags |= 0x10;
        const uint32_t p = 0x4f9620 + id * 824u;
        if (RD32(p) != var) {
            WR32(p, var);
            WR32(TEX_STATE, TEX_RESET);   // 0x41e730
        }
    } else {
        WR32(node + 0x14, 0);
    }
    if (flags != RD32(sp + 0x24)) {
        WR32(TEX_STATE, TEX_RESET);
        WR32(sp + 0x24, flags);
    }
    WR32(0x625730, RD32(0x625730) + 1);
    const uint32_t st = RD32(sp + 0x34);
    if (line) {
        if (st == 1) { WR32(sp + 0x34, 0); WR32(TEX_STATE, TEX_RESET); }
    } else {
        if (st == 0) { WR32(sp + 0x34, 1); WR32(TEX_STATE, TEX_RESET); }
    }
    // primitives
    const uint32_t f = RD32(sp + 0x24), v2 = RD32(node + 0x14);
    if (line) {
        if (!emit_line(eb, RD32(sp + 0x12), id, f, v2)) return 3;
        WR16(sp + 0x12, static_cast<uint16_t>(RD16(sp + 0x12) + 2));
    } else {
        const uint32_t shape = RD32(0x4e8930) ? RD32(sp + 0x38) : 0u;
        if (!emit_triangle(eb, RD32(sp + 0x12), shape, id, f, v2)) return 3;
        WR16(sp + 0x12, static_cast<uint16_t>(RD16(sp + 0x12) + 3));
    }
    if (RD32(sp + 0x38) && !line) {
        WR32(0x625730, RD32(0x625730) + 1);
        const uint32_t first = static_cast<uint16_t>(RD16(sp + 0x12) - 3);
        if (!emit_triangle(eb, first, 1u, id, RD32(sp + 0x24), RD32(node + 0x14))) return 3;
        WR16(sp + 0x12, static_cast<uint16_t>(RD16(sp + 0x12) + 1));
    }
    return 1;
}

struct FlushPending {
    bool active = false;
    int exit = 0;
    native::Snapshot native;
    std::vector<native::Region> regions;
} flush_pending;

}  // namespace

extern "C" int native_flush_polygon(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    const uint32_t sp = R.esp, node = R.ebx;
    if (md == ON) {
        const int r = flush_polygon();
        R.ebx = node;
        R.esp = sp;
        return r;
    }
    // check: native first, then the translated code on the same input; compared at the exits
    const uint32_t eb = RD32(EB_OBJ), poly = RD32(node), id = RD16(poly + 0x36);
    std::vector<native::Region> regions = {{eb, 16}, {RD32(eb + 4), 4 * 32}, {RD32(eb + 8), 0x70}, {sp, 0x60}, {poly, 0x40},
                                   {node, 0x18}, {0x624370, 4}, {0x51bfd0, 4}, {COLOR, 4}, {HALF_LIGHT, 4},
                                   {ROOM_VERTS, 4}, {0x625730, 4}, {TEX_STATE, 4}, {0x4e4f2c, 4}, {0x4e4f34, 8},
                                   {0x53bcf0, 4}, {0x4f9620 + id * 824u, 4},
                                   // the open TRIANGLE instruction's count (before the write pointer):
                                   // without it check mode counted triangles twice (glitches on screen)
                                   {RD32(0x53bcf0), 4}};
    native::Snapshot before = native::take(regions);
    const int r = flush_polygon();
    R.ebx = node;
    R.esp = sp;
    flush_pending.native = native::take(regions);
    flush_pending.exit = r;
    flush_pending.regions = regions;
    flush_pending.active = true;
    native::restore(before);
    return 0;
}

// GAME_HOOKs at the three exits (check mode)
extern "C" void native_flush_check(void) {
    if (!flush_pending.active) return;
    flush_pending.active = false;
    native::Snapshot tr = native::take(flush_pending.regions);
    native::Snapshot &nat = flush_pending.native;
    nat.r.eax = tr.r.eax; nat.r.ecx = tr.r.ecx; nat.r.edx = tr.r.edx;   // dead at the exits
    nat.r.esi = tr.r.esi; nat.r.edi = tr.r.edi; nat.r.ebp = tr.r.ebp;
    nat.f = tr.f;
    native::compare("flush polygon 0x4af6cb", nat, tr);
}

extern "C" void f_00419d60_t(void);
extern "C" void f_00419ee0_t(void);
extern "C" void f_0041dde0_t(void);
extern "C" void f_0041e2b0_t(void);
extern "C" void f_0041e380_t(void);

namespace {
// whole-function replacement (stdcall / thiscall): native, or check against the translated body
template <typename Fn>
int method(Fn fn, void (*translated)(void), uint32_t nargs, const char *name, bool returns = true) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    const uint32_t sp = R.esp, eb = R.ecx;
    if (md == ON) {
        R.eax = fn(sp, eb);
        R.esp = sp + 4 + 4 * nargs;
        return 1;
    }
    const uint32_t ip = RD32(eb + 8);
    const std::vector<native::Region> regions = {{eb, 16}, {ip, 0x80}, {0x4e4f3c, 4}, {PRIM_HEADER, 4}, {BLEND_ON, 4}, {0x4e4f34, 8},
                                                 {RD32(PRIM_HEADER), 4}};
    native::Snapshot before = native::take(regions);
    R.eax = fn(sp, eb);
    R.esp = sp + 4 + 4 * nargs;
    native::Snapshot nat = native::take(regions);
    native::restore(before);
    translated();
    native::Snapshot tr = native::take(regions);
    nat.r.ecx = tr.r.ecx; nat.r.edx = tr.r.edx;   // scratch
    if (!returns) nat.r.eax = tr.r.eax;
    native::compare(name, nat, tr);
    return 1;
}
}  // namespace

extern "C" int native_emit_triangle(void) {
    return method([](uint32_t sp, uint32_t eb) { MEM_LOCAL(); return emit_triangle(eb, RD32(sp + 4), RD32(sp + 8), RD32(sp + 12), RD32(sp + 16), RD32(sp + 20)); },
                  f_00419d60_t, 5, "triangle 0x419d60");
}
extern "C" int native_emit_line(void) {
    return method([](uint32_t sp, uint32_t eb) { MEM_LOCAL(); return emit_line(eb, RD32(sp + 4), RD32(sp + 12), RD32(sp + 16), RD32(sp + 20)); },
                  f_00419ee0_t, 5, "line 0x419ee0");
}
extern "C" int native_blend_states(void) {
    return method([](uint32_t sp, uint32_t eb) { MEM_LOCAL(); blend_states(eb, RD32(sp + 4), RD32(sp + 8)); return R.eax; },
                  f_0041dde0_t, 2, "blend states 0x41dde0", false);
}
extern "C" int native_wrap_states(void) {
    return method([](uint32_t sp, uint32_t eb) { MEM_LOCAL(); wrap_states(eb, RD32(sp + 4)); return R.eax; },
                  f_0041e2b0_t, 1, "states 0x41e2b0", false);
}
extern "C" int native_state_1a(void) {
    return method([](uint32_t sp, uint32_t eb) { MEM_LOCAL(); state_1a(eb, RD32(sp + 4), RD32(sp + 8)); return R.eax; },
                  f_0041e380_t, 2, "state 0x41e380", false);
}
