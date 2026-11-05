// Guest address space and allocators.
//
// The whole 4 GB guest space is one host reservation (plat::VirtualMemory). The fixed low
// region and the TEB are committed at start; arena blocks are committed as they are handed out.
// Win32 heap APIs share one best-fit allocator over [HEAP_LO, HEAP_HI); VirtualAlloc hands out
// 64 KB-aligned regions from [VALLOC_LO, VALLOC_HI). Bookkeeping lives on the host so guest
// corruption can't break it.
#include <algorithm>
#include <cstdlib>
#ifdef __PSP__
#include <malloc.h>
#include <pspsysmem.h>
#include <unistd.h>
size_t psp_host_stack_used();
namespace rt { void malloc_trace_log(); }
#endif
#include <string>
#include <vector>
#include <map>
#include <unordered_map>

#include "platform/platform.h"
#include "runtime.h"

namespace rt {

namespace {

constexpr uint32_t ALIGN = 16;
constexpr uint32_t kBigBlock = 256u << 10;   // compact layout: allocated top-down (Arena::alloc)

// Arena blocks: heap blocks lie inside the block by construction; VirtualAlloc reservations may extend
// past what is backed in the compact layout (only commits must fit: check_guest_range in VirtualAlloc).
void commit(uint32_t va, uint32_t size) {
    if (!plat::host().memory.commit(M + va, size)) fatal("cannot commit guest memory 0x%08x+0x%x", va, size);
}

struct Arena {
    std::map<uint32_t, uint32_t> free_by_addr;          // addr -> size
    std::multimap<uint32_t, uint32_t> free_by_size;     // size -> addr
    std::unordered_map<uint32_t, uint32_t> live;        // addr -> size (rounded)
    uint64_t in_use = 0, peak = 0;                      // bytes handed out (MOTORACER_MEM_STATS)
    bool big_top_down = false;                          // guest heap only (not the VirtualAlloc arena)

    void init(uint32_t lo, uint32_t hi) { insert_free(lo, hi - lo); }

    void insert_free(uint32_t a, uint32_t s) {
        free_by_addr[a] = s;
        free_by_size.emplace(s, a);
    }
    void erase_free(std::map<uint32_t, uint32_t>::iterator it) {
        auto range = free_by_size.equal_range(it->second);
        for (auto j = range.first; j != range.second; ++j)
            if (j->second == it->first) { free_by_size.erase(j); break; }
        free_by_addr.erase(it);
    }

    uint32_t alloc(uint32_t size, uint32_t align) {
        uint32_t need = (size + ALIGN - 1) & ~(ALIGN - 1);
        if (need == 0) need = ALIGN;
#ifdef RECOMP_COMPACT_GUEST
        // Small heaps (PSP-1000, docs/25-psp-port.md): big blocks (640x480 surfaces, models) from the top
        // of the highest free range that fits, small ones best-fit from the bottom, so long-lived small
        // objects do not split the space the next big block needs (a race left no 600 KB gap for the
        // menus' surfaces otherwise).
        if (big_top_down && need >= kBigBlock) {
            for (auto it = free_by_addr.rbegin(); it != free_by_addr.rend(); ++it) {
                uint32_t a = it->first, s = it->second;
                if (s < need + align) continue;
                uint32_t start = (a + s - need) & ~(align - 1);
                if (start < a) continue;
                erase_free(free_by_addr.find(a));
                if (start > a) insert_free(a, start - a);
                if (start + need < a + s) insert_free(start + need, a + s - start - need);
                live[start] = need;
                in_use += need;
                peak = std::max(peak, in_use);
                commit(start, need);
                return start;
            }
            return 0;
        }
#endif
        for (auto it = free_by_size.lower_bound(need); it != free_by_size.end(); ++it) {
            uint32_t a = it->second, s = it->first;
            uint32_t start = (a + align - 1) & ~(align - 1);
            if (start - a + need > s) continue;
            erase_free(free_by_addr.find(a));
            if (start > a) insert_free(a, start - a);
            uint32_t end = start + need;
            if (a + s > end) insert_free(end, a + s - end);
            live[start] = need;
            in_use += need;
            peak = std::max(peak, in_use);
            commit(start, need);
            return start;
        }
        return 0;
    }

    bool release(uint32_t a) {
        auto it = live.find(a);
        if (it == live.end()) return false;
        uint32_t s = it->second;
        in_use -= s;
        live.erase(it);
        // coalesce with neighbours
        auto next = free_by_addr.lower_bound(a);
        if (next != free_by_addr.end() && next->first == a + s) {
            s += next->second;
            erase_free(next);
        }
        auto prev = free_by_addr.lower_bound(a);
        if (prev != free_by_addr.begin()) {
            --prev;
            if (prev->first + prev->second == a) {
                a = prev->first;
                s += prev->second;
                erase_free(prev);
            }
        }
        insert_free(a, s);
        return true;
    }
};

Arena heap, varena;

}  // namespace

void check_guest_range(uint32_t va, uint32_t size) {
    if (va < GUEST_LO || va + static_cast<uint64_t>(size) > GUEST_HI)
        fatal("guest memory 0x%08x+0x%x is outside the host block (compact layout: raise RECOMP_GUEST_HEAP_MB "
              "or VALLOC_BACKED)", va, size);
#ifdef RECOMP_COMPACT_GUEST
    if (va >= VALLOC_LO && va < VALLOC_HI && va + static_cast<uint64_t>(size) > VALLOC_LO + VALLOC_BACKED)
        fatal("VirtualAlloc commit 0x%08x+0x%x past the backed %u KB (raise VALLOC_BACKED)", va, size,
              VALLOC_BACKED / 1024);
#endif
}



#ifdef __PSP__
// Room left in the C heap's block (pspsdk reserves it at start, so kernel "free" says little): the
// largest sbrk that succeeds, probed in 64 KB steps and given back at once. (Probing with malloc does
// not work: newlib's malloc does not check the block's end.)
size_t psp_heap_room() {
    size_t lo = 0, hi = 64u << 20;
    while (hi - lo > (64u << 10)) {
        size_t mid = (lo + hi) / 2;
        void *p = sbrk(static_cast<ptrdiff_t>(mid));
        if (p == reinterpret_cast<void *>(-1)) { hi = mid; continue; }
        sbrk(-static_cast<ptrdiff_t>(mid));
        lo = mid;
    }
    return lo;
}
#endif

void memory_init() {
    auto &vm = plat::host().memory;
#ifdef __PSP__
    {
        struct mallinfo mi = mallinfo();
        log("memory: guest block %.2f MB; C heap in use %.2f MB, room left %.2f MB; kernel free %.2f MB\n",
            (GUEST_HI - GUEST_LO) / 1048576.0, mi.uordblks / 1048576.0, psp_heap_room() / 1048576.0,
            sceKernelTotalFreeMemSize() / 1048576.0);
    }
#endif
    uint8_t *block = vm.reserve(GUEST_HI - GUEST_LO);
    if (!block) fatal("cannot reserve the guest address space (%llu MB)", (unsigned long long)((GUEST_HI - GUEST_LO) >> 20));
    M = block - GUEST_LO;
    commit(NULL_GUARD, HEAP_LO - NULL_GUARD);   // stack, image, relocated engine tables
    commit(TEB_VA, 0x2000);                     // TEB + PEB
#ifndef RECOMP_COMPACT_GUEST
    commit(THUNK_LO, THUNK_HI - THUNK_LO);      // host thunk addresses stay readable, as before
    vm.make_inaccessible(M, NULL_GUARD);
#endif
    heap.big_top_down = true;
    heap.init(HEAP_LO, HEAP_HI);
    varena.init(VALLOC_LO, VALLOC_HI);
}

#ifdef RECOMP_COMPACT_GUEST
// PSP-1000 memory (docs/25-psp-port.md): the translated code never executes the x86 .text and reads
// only its jump / index tables and the self-modifying rasterizer (text_keep.inc). The rest becomes
// guest heap once the image is loaded. MOTORACER_TEXT_HEAP=0 turns it off.
namespace {
#include "text_keep.inc"
}
void donate_text_to_heap() {
    if (const char *e = getenv("MOTORACER_TEXT_HEAP"); e && e[0] == '0') return;
    uint32_t total = 0, pieces = 0, at = TEXT_LO;
    auto give = [&](uint32_t a, uint32_t b) {
        a = (a + 15) & ~15u;
        b &= ~15u;
        if (b <= a || b - a < 256) return;
        heap.insert_free(a, b - a);
        total += b - a;
        pieces++;
    };
    for (const auto &r : TEXT_KEEP) {
        give(at, r[0]);
        at = r[1];
    }
    give(at, TEXT_HI);
    // the unused layout gaps: after the PEB up to the stack, after the relocated tables (2 x 16 KB)
    give(PEB_VA + 0x1000, STACK_LO);
    give(EXT_LO + 0x10 + 2 * (4 * 4096 + 16) + 64, EXT_HI);
    log("memory: %u KB of the x86 .text and layout gaps (%u pieces) added to the guest heap\n", total >> 10, pieces);
}
#else
void donate_text_to_heap() {}
#endif

// MOTORACER_MEM_STATS: who owns the live heap blocks — the first return address on the guest stack
// inside the game's own code (below the CRT, which starts around 0x4b0000), i.e. past malloc.
std::unordered_map<uint32_t, uint32_t> block_owner;
bool track_owners() {   // MOTORACER_MEM_STATS=2: the per-block owner map costs host memory (PSP-1000)
    static const bool on = [] { const char *e = getenv("MOTORACER_MEM_STATS"); return e && atoi(e) >= 2; }();
    return on;
}
uint32_t guest_caller() {
    for (uint32_t sp = R.esp, n = 0; n < 64 && sp + 4 <= STACK_HI; sp += 4, n++) {
        const uint32_t v = RD32(sp);
        if (v >= 0x401000 && v < 0x4b0000) return v;
    }
    return 0;
}

uint32_t heap_alloc(uint32_t size, bool zero) {
    uint32_t a = heap.alloc(size, ALIGN);
    if (!a) {
        uint64_t free_total = 0, largest = 0;
        for (auto &[fa, fs] : heap.free_by_addr) { free_total += fs; largest = std::max<uint64_t>(largest, fs); }
        fatal("guest heap exhausted (request %u; in use %.2f MB in %zu blocks, peak %.2f MB, free %.2f MB, largest free %u KB)",
              size, heap.in_use / 1048576.0, heap.live.size(), heap.peak / 1048576.0, free_total / 1048576.0,
              static_cast<unsigned>(largest >> 10));
    }
    if (zero) memset(M + a, 0, heap.live[a]);
    if (track_owners()) block_owner[a] = guest_caller();
    return a;
}

void heap_free(uint32_t va) {
    if (va && !heap.release(va)) log("heap_free: 0x%08x is not a live block\n", va);
    else if (va && track_owners()) block_owner.erase(va);
}

uint32_t heap_size(uint32_t va) {
    auto it = heap.live.find(va);
    return it == heap.live.end() ? 0xFFFFFFFFu : it->second;
}

uint32_t heap_realloc(uint32_t va, uint32_t size, bool zero) {
    if (!va) return heap_alloc(size, zero);
    uint32_t old = heap_size(va);
    if (old == 0xFFFFFFFFu) fatal("heap_realloc of non-heap 0x%08x", va);
    if (size <= old) return va;
    uint32_t n = heap_alloc(size, zero);
    memcpy(M + n, M + va, old);
    heap_free(va);
    return n;
}

uint32_t valloc_reserve(uint32_t addr, uint32_t size) {
    size = (size + 0xFFFF) & ~0xFFFFu;
    if (addr) {
        // Explicit placement: only honoured inside a free part of the VirtualAlloc arena.
        auto it = varena.free_by_addr.upper_bound(addr);
        if (it == varena.free_by_addr.begin()) return 0;
        --it;
        if (it->first > addr || it->first + it->second < addr + size) return 0;
        uint32_t a = it->first, s = it->second;
        varena.erase_free(it);
        if (addr > a) varena.insert_free(a, addr - a);
        if (a + s > addr + size) varena.insert_free(addr + size, a + s - (addr + size));
        varena.live[addr] = size;
        commit(addr, size);
        return addr;
    }
    return varena.alloc(size, 0x10000);
}

void valloc_release(uint32_t addr) {
    auto it = varena.live.find(addr);
    if (it == varena.live.end()) return;
    memset(M + addr, 0, it->second);
    varena.release(addr);
}

std::string read_cstr(uint32_t va, uint32_t max) {
    if (!va) return {};
    const char *p = host_ptr(va);
    return std::string(p, strnlen(p, max));
}

std::u16string read_wstr(uint32_t va, uint32_t max) {
    std::u16string s;
    if (!va) return s;
    for (uint32_t i = 0; i < max; i++) {
        char16_t c = RD16(va + 2 * i);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

uint32_t write_cstr(uint32_t va, const std::string &s, uint32_t cap) {
    if (!cap) return 0;
    uint32_t n = static_cast<uint32_t>(s.size()) < cap - 1 ? static_cast<uint32_t>(s.size()) : cap - 1;
    memcpy(M + va, s.data(), n);
    M[va + n] = 0;
    return n;
}

uint32_t alloc_cstr(const std::string &s) {
    uint32_t a = heap_alloc(static_cast<uint32_t>(s.size()) + 1);
    memcpy(M + a, s.c_str(), s.size() + 1);
    return a;
}

}  // namespace rt

namespace rt {
// Dev aid (MOTORACER_MEM_STATS=1, logged from game::frame_tick): guest heap and VirtualAlloc use,
// with the largest live blocks (screen surfaces show up as the biggest ones).
void memory_stats_log() {
    std::vector<uint32_t> big;
    for (auto &[a, s] : heap.live) big.push_back(s);
    for (auto &[a, s] : varena.live) big.push_back(s);
    std::sort(big.rbegin(), big.rend());
    std::string top;
    for (size_t i = 0; i < big.size() && i < 10; i++) top += " " + std::to_string(big[i] / 1024) + "K";
    // VirtualAlloc regions are mostly reservations (the CRT's small-block heap reserves 4 MB and
    // commits as it grows): count the 4 KB pages holding anything but zeros.
    uint32_t touched = 0;
    for (auto &[a, s] : varena.live)
        for (uint32_t p = a; p < a + s && p + 4096 <= GUEST_HI; p += 4096) {   // only what is backed
            const uint64_t *q = reinterpret_cast<const uint64_t *>(M + p);
            for (int i = 0; i < 512; i++)
                if (q[i]) { touched++; break; }
        }
    log("memstats: heap %.1f MB (peak %.1f, %zu blocks), VirtualAlloc %.1f MB (peak %.1f, %.2f MB non-zero); "
        "largest:%s\n",
        heap.in_use / 1048576.0, heap.peak / 1048576.0, heap.live.size(), varena.in_use / 1048576.0,
        varena.peak / 1048576.0, touched * 4096 / 1048576.0, top.c_str());
    // guest stack high-water mark: the stack region is zero at start; lowest non-zero dword
    uint32_t low = STACK_HI;
    for (uint32_t a = STACK_LO; a < STACK_HI; a += 64)
        if (RD32(a) | RD32(a + 16) | RD32(a + 32) | RD32(a + 48)) { low = a; break; }
    log("memstats: guest stack used %u KB of %u KB\n", (STACK_HI - low) / 1024, (STACK_HI - STACK_LO) / 1024);
    // live heap bytes by owner (the 12 largest)
    std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> by_owner;
    for (auto &[a, sz] : heap.live) {
        auto it = block_owner.find(a);
        auto &e = by_owner[it == block_owner.end() ? 0 : it->second];
        e.first += sz;
        e.second++;
    }
    std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> owners;
    for (auto &[o, e] : by_owner) owners.push_back({e.first, {o, e.second}});
    std::sort(owners.rbegin(), owners.rend());
    std::string list;
    for (size_t i = 0; i < owners.size() && i < 12; i++) {
        char b[64];
        snprintf(b, sizeof b, " %x:%lluK/%u", owners[i].second.first, (unsigned long long)(owners[i].first / 1024),
                 owners[i].second.second);
        list += b;
    }
    log("memstats: heap by caller (KB/blocks):%s\n", list.c_str());
#ifdef __PSP__
    log("memstats: host stack used %zu KB of %llu KB\n", psp_host_stack_used() / 1024,
        static_cast<unsigned long long>(HOST_STACK_BYTES / 1024));
#endif
#ifdef __PSP__
    // host side: newlib malloc (runtime, SDL, converted textures, the guest block) and the kernel's
    // free user memory
    struct mallinfo mi = mallinfo();
    log("memstats: host malloc in use %.2f MB (arena %.2f MB), kernel free %.2f MB (largest block %.2f MB)\n",
        mi.uordblks / 1048576.0, mi.arena / 1048576.0, sceKernelTotalFreeMemSize() / 1048576.0,
        sceKernelMaxFreeMemSize() / 1048576.0);
#ifdef RECOMP_MALLOC_TRACE
    malloc_trace_log();
#endif
#endif
}
}  // namespace rt
