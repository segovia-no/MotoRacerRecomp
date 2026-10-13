// GDI32: the game draws through DirectDraw; GDI is only touched by MFC housekeeping.
// Device contexts and objects are opaque tokens with plausible answers.
#include "win32.h"

using namespace rt;

STDCALL(GDI32, GetStockObject, 1) { return 0x0A000000u | arg(0); }
STDCALL(GDI32, DeleteObject, 1) { return 1; }
STDCALL(GDI32, SelectObject, 2) { return 0x0A000000u; }
STDCALL(GDI32, DeleteDC, 1) { return 1; }
STDCALL(GDI32, SaveDC, 1) { return 1; }
STDCALL(GDI32, RestoreDC, 2) { return 1; }
STDCALL(GDI32, SetBkColor, 2) { return 0xFFFFFF; }
STDCALL(GDI32, SetTextColor, 2) { return 0; }
STDCALL(GDI32, SetMapMode, 2) { return 1; }
STDCALL(GDI32, SetViewportOrgEx, 4) { if (arg(3)) WR64(arg(3), 0); return 1; }
STDCALL(GDI32, OffsetViewportOrgEx, 4) { if (arg(3)) WR64(arg(3), 0); return 1; }
STDCALL(GDI32, SetViewportExtEx, 4) { if (arg(3)) WR64(arg(3), 0x0000000100000001ull); return 1; }
STDCALL(GDI32, SetWindowExtEx, 4) { if (arg(3)) WR64(arg(3), 0x0000000100000001ull); return 1; }
STDCALL(GDI32, ScaleViewportExtEx, 6) { if (arg(5)) WR64(arg(5), 0x0000000100000001ull); return 1; }
STDCALL(GDI32, ScaleWindowExtEx, 6) { if (arg(5)) WR64(arg(5), 0x0000000100000001ull); return 1; }
STDCALL(GDI32, GetClipBox, 2) {
    WR32(arg(1), 0); WR32(arg(1) + 4, 0); WR32(arg(1) + 8, 640); WR32(arg(1) + 12, 480);
    return 2;   // SIMPLEREGION
}
STDCALL(GDI32, PtVisible, 3) { return 1; }
STDCALL(GDI32, RectVisible, 2) { return 1; }
STDCALL(GDI32, TextOutA, 5) { return 1; }
STDCALL(GDI32, ExtTextOutA, 8) { return 1; }
STDCALL(GDI32, Escape, 5) { return 0; }
STDCALL(GDI32, CreateBitmap, 5) { return 0x0E100000u; }

STDCALL(GDI32, GetObjectA, 3) {
    memset(M + arg(2), 0, arg(1));
    return arg(1);
}

STDCALL(GDI32, GetDeviceCaps, 2) {
    switch (arg(1)) {
    case 8: return 1024;     // HORZRES
    case 10: return 768;     // VERTRES
    case 12: return 16;      // BITSPIXEL
    case 14: return 1;       // PLANES
    case 24: return 0x10000; // NUMCOLORS (hi-color)
    case 88: case 90: return 96;   // LOGPIXELSX/Y
    case 104: return 0;      // SIZEPALETTE
    case 38: return 0x7E99;  // RASTERCAPS
    default: return 0;
    }
}
