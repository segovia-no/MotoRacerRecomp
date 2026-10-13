// WINMM, COMCTL32, COMDLG32, WINSPOOL, DPLAYX and wsprintfA.
#include <SDL3/SDL.h>

#include <string>

#include "win32.h"

using namespace rt;

// ---------- wsprintfA (cdecl, varargs on the guest stack) ----------
namespace {

std::string format_guest(const std::string &fmt, uint32_t ap) {
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c != '%') { out += c; continue; }
        if (++i >= fmt.size()) break;
        if (fmt[i] == '%') { out += '%'; continue; }
        std::string spec = "%";
        while (i < fmt.size() && strchr("-+ #0", fmt[i])) spec += fmt[i++];
        while (i < fmt.size() && isdigit(static_cast<unsigned char>(fmt[i]))) spec += fmt[i++];
        if (i < fmt.size() && fmt[i] == '.') { spec += fmt[i++]; while (i < fmt.size() && isdigit(static_cast<unsigned char>(fmt[i]))) spec += fmt[i++]; }
        while (i < fmt.size() && strchr("lhI", fmt[i])) i++;   // size prefixes: all 32-bit here
        char conv = i < fmt.size() ? fmt[i] : 's';
        char buf[512];
        uint32_t v = RD32(ap);
        ap += 4;
        switch (conv) {
        case 'd': case 'i': snprintf(buf, sizeof buf, (spec + "d").c_str(), static_cast<int32_t>(v)); break;
        case 'u': snprintf(buf, sizeof buf, (spec + "u").c_str(), v); break;
        case 'x': snprintf(buf, sizeof buf, (spec + "x").c_str(), v); break;
        case 'X': snprintf(buf, sizeof buf, (spec + "X").c_str(), v); break;
        case 'c': snprintf(buf, sizeof buf, (spec + "c").c_str(), static_cast<int>(v & 0xFF)); break;
        case 's': case 'S': snprintf(buf, sizeof buf, (spec + "s").c_str(), v ? read_cstr(v).c_str() : "(null)"); break;
        default: snprintf(buf, sizeof buf, "%%%c", conv); break;
        }
        out += buf;
    }
    return out;
}

}  // namespace

CDECL(USER32, wsprintfA) {
    std::string s = format_guest(read_cstr(arg(1)), R.esp + 12);
    return write_cstr(arg(0), s, 1024);
}

// ---------- WINMM ----------
STDCALL(WINMM, timeGetTime, 0) { return static_cast<uint32_t>(SDL_GetTicks()); }

STDCALL(WINMM, joyGetNumDevs, 0) { return 16; }
STDCALL(WINMM, joyGetDevCapsA, 3) { return 167; }   // JOYERR_UNPLUGGED (no joystick yet)
STDCALL(WINMM, joyGetPosEx, 2) { return 167; }

STDCALL(WINMM, mixerGetNumDevs, 0) { return 0; }
STDCALL(WINMM, mixerOpen, 5) { return 2; }           // MMSYSERR_BADDEVICEID
STDCALL(WINMM, mixerClose, 1) { return 0; }
STDCALL(WINMM, mixerGetLineInfoA, 3) { return 2; }
STDCALL(WINMM, mixerGetLineControlsA, 3) { return 2; }
STDCALL(WINMM, mixerGetControlDetailsA, 3) { return 2; }
STDCALL(WINMM, mixerSetControlDetails, 3) { return 2; }

STDCALL(WINMM, mciSendCommandA, 4) {
    log("mciSendCommandA(dev %u, msg 0x%x, flags 0x%x) -> not supported yet\n", arg(0), arg(1), arg(2));
    return 263;   // MCIERR_INVALID_DEVICE_NAME
}
STDCALL(WINMM, mciGetErrorStringA, 3) { write_cstr(arg(1), "MCI error", arg(2)); return 1; }

// ---------- COMCTL32 / COMDLG32 / WINSPOOL ----------
STDCALL(COMCTL32, ord17, 0) { return 0; }   // InitCommonControls
STDCALL(COMCTL32, ImageList_Destroy, 1) { return 1; }
STDCALL(COMDLG32, GetOpenFileNameA, 1) { return 0; }
STDCALL(COMDLG32, GetSaveFileNameA, 1) { return 0; }
STDCALL(COMDLG32, GetFileTitleA, 3) { return 1; }
STDCALL(WINSPOOL, OpenPrinterA, 3) { return 0; }
STDCALL(WINSPOOL, DocumentPropertiesA, 6) { return 0xFFFFFFFF; }
STDCALL(WINSPOOL, ClosePrinter, 1) { return 1; }

// ---------- DPLAYX: multiplayer is stubbed (decision D3) ----------
STDCALL(DPLAYX, ord1, 3) { log("DirectPlayCreate -> unavailable\n"); return 0x80004005; }   // E_FAIL
STDCALL(DPLAYX, ord2, 2) { log("DirectPlayEnumerate -> none\n"); return 0; }
