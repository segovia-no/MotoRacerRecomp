// One printf conversion (c s d i u o x X) with the C standard's flags / width / precision rules, as
// MSVC's CRT prints them: a small replacement for the host snprintf in the native _output
// (native_load.cpp). newlib's vfprintf is large and slow on the PSP (its code does not stay in the
// 16 KB instruction cache between the HUD's calls).
#pragma once
#include <cstdint>
#include <cstring>

namespace cfmt {

struct Spec {
    bool minus = false, plus = false, space = false, hash = false, zero = false;
    int width = -1, prec = -1;   // -1: none (a negative '*' width means '-' and its magnitude)
    bool half = false;           // h: short / unsigned short
    char conv = 0;
};

// Writes the conversion into out (room for cap bytes, no NUL); returns its length, or -1 for the
// combinations the C standard leaves undefined (or too long for cap): the caller then lets the
// translated CRT print it.
inline int convert(const Spec &s, uint32_t v, const char *str, size_t slen, char *out, size_t cap) {
    if (s.width > 480 || s.prec > 480) return -1;
    const int width = s.width < 0 ? 0 : s.width;
    size_t n = 0;
    auto pad = [&](int k, char ch) { while (k-- > 0) out[n++] = ch; };
    if (s.conv == 'c' || s.conv == 's') {
        if (s.zero || s.hash) return -1;
        const char ch = static_cast<char>(v & 0xff);
        const char *p = s.conv == 'c' ? &ch : str;
        size_t len = s.conv == 'c' ? 1 : slen;
        if (s.conv == 's' && s.prec >= 0 && static_cast<size_t>(s.prec) < len) len = static_cast<size_t>(s.prec);
        const int fill = width > static_cast<int>(len) ? width - static_cast<int>(len) : 0;
        if (len + static_cast<size_t>(fill) > cap) return -1;
        if (!s.minus) pad(fill, ' ');
        memcpy(out + n, p, len);
        n += len;
        if (s.minus) pad(fill, ' ');
        return static_cast<int>(n);
    }
    const bool sgn = s.conv == 'd' || s.conv == 'i';
    if (sgn && s.hash) return -1;
    uint32_t mag;
    bool neg = false;
    if (sgn) {
        const int32_t iv = s.half ? static_cast<int16_t>(v) : static_cast<int32_t>(v);
        neg = iv < 0;
        mag = neg ? 0u - static_cast<uint32_t>(iv) : static_cast<uint32_t>(iv);
    } else {
        mag = s.half ? static_cast<uint16_t>(v) : v;
    }
    char dig[16];
    int nd = 0;
    if (!(s.prec == 0 && mag == 0)) {
        const unsigned base = s.conv == 'o' ? 8u : (s.conv == 'x' || s.conv == 'X') ? 16u : 10u;
        const char *digits = s.conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        uint32_t m = mag;
        do { dig[nd++] = digits[m % base]; m /= base; } while (m);
    }
    int zeros = s.prec > nd ? s.prec - nd : 0;
    if (s.conv == 'o' && s.hash && zeros == 0 && (nd == 0 || dig[nd - 1] != '0')) zeros = 1;
    char prefix[2];
    int np = 0;
    if (sgn) {
        if (neg) prefix[np++] = '-';
        else if (s.plus) prefix[np++] = '+';
        else if (s.space) prefix[np++] = ' ';
    } else if ((s.conv == 'x' || s.conv == 'X') && s.hash && mag) {
        prefix[np++] = '0';
        prefix[np++] = s.conv;
    }
    int len = np + zeros + nd;
    int fill = width > len ? width - len : 0;
    if (s.zero && !s.minus && s.prec < 0) zeros += fill, len += fill, fill = 0;
    if (static_cast<size_t>(len + fill) > cap) return -1;
    if (!s.minus) pad(fill, ' ');
    for (int i = 0; i < np; i++) out[n++] = prefix[i];
    pad(zeros, '0');
    while (nd) out[n++] = dig[--nd];
    if (s.minus) pad(fill, ' ');
    return static_cast<int>(n);
}

}  // namespace cfmt
