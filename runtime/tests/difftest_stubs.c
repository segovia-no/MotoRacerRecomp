/* Minimal host side for tools/recomp/tests/difftest.py: guest memory + trap stubs. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "recomp.h"

int tr_trap; /* set when generated code hits a trap helper */

void tr_init(void) {
    M = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (M == MAP_FAILED) { perror("mmap"); exit(1); }
}

void call_indirect(uint32_t va) { (void)va; tr_trap = 1; }
void x86_rdtsc(void) { tr_trap = 2; }
uint32_t x86_in(uint32_t port, int size) { (void)port; (void)size; tr_trap = 3; return 0; }
void x86_out(uint32_t port, uint32_t value, int size) { (void)port; (void)value; (void)size; tr_trap = 4; }
void x86_int(uint32_t va, int vector) { (void)va; (void)vector; tr_trap = 5; }
void x86_div_fault(uint32_t va) { (void)va; tr_trap = 6; }
void x86_unimpl(uint32_t va, const char *what) { (void)va; (void)what; tr_trap = 7; }

/* ---- pristine-copy diffing so tests don't copy whole regions each trial ---- */
static uint8_t *pristine;
static uint32_t p_base, p_len;

void tr_set_pristine(uint32_t base, uint32_t len, const uint8_t *data) {
    free(pristine);
    pristine = malloc(len);
    memcpy(pristine, data, len);
    memcpy(M + base, data, len);
    p_base = base;
    p_len = len;
}

void tr_poke(uint32_t a, const uint8_t *d, uint32_t n) {
    memcpy(M + a, d, n);
    if (a >= p_base && a + n <= p_base + p_len) memcpy(pristine + (a - p_base), d, n);
}

/* Record bytes that differ from the pristine copy, then restore them. */
uint32_t tr_collect(uint32_t *addrs, uint8_t *vals, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t off = 0; off < p_len; off += 4096) {
        uint32_t len = p_len - off < 4096 ? p_len - off : 4096;
        if (!memcmp(M + p_base + off, pristine + off, len)) continue;
        for (uint32_t i = off; i < off + len; i++) {
            if (M[p_base + i] != pristine[i]) {
                if (n < max) { addrs[n] = p_base + i; vals[n] = M[p_base + i]; }
                n++;
                M[p_base + i] = pristine[i];
            }
        }
    }
    return n;
}
