// Maps game.EXE into guest memory at its preferred base and prepares the main thread.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "runtime.h"

namespace rt {

LoadedImage image;
std::string guest_command_line;

namespace {
constexpr uint32_t GOG_TIMESTAMP = 0x35A4A64F;   // 1998-07-09 11:15:27, GOG game.EXE
constexpr uint32_t GOG_SIZE = 1133056;

template <class T> T get(const std::vector<uint8_t> &b, size_t off) {
    T v;
    memcpy(&v, b.data() + off, sizeof v);
    return v;
}
}  // namespace

void load_image(const std::string &exe_path) {
    std::ifstream f(exe_path, std::ios::binary);
    if (!f) fatal("cannot open %s", exe_path.c_str());
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    uint32_t pe = get<uint32_t>(b, 0x3C);
    if (b.size() < 0x400 || get<uint32_t>(b, pe) != 0x4550) fatal("%s is not a PE file", exe_path.c_str());
    uint32_t ts = get<uint32_t>(b, pe + 8);
    if (b.size() != GOG_SIZE || ts != GOG_TIMESTAMP)
        fatal("%s is not the GOG game.EXE this build was generated from", exe_path.c_str());

    uint16_t nsec = get<uint16_t>(b, pe + 6);
    uint16_t opt_size = get<uint16_t>(b, pe + 20);
    uint32_t opt = pe + 24;
    uint32_t base = get<uint32_t>(b, opt + 28);
    if (base != IMAGE_BASE) fatal("unexpected image base 0x%x", base);
    image.entry = base + get<uint32_t>(b, opt + 16);
    image.image_size = get<uint32_t>(b, opt + 56);
    uint32_t hdr_size = get<uint32_t>(b, opt + 60);
    memcpy(M + base, b.data(), hdr_size);

    uint32_t sec = opt + opt_size;
    for (uint16_t i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = get<uint32_t>(b, sec + 8), va = get<uint32_t>(b, sec + 12);
        uint32_t raw_size = get<uint32_t>(b, sec + 16), raw_off = get<uint32_t>(b, sec + 20);
        uint32_t n = raw_size < vsize || vsize == 0 ? raw_size : vsize;
        memcpy(M + base + va, b.data() + raw_off, n);
    }

    // Point every IAT slot at a thunk that runs the host shim.
    const RecompModule &g = guest_code();
    for (uint32_t i = 0; i < g.import_count; i++) {
        const ImportEntry &e = g.imports[i];
        WR32(e.iat_va, register_thunk(e.fn, e.name));
    }

    image.exe_path = exe_path;
    size_t slash = exe_path.find_last_of('/');
    image.game_dir = slash == std::string::npos ? "." : exe_path.substr(0, slash);
    log("loaded %s: entry 0x%08x, %u imports\n", exe_path.c_str(), image.entry, g.import_count);
}

void setup_thread_environment(const std::string &command_line) {
    // TEB (only the fields MSVC CRT/MFC touch) and an empty PEB.
    WR32(TEB_VA + 0x00, 0xFFFFFFFF);   // SEH chain end
    WR32(TEB_VA + 0x04, STACK_HI);     // stack base
    WR32(TEB_VA + 0x08, STACK_LO);     // stack limit
    WR32(TEB_VA + 0x18, TEB_VA);       // self
    WR32(TEB_VA + 0x20, 1);            // process id
    WR32(TEB_VA + 0x24, 1);            // thread id
    WR32(TEB_VA + 0x30, PEB_VA);
    WR32(PEB_VA + 0x08, IMAGE_BASE);   // ImageBaseAddress
    R.fs_base = TEB_VA;

    R.esp = STACK_HI - 64;
    R.ebp = 0;
    R.df = 0;
    F.cw = 0x27F;
    guest_command_line = command_line;
}

static uint32_t last_error;
void set_last_error(uint32_t e) { last_error = e; WR32(TEB_VA + 0x34, e); }
uint32_t get_last_error() { return last_error; }

}  // namespace rt
