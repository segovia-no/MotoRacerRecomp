// Dev aid (CMake MOTORACER_MALLOC_TRACE, PSP): live host heap blocks by size and call site, logged
// with MOTORACER_MEM_STATS. Wraps malloc / calloc / realloc / memalign / free (linker --wrap); the
// table is a fixed open-addressing array, so tracking itself never allocates. Call sites are return
// addresses (operator new shows up as itself; the size usually tells what it is). Resolve with
// psp-addr2line -e build/psp/motoracer <address>.
#if defined(__PSP__) && defined(RECOMP_MALLOC_TRACE)
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "runtime.h"

extern "C" {
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t);
void __real_free(void *);
}

namespace {
struct Entry { void *p; uint32_t size; void *ra; };
constexpr size_t kSlots = 1 << 15;
Entry table[kSlots];
volatile int lock_held = 0;

size_t slot(void *p) { return (reinterpret_cast<uintptr_t>(p) >> 3) * 2654435761u & (kSlots - 1); }

void track(void *p, size_t size, void *ra) {
    if (!p) return;
    for (size_t i = slot(p), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), n++)
        if (!table[i].p || table[i].p == p) { table[i] = Entry{p, static_cast<uint32_t>(size), ra}; return; }
}
void untrack(void *p) {
    if (!p) return;
    for (size_t i = slot(p), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), n++) {
        if (!table[i].p) return;
        if (table[i].p == p) {
            // backward-shift deletion keeps the probe chains intact
            size_t j = i;
            for (;;) {
                table[j].p = nullptr;
                size_t k = j;
                for (;;) {
                    k = (k + 1) & (kSlots - 1);
                    if (!table[k].p) return;
                    size_t h = slot(table[k].p);
                    if ((j <= k) ? (h <= j || h > k) : (h <= j && h > k)) break;
                }
                table[j] = table[k];
                j = k;
            }
        }
    }
}
}  // namespace

extern "C" {
void *__wrap_malloc(size_t n) {
    void *p = __real_malloc(n);
    track(p, n, __builtin_return_address(0));
    return p;
}
void *__wrap_calloc(size_t a, size_t b) {
    void *p = __real_calloc(a, b);
    track(p, a * b, __builtin_return_address(0));
    return p;
}
void *__wrap_realloc(void *old, size_t n) {
    void *p = __real_realloc(old, n);
    if (p) { untrack(old); track(p, n, __builtin_return_address(0)); }
    return p;
}
void *__wrap_memalign(size_t a, size_t n) {
    void *p = __real_memalign(a, n);
    track(p, n, __builtin_return_address(0));
    return p;
}
void __wrap_free(void *p) {
    untrack(p);
    __real_free(p);
}
}

namespace rt {
void malloc_trace_log() {
    struct Site { void *ra; uint64_t bytes; uint32_t blocks; };
    static Site sites[256];
    size_t nsites = 0;
    uint64_t total = 0;
    for (const Entry &e : table) {
        if (!e.p) continue;
        total += e.size;
        size_t i = 0;
        while (i < nsites && sites[i].ra != e.ra) i++;
        if (i == nsites) {
            if (nsites == 256) continue;
            sites[nsites++] = Site{e.ra, 0, 0};
        }
        sites[i].bytes += e.size;
        sites[i].blocks++;
    }
    std::sort(sites, sites + nsites, [](const Site &a, const Site &b) { return a.bytes > b.bytes; });
    char line[1024];
    int len = snprintf(line, sizeof line, "malloc trace: %.2f MB live; by call site (KB/blocks):", total / 1048576.0);
    for (size_t i = 0; i < nsites && i < 16 && len < static_cast<int>(sizeof line) - 40; i++)
        len += snprintf(line + len, sizeof line - len, " %p:%lluK/%u", sites[i].ra,
                        static_cast<unsigned long long>(sites[i].bytes / 1024), sites[i].blocks);
    log("%s\n", line);
}
}  // namespace rt
#endif
