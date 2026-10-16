// Guest address space and allocators.
//
// The whole 4 GB guest space is one host reservation (plat::VirtualMemory). The fixed low
// region and the TEB are committed at start; arena blocks are committed as they are handed out.
// Win32 heap APIs share one best-fit allocator over [HEAP_LO, HEAP_HI); VirtualAlloc hands out
// 64 KB-aligned regions from [VALLOC_LO, VALLOC_HI). Bookkeeping lives on the host so guest
// corruption can't break it.
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>

#include "platform/platform.h"
#include "runtime.h"

namespace rt {

namespace {

constexpr uint32_t ALIGN = 16;

void commit(uint32_t va, uint32_t size) {
    if (!plat::host().memory.commit(M + va, size)) fatal("cannot commit guest memory 0x%08x+0x%x", va, size);
}

struct Arena {
    std::map<uint32_t, uint32_t> free_by_addr;          // addr -> size
    std::multimap<uint32_t, uint32_t> free_by_size;     // size -> addr
    std::unordered_map<uint32_t, uint32_t> live;        // addr -> size (rounded)
    uint64_t in_use = 0, peak = 0;                      // bytes handed out (MOTORACER_MEM_STATS)

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

void memory_init() {
    auto &vm = plat::host().memory;
    M = vm.reserve(1ull << 32);
    if (!M) fatal("cannot reserve 4 GB guest address space");
    commit(NULL_GUARD, HEAP_LO - NULL_GUARD);   // stack, image, relocated engine tables
    commit(TEB_VA, 0x2000);                     // TEB + PEB
    commit(THUNK_LO, THUNK_HI - THUNK_LO);      // host thunk addresses stay readable, as before
    vm.make_inaccessible(M, NULL_GUARD);
    heap.init(HEAP_LO, HEAP_HI);
    varena.init(VALLOC_LO, VALLOC_HI);
}

uint32_t heap_alloc(uint32_t size, bool zero) {
    uint32_t a = heap.alloc(size, ALIGN);
    if (!a) fatal("guest heap exhausted (request %u)", size);
    if (zero) memset(M + a, 0, heap.live[a]);
    return a;
}

void heap_free(uint32_t va) {
    if (va && !heap.release(va)) log("heap_free: 0x%08x is not a live block\n", va);
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
        for (uint32_t p = a; p < a + s; p += 4096) {
            const uint64_t *q = reinterpret_cast<const uint64_t *>(M + p);
            for (int i = 0; i < 512; i++)
                if (q[i]) { touched++; break; }
        }
    log("memstats: heap %.1f MB (peak %.1f, %zu blocks), VirtualAlloc %.1f MB (peak %.1f, %.2f MB non-zero); "
        "largest:%s\n",
        heap.in_use / 1048576.0, heap.peak / 1048576.0, heap.live.size(), varena.in_use / 1048576.0,
        varena.peak / 1048576.0, touched * 4096 / 1048576.0, top.c_str());
}
}  // namespace rt
