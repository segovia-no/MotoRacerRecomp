// KERNEL32 shims. Behaviour follows Windows 98, which the game targets.
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>

#include "win32.h"

using namespace rt;
using namespace w32;

namespace {

// ---------- file objects ----------
struct File : Object {
    int fd = -1;
    std::string path;
    ~File() override { if (fd >= 0) close(fd); }
};

struct FindState : Object {
    std::vector<std::string> names;    // host names matching the pattern
    std::string host_dir;
    size_t next = 0;
};

constexpr uint64_t EPOCH_DIFF_100NS = 116444736000000000ull;   // 1601 -> 1970

uint64_t unix_to_filetime(time_t t) { return static_cast<uint64_t>(t) * 10000000ull + EPOCH_DIFF_100NS; }

void fill_find_data(uint32_t fd_va, const std::string &host, const std::string &name) {
    struct stat st{};
    stat(host.c_str(), &st);
    memset(M + fd_va, 0, 320);
    WR32(fd_va + 0, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    uint64_t ft = unix_to_filetime(st.st_mtime);
    for (int k = 0; k < 3; k++) { WR32(fd_va + 4 + 8 * k, (uint32_t)ft); WR32(fd_va + 8 + 8 * k, (uint32_t)(ft >> 32)); }
    WR32(fd_va + 28, 0);
    WR32(fd_va + 32, static_cast<uint32_t>(st.st_size));
    write_cstr(fd_va + 44, name, 260);
    write_cstr(fd_va + 304, name.size() <= 12 ? name : name.substr(0, 12), 14);
}

uint32_t find_next_into(FindState *fs, uint32_t fd_va) {
    if (fs->next >= fs->names.size()) { set_last_error(ERROR_NO_MORE_FILES); return 0; }
    const std::string &n = fs->names[fs->next++];
    fill_find_data(fd_va, fs->host_dir.empty() ? n : fs->host_dir + "/" + n, n);
    return 1;
}

// ---------- environment / command line ----------
uint32_t cmdline_va, env_a_va, env_w_va;

// ---------- TLS ----------
std::vector<uint32_t> tls(64);
uint32_t tls_next = 0;

// ---------- codepage 1252 helpers (the game only uses ASCII/Latin-1) ----------
uint16_t widen(uint8_t c) { return c; }
uint8_t narrow(uint16_t c) { return c < 256 ? static_cast<uint8_t>(c) : '?'; }

uint16_t ctype1(unsigned c) {
    uint16_t t = 0;
    if (isupper(c)) t |= 0x001;
    if (islower(c)) t |= 0x002;
    if (isdigit(c)) t |= 0x004;
    if (isspace(c)) t |= 0x008;
    if (ispunct(c)) t |= 0x010;
    if (iscntrl(c)) t |= 0x020;
    if (c == ' ' || c == '\t') t |= 0x040;
    if (isxdigit(c)) t |= 0x080;
    if (isalpha(c)) t |= 0x100;
    return t;
}

// ---------- INI files ----------
using Ini = std::map<std::string, std::map<std::string, std::string>>;

std::string lower(std::string s) { for (auto &c : s) c = static_cast<char>(tolower(c)); return s; }
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string ini_host_path(uint32_t file_va) {
    std::string f = read_cstr(file_va);
    if (f.find('\\') == std::string::npos && f.find('/') == std::string::npos) f = "C:\\WINDOWS\\" + f;
    std::string full = guest_full_path(f);
    if (lower(full).rfind("c:\\windows\\", 0) == 0) return "windows/" + lower(full.substr(11));
    return host_path(full);
}

Ini ini_load(const std::string &host) {
    Ini ini;
    std::ifstream in(host);
    std::string line, sec;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') { sec = lower(trim(line.substr(1, line.find(']') - 1))); continue; }
        size_t eq = line.find('=');
        if (eq != std::string::npos) ini[sec][lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }
    return ini;
}

}  // namespace

// =====================================================================================
// Process, version, environment
// =====================================================================================
STDCALL(KERNEL32, GetVersion, 0) { return 0xC0000A04; }   // Windows 98 (4.10), 9x flag

STDCALL(KERNEL32, GetVersionExA, 1) {
    uint32_t p = arg(0);
    WR32(p + 4, 4);              // major
    WR32(p + 8, 10);             // minor
    WR32(p + 12, 0x040A07CE);    // build 1998
    WR32(p + 16, 1);             // VER_PLATFORM_WIN32_WINDOWS
    write_cstr(p + 20, " A ", 128);
    return 1;
}

STDCALL(KERNEL32, GetProcessVersion, 1) { return 0x0004000A; }

STDCALL(KERNEL32, GetCommandLineA, 0) {
    if (!cmdline_va) cmdline_va = alloc_cstr(guest_command_line);
    return cmdline_va;
}

STDCALL(KERNEL32, GetStartupInfoA, 1) {
    uint32_t p = arg(0);
    memset(M + p, 0, 68);
    WR32(p, 68);
    return 0;
}

STDCALL(KERNEL32, GetEnvironmentStrings, 0) {
    if (!env_a_va) {
        static const char env[] = "PATH=C:\\WINDOWS\0WINDIR=C:\\WINDOWS\0\0";
        env_a_va = heap_alloc(sizeof env);
        memcpy(M + env_a_va, env, sizeof env);
    }
    return env_a_va;
}

STDCALL(KERNEL32, GetEnvironmentStringsW, 0) {
    if (!env_w_va) {
        static const char env[] = "PATH=C:\\WINDOWS\0WINDIR=C:\\WINDOWS\0\0";
        env_w_va = heap_alloc(2 * sizeof env);
        for (size_t i = 0; i < sizeof env; i++) WR16(env_w_va + 2 * static_cast<uint32_t>(i), static_cast<uint8_t>(env[i]));
    }
    return env_w_va;
}

STDCALL(KERNEL32, FreeEnvironmentStringsA, 1) { return 1; }
STDCALL(KERNEL32, FreeEnvironmentStringsW, 1) { return 1; }
STDCALL(KERNEL32, SetEnvironmentVariableA, 2) { return 1; }

STDCALL(KERNEL32, GetStdHandle, 1) { return INVALID_HANDLE; }   // GUI process: no console
STDCALL(KERNEL32, SetStdHandle, 2) { return 1; }
STDCALL(KERNEL32, SetHandleCount, 1) { return arg(0); }
STDCALL(KERNEL32, SetErrorMode, 1) { return 0; }
STDCALL(KERNEL32, SetConsoleCtrlHandler, 2) { return 1; }
STDCALL(KERNEL32, SetUnhandledExceptionFilter, 1) { return 0; }
STDCALL(KERNEL32, UnhandledExceptionFilter, 1) { return 0; }

STDCALL(KERNEL32, GetCurrentProcess, 0) { return 0xFFFFFFFF; }
STDCALL(KERNEL32, GetCurrentThread, 0) { return 0xFFFFFFFE; }
STDCALL(KERNEL32, GetCurrentThreadId, 0) { return 1; }
STDCALL(KERNEL32, SetThreadPriority, 2) { return 1; }

STDCALL(KERNEL32, ExitProcess, 1) { exit_process(arg(0)); }
STDCALL(KERNEL32, TerminateProcess, 2) { exit_process(arg(1)); }

STDCALL(KERNEL32, GetLastError, 0) { return get_last_error(); }
STDCALL(KERNEL32, SetLastError, 1) { set_last_error(arg(0)); return 0; }

STDCALL(KERNEL32, OutputDebugStringA, 1) {
    log("[guest debug] %s", read_cstr(arg(0)).c_str());
    return 0;
}

STDCALL(KERNEL32, Beep, 2) { return 1; }

STDCALL(KERNEL32, FormatMessageA, 7) {
    char msg[64];
    snprintf(msg, sizeof msg, "Error %u\r\n", arg(2));
    if (arg(0) & 0x100) {   // FORMAT_MESSAGE_ALLOCATE_BUFFER
        WR32(arg(4), alloc_cstr(msg));
        return static_cast<uint32_t>(strlen(msg));
    }
    return write_cstr(arg(4), msg, arg(5));
}


STDCALL(KERNEL32, IsBadReadPtr, 2) { return arg(0) < NULL_GUARD; }
STDCALL(KERNEL32, IsBadWritePtr, 2) { return arg(0) < NULL_GUARD; }
STDCALL(KERNEL32, IsBadCodePtr, 1) { return arg(0) < NULL_GUARD; }

STDCALL(KERNEL32, MulDiv, 3) {
    int64_t a = static_cast<int32_t>(arg(0)), b = static_cast<int32_t>(arg(1)), c = static_cast<int32_t>(arg(2));
    if (c == 0) return 0xFFFFFFFF;
    int64_t p = a * b;
    int64_t r = (p >= 0) == (c > 0) ? (p + c / 2) / c : (p - c / 2) / c;
    return static_cast<uint32_t>(r);
}

STDCALL(KERNEL32, InterlockedIncrement, 1) { uint32_t v = RD32(arg(0)) + 1; WR32(arg(0), v); return v; }
STDCALL(KERNEL32, InterlockedDecrement, 1) { uint32_t v = RD32(arg(0)) - 1; WR32(arg(0), v); return v; }

// Single guest thread: critical sections are no-ops.
STDCALL(KERNEL32, InitializeCriticalSection, 1) { memset(M + arg(0), 0, 24); return 0; }
STDCALL(KERNEL32, DeleteCriticalSection, 1) { return 0; }
STDCALL(KERNEL32, EnterCriticalSection, 1) { return 0; }
STDCALL(KERNEL32, LeaveCriticalSection, 1) { return 0; }

STDCALL(KERNEL32, TlsAlloc, 0) { return tls_next < tls.size() ? tls_next++ : 0xFFFFFFFF; }
STDCALL(KERNEL32, TlsSetValue, 2) {
    if (arg(0) >= tls.size()) return 0;
    tls[arg(0)] = arg(1);
    return 1;
}
STDCALL(KERNEL32, TlsGetValue, 1) {
    set_last_error(0);
    return arg(0) < tls.size() ? tls[arg(0)] : 0;
}

// =====================================================================================
// Memory
// =====================================================================================
constexpr uint32_t HEAP_HANDLE = 0x00A00000;

STDCALL(KERNEL32, HeapCreate, 3) { return HEAP_HANDLE; }
STDCALL(KERNEL32, HeapDestroy, 1) { return 1; }
STDCALL(KERNEL32, HeapAlloc, 3) { return heap_alloc(arg(2), arg(1) & 0x8); }
STDCALL(KERNEL32, HeapFree, 3) { heap_free(arg(2)); return 1; }
STDCALL(KERNEL32, HeapReAlloc, 4) {
    if (arg(1) & 0x10) {   // HEAP_REALLOC_IN_PLACE_ONLY
        uint32_t s = heap_size(arg(2));
        return s != 0xFFFFFFFF && arg(3) <= s ? arg(2) : 0;
    }
    return heap_realloc(arg(2), arg(3), arg(1) & 0x8);
}
STDCALL(KERNEL32, HeapSize, 3) { return heap_size(arg(2)); }

// Moveable memory handles are the block address itself.
STDCALL(KERNEL32, GlobalAlloc, 2) { return heap_alloc(arg(1), arg(0) & 0x40); }
STDCALL(KERNEL32, GlobalFree, 1) { heap_free(arg(0)); return 0; }
STDCALL(KERNEL32, GlobalReAlloc, 3) { return heap_realloc(arg(0), arg(1), arg(2) & 0x40); }
STDCALL(KERNEL32, GlobalLock, 1) { return arg(0); }
STDCALL(KERNEL32, GlobalUnlock, 1) { return 0; }
STDCALL(KERNEL32, GlobalHandle, 1) { return arg(0); }
STDCALL(KERNEL32, GlobalFlags, 1) { return 0; }
STDCALL(KERNEL32, GlobalDeleteAtom, 1) { return 0; }
STDCALL(KERNEL32, LocalAlloc, 2) { return heap_alloc(arg(1), arg(0) & 0x40); }
STDCALL(KERNEL32, LocalFree, 1) { heap_free(arg(0)); return 0; }
STDCALL(KERNEL32, LocalReAlloc, 3) { return heap_realloc(arg(0), arg(1), arg(2) & 0x40); }

STDCALL(KERNEL32, GlobalMemoryStatus, 1) {
    uint32_t p = arg(0);
    WR32(p + 0, 32);
    WR32(p + 4, 30);                 // load %
    WR32(p + 8, 128u << 20);         // total physical: a well-equipped 1998 PC
    WR32(p + 12, 96u << 20);
    WR32(p + 16, 256u << 20);        // page file
    WR32(p + 20, 200u << 20);
    WR32(p + 24, 0x7FFE0000);        // virtual
    WR32(p + 28, 0x7F000000);
    return 0;
}

STDCALL(KERNEL32, VirtualAlloc, 4) {
    uint32_t addr = arg(0), size = arg(1), type = arg(2);
    if ((type & 0x2000) || !addr) {   // MEM_RESERVE, or commit without a reservation
        uint32_t a = valloc_reserve(addr & ~0xFFFFu, size + (addr & 0xFFFF));
        if (!a) return 0;
        return addr ? addr : a;
    }
    return addr & ~0xFFFu;            // MEM_COMMIT inside an existing reservation
}

STDCALL(KERNEL32, VirtualFree, 3) {
    if (arg(2) & 0x8000) valloc_release(arg(0));
    else if (arg(2) & 0x4000) memset(M + (arg(0) & ~0xFFFu), 0, (arg(1) + 0xFFF) & ~0xFFFu);
    return 1;
}

STDCALL(KERNEL32, VirtualProtect, 4) {
    if (arg(3)) WR32(arg(3), 0x40);   // everything is RWX in the guest
    return 1;
}

// =====================================================================================
// Strings, code pages, locale
// =====================================================================================
STDCALL(KERNEL32, lstrlenA, 1) { return arg(0) ? static_cast<uint32_t>(read_cstr(arg(0)).size()) : 0; }
STDCALL(KERNEL32, lstrcpyA, 2) { std::string s = read_cstr(arg(1)); memcpy(M + arg(0), s.c_str(), s.size() + 1); return arg(0); }
STDCALL(KERNEL32, lstrcpynA, 3) { write_cstr(arg(0), read_cstr(arg(1)), arg(2)); return arg(0); }
STDCALL(KERNEL32, lstrcatA, 2) {
    std::string s = read_cstr(arg(0)) + read_cstr(arg(1));
    memcpy(M + arg(0), s.c_str(), s.size() + 1);
    return arg(0);
}
STDCALL(KERNEL32, lstrcmpA, 2) {
    int r = strcmp(read_cstr(arg(0)).c_str(), read_cstr(arg(1)).c_str());
    return static_cast<uint32_t>(r < 0 ? -1 : r > 0);
}
STDCALL(KERNEL32, lstrcmpiA, 2) {
    int r = strcasecmp(read_cstr(arg(0)).c_str(), read_cstr(arg(1)).c_str());
    return static_cast<uint32_t>(r < 0 ? -1 : r > 0);
}

STDCALL(KERNEL32, GetACP, 0) { return 1252; }
STDCALL(KERNEL32, GetOEMCP, 0) { return 437; }
STDCALL(KERNEL32, GetThreadLocale, 0) { return 0x409; }

STDCALL(KERNEL32, GetCPInfo, 2) {
    uint32_t p = arg(1);
    memset(M + p, 0, 20);
    WR32(p, 1);
    WR8(p + 4, '?');
    return 1;
}

STDCALL(KERNEL32, MultiByteToWideChar, 6) {
    uint32_t src = arg(2), dst = arg(4), dstlen = arg(5);
    int32_t len = static_cast<int32_t>(arg(3));
    uint32_t n = len < 0 ? static_cast<uint32_t>(read_cstr(src).size()) + 1 : static_cast<uint32_t>(len);
    if (!dstlen) return n;
    if (dstlen < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (uint32_t i = 0; i < n; i++) WR16(dst + 2 * i, widen(RD8(src + i)));
    return n;
}

STDCALL(KERNEL32, WideCharToMultiByte, 8) {
    uint32_t src = arg(2), dst = arg(4), dstlen = arg(5);
    int32_t len = static_cast<int32_t>(arg(3));
    uint32_t n = len < 0 ? static_cast<uint32_t>(read_wstr(src).size()) + 1 : static_cast<uint32_t>(len);
    if (arg(7)) WR32(arg(7), 0);
    if (!dstlen) return n;
    if (dstlen < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (uint32_t i = 0; i < n; i++) WR8(dst + i, narrow(RD16(src + 2 * i)));
    return n;
}

STDCALL(KERNEL32, LCMapStringA, 6) {
    uint32_t flags = arg(1), src = arg(2), dst = arg(4), dstlen = arg(5);
    int32_t len = static_cast<int32_t>(arg(3));
    uint32_t n = len < 0 ? static_cast<uint32_t>(read_cstr(src).size()) + 1 : static_cast<uint32_t>(len);
    if (!dstlen) return n;
    if (dstlen < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        int c = RD8(src + i);
        WR8(dst + i, static_cast<uint8_t>((flags & 0x200) ? toupper(c) : (flags & 0x100) ? tolower(c) : c));
    }
    return n;
}

STDCALL(KERNEL32, LCMapStringW, 6) {
    uint32_t flags = arg(1), src = arg(2), dst = arg(4), dstlen = arg(5);
    int32_t len = static_cast<int32_t>(arg(3));
    uint32_t n = len < 0 ? static_cast<uint32_t>(read_wstr(src).size()) + 1 : static_cast<uint32_t>(len);
    if (!dstlen) return n;
    if (dstlen < n) { set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = RD16(src + 2 * i);
        if (c < 128) c = static_cast<uint16_t>((flags & 0x200) ? toupper(c) : (flags & 0x100) ? tolower(c) : c);
        WR16(dst + 2 * i, c);
    }
    return n;
}

static uint32_t string_type(uint32_t info, uint32_t src, int32_t len, uint32_t out, bool wide) {
    uint32_t n = len < 0 ? static_cast<uint32_t>(wide ? read_wstr(src).size() : read_cstr(src).size()) + 1
                         : static_cast<uint32_t>(len);
    for (uint32_t i = 0; i < n; i++) {
        unsigned c = wide ? RD16(src + 2 * i) : RD8(src + i);
        WR16(out + 2 * i, info == 1 && c < 128 ? ctype1(c) : 0);
    }
    return 1;
}
STDCALL(KERNEL32, GetStringTypeA, 5) { return string_type(arg(1), arg(2), static_cast<int32_t>(arg(3)), arg(4), false); }
STDCALL(KERNEL32, GetStringTypeExA, 5) { return string_type(arg(1), arg(2), static_cast<int32_t>(arg(3)), arg(4), false); }
STDCALL(KERNEL32, GetStringTypeW, 4) { return string_type(arg(0), arg(1), static_cast<int32_t>(arg(2)), arg(3), true); }

static uint32_t compare_result(int r) { return r < 0 ? 1 : r > 0 ? 3 : 2; }
STDCALL(KERNEL32, CompareStringA, 6) {
    auto get = [](uint32_t p, int32_t n) { return n < 0 ? read_cstr(p) : std::string(host_ptr(p), static_cast<size_t>(n)); };
    std::string a = get(arg(2), static_cast<int32_t>(arg(3))), b = get(arg(4), static_cast<int32_t>(arg(5)));
    return compare_result((arg(1) & 1) ? strcasecmp(a.c_str(), b.c_str()) : strcmp(a.c_str(), b.c_str()));
}
STDCALL(KERNEL32, CompareStringW, 6) {
    auto get = [](uint32_t p, int32_t n) {
        std::u16string s = n < 0 ? read_wstr(p) : std::u16string(reinterpret_cast<char16_t *>(host_ptr(p)), static_cast<size_t>(n));
        std::string o;
        for (char16_t c : s) o.push_back(static_cast<char>(narrow(c)));
        return o;
    };
    std::string a = get(arg(2), static_cast<int32_t>(arg(3))), b = get(arg(4), static_cast<int32_t>(arg(5)));
    return compare_result((arg(1) & 1) ? strcasecmp(a.c_str(), b.c_str()) : strcmp(a.c_str(), b.c_str()));
}

static std::string locale_info(uint32_t type) {
    switch (type & 0xFFFF) {
    case 0x0001: return "0409";    // LOCALE_ILANGUAGE
    case 0x0003: return "ENU";     // LOCALE_SABBREVLANGNAME
    case 0x0007: return "USA";
    case 0x000B: return "437";     // LOCALE_IDEFAULTCODEPAGE
    case 0x000E: return ".";       // LOCALE_SDECIMAL
    case 0x000F: return ",";       // LOCALE_STHOUSAND
    case 0x1004: return "1252";    // LOCALE_IDEFAULTANSICODEPAGE
    default: return "";
    }
}
STDCALL(KERNEL32, GetLocaleInfoA, 4) {
    std::string s = locale_info(arg(1));
    if (s.empty()) return 0;
    if (!arg(3)) return static_cast<uint32_t>(s.size()) + 1;
    write_cstr(arg(2), s, arg(3));
    return static_cast<uint32_t>(s.size()) + 1;
}
STDCALL(KERNEL32, GetLocaleInfoW, 4) {
    std::string s = locale_info(arg(1));
    if (s.empty()) return 0;
    if (!arg(3)) return static_cast<uint32_t>(s.size()) + 1;
    for (size_t i = 0; i <= s.size() && i < arg(3); i++) WR16(arg(2) + 2 * static_cast<uint32_t>(i), i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
    return static_cast<uint32_t>(s.size()) + 1;
}

STDCALL(KERNEL32, GetTimeZoneInformation, 1) { memset(M + arg(0), 0, 172); return 0; }

// =====================================================================================
// Modules and resources
// =====================================================================================
STDCALL(KERNEL32, GetModuleHandleA, 1) {
    if (!arg(0)) return HMODULE_GAME;
    uint32_t h = module_handle_for(read_cstr(arg(0)));
    if (!h) set_last_error(ERROR_MOD_NOT_FOUND);
    return h;
}

STDCALL(KERNEL32, GetModuleFileNameA, 3) {
    std::string name = std::string(GUEST_ROOT) + "\\GAME.EXE";
    return write_cstr(arg(1), name, arg(2));
}

STDCALL(KERNEL32, LoadLibraryA, 1) {
    std::string n = read_cstr(arg(0));
    uint32_t h = module_handle_for(n);
    log("LoadLibraryA(\"%s\") -> 0x%08x\n", n.c_str(), h);
    if (!h) set_last_error(ERROR_MOD_NOT_FOUND);
    return h;
}

STDCALL(KERNEL32, FreeLibrary, 1) { return 1; }

STDCALL(KERNEL32, GetProcAddress, 2) {
    std::string name = arg(1) < 0x10000 ? "#" + std::to_string(arg(1)) : read_cstr(arg(1));
    uint32_t a = proc_address(arg(0), name);
    log("GetProcAddress(0x%08x, \"%s\") -> 0x%08x\n", arg(0), name.c_str(), a);
    if (!a) set_last_error(ERROR_PROC_NOT_FOUND);
    return a;
}

STDCALL(KERNEL32, FindResourceA, 3) { return find_resource(arg(2), arg(1)); }
STDCALL(KERNEL32, LoadResource, 2) { return arg(1) ? resource_data(arg(1)) : 0; }
STDCALL(KERNEL32, LockResource, 1) { return arg(0); }

// =====================================================================================
// Files
// =====================================================================================
STDCALL(KERNEL32, CreateFileA, 7) {
    std::string gp = read_cstr(arg(0));
    std::string hp = host_path(gp);
    uint32_t access = arg(1), disp = arg(4);
    int flags = (access & 0x40000000) ? ((access & 0x80000000) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;    // CREATE_NEW
    case 2: flags |= O_CREAT | O_TRUNC; break;   // CREATE_ALWAYS
    case 4: flags |= O_CREAT; break;             // OPEN_ALWAYS
    case 5: flags |= O_TRUNC; break;             // TRUNCATE_EXISTING
    default: break;                              // OPEN_EXISTING
    }
    int fd = open(hp.c_str(), flags, 0644);
    if (trace_imports || fd < 0) log("CreateFileA(\"%s\" -> %s) = %s\n", gp.c_str(), hp.c_str(), fd < 0 ? "FAIL" : "ok");
    if (fd < 0) {
        set_last_error(errno == EEXIST ? ERROR_FILE_EXISTS : errno == EACCES ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE;
    }
    auto f = std::make_unique<File>();
    f->fd = fd;
    f->path = hp;
    set_last_error(0);
    return handle_add(std::move(f));
}

STDCALL(KERNEL32, ReadFile, 5) {
    File *f = handle_as<File>(arg(0));
    if (!f) { set_last_error(ERROR_INVALID_HANDLE); return 0; }
    ssize_t n = read(f->fd, M + arg(1), arg(2));
    if (arg(3)) WR32(arg(3), n < 0 ? 0 : static_cast<uint32_t>(n));
    return n >= 0;
}

STDCALL(KERNEL32, WriteFile, 5) {
    File *f = handle_as<File>(arg(0));
    if (!f) { set_last_error(ERROR_INVALID_HANDLE); return 0; }
    ssize_t n = write(f->fd, M + arg(1), arg(2));
    if (arg(3)) WR32(arg(3), n < 0 ? 0 : static_cast<uint32_t>(n));
    return n >= 0;
}

STDCALL(KERNEL32, SetFilePointer, 4) {
    File *f = handle_as<File>(arg(0));
    if (!f) { set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFF; }
    int64_t off = static_cast<int32_t>(arg(1));
    if (arg(2)) off = static_cast<int64_t>((static_cast<uint64_t>(RD32(arg(2))) << 32) | arg(1));
    off_t r = lseek(f->fd, off, arg(3) == 0 ? SEEK_SET : arg(3) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) return 0xFFFFFFFF;
    if (arg(2)) WR32(arg(2), static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32));
    return static_cast<uint32_t>(r);
}

STDCALL(KERNEL32, GetFileSize, 2) {
    File *f = handle_as<File>(arg(0));
    if (!f) return 0xFFFFFFFF;
    struct stat st;
    fstat(f->fd, &st);
    if (arg(1)) WR32(arg(1), 0);
    return static_cast<uint32_t>(st.st_size);
}

STDCALL(KERNEL32, GetFileType, 1) { return handle_as<File>(arg(0)) ? 1 : 0; }

STDCALL(KERNEL32, GetFileTime, 4) {
    File *f = handle_as<File>(arg(0));
    if (!f) return 0;
    struct stat st;
    fstat(f->fd, &st);
    uint64_t ft = unix_to_filetime(st.st_mtime);
    for (int k = 1; k <= 3; k++)
        if (arg(k)) { WR32(arg(k), static_cast<uint32_t>(ft)); WR32(arg(k) + 4, static_cast<uint32_t>(ft >> 32)); }
    return 1;
}

STDCALL(KERNEL32, FileTimeToLocalFileTime, 2) { WR64(arg(1), RD64(arg(0))); return 1; }

STDCALL(KERNEL32, FileTimeToSystemTime, 2) {
    time_t t = static_cast<time_t>((RD64(arg(0)) - EPOCH_DIFF_100NS) / 10000000ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    uint32_t p = arg(1);
    WR16(p + 0, static_cast<uint16_t>(tm.tm_year + 1900));
    WR16(p + 2, static_cast<uint16_t>(tm.tm_mon + 1));
    WR16(p + 4, static_cast<uint16_t>(tm.tm_wday));
    WR16(p + 6, static_cast<uint16_t>(tm.tm_mday));
    WR16(p + 8, static_cast<uint16_t>(tm.tm_hour));
    WR16(p + 10, static_cast<uint16_t>(tm.tm_min));
    WR16(p + 12, static_cast<uint16_t>(tm.tm_sec));
    WR16(p + 14, 0);
    return 1;
}

STDCALL(KERNEL32, CloseHandle, 1) { return handle_close(arg(0)) ? 1 : 0; }
STDCALL(KERNEL32, FlushFileBuffers, 1) { return 1; }
STDCALL(KERNEL32, LockFile, 5) { return 1; }
STDCALL(KERNEL32, UnlockFile, 5) { return 1; }

STDCALL(KERNEL32, SetEndOfFile, 1) {
    File *f = handle_as<File>(arg(0));
    if (!f) return 0;
    return ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0;
}

STDCALL(KERNEL32, DuplicateHandle, 7) {
    if (arg(3)) WR32(arg(3), arg(1));
    return 1;
}

STDCALL(KERNEL32, DeleteFileA, 1) { return unlink(host_path(read_cstr(arg(0))).c_str()) == 0; }

STDCALL(KERNEL32, GetFileAttributesA, 1) {
    struct stat st;
    std::string hp = host_path(read_cstr(arg(0)));
    if (stat(hp.c_str(), &st) != 0) { set_last_error(ERROR_FILE_NOT_FOUND); return 0xFFFFFFFF; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
STDCALL(KERNEL32, SetFileAttributesA, 2) { return 1; }

STDCALL(KERNEL32, GetFullPathNameA, 4) {
    std::string full = guest_full_path(read_cstr(arg(0)));
    if (arg(1) <= full.size()) return static_cast<uint32_t>(full.size()) + 1;
    write_cstr(arg(2), full, arg(1));
    if (arg(3)) {
        size_t slash = full.find_last_of('\\');
        WR32(arg(3), arg(2) + static_cast<uint32_t>(slash + 1));
    }
    return static_cast<uint32_t>(full.size());
}

STDCALL(KERNEL32, GetCurrentDirectoryA, 2) {
    if (arg(0) <= guest_cwd.size()) return static_cast<uint32_t>(guest_cwd.size()) + 1;
    return write_cstr(arg(1), guest_cwd, arg(0));
}

STDCALL(KERNEL32, SetCurrentDirectoryA, 1) {
    guest_cwd = guest_full_path(read_cstr(arg(0)));
    return 1;
}

STDCALL(KERNEL32, FindFirstFileA, 2) {
    std::string pat = read_cstr(arg(0));
    std::string full = guest_full_path(pat);
    size_t slash = full.find_last_of('\\');
    std::string dir = full.substr(0, slash), mask = full.substr(slash + 1);
    auto fs = std::make_unique<FindState>();
    fs->host_dir = host_path(dir);
    if (DIR *d = opendir(fs->host_dir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            if (fnmatch(mask == "*.*" ? "*" : mask.c_str(), n.c_str(), FNM_CASEFOLD) == 0) fs->names.push_back(n);
        }
        closedir(d);
    }
    if (fs->names.empty()) { set_last_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; }
    find_next_into(fs.get(), arg(1));
    return handle_add(std::move(fs));
}

STDCALL(KERNEL32, FindNextFileA, 2) {
    FindState *fs = handle_as<FindState>(arg(0));
    return fs ? find_next_into(fs, arg(1)) : 0;
}

STDCALL(KERNEL32, FindClose, 1) { return handle_close(arg(0)) ? 1 : 0; }

// =====================================================================================
// Drives. C: is the install, D: the (virtual) Moto Racer CD; both map to the game dir.
// =====================================================================================
STDCALL(KERNEL32, GetLogicalDrives, 0) { return 0xC; }

STDCALL(KERNEL32, GetDriveTypeA, 1) {
    std::string r = read_cstr(arg(0));
    char d = r.empty() ? 'C' : static_cast<char>(toupper(r[0]));
    return d == 'C' ? 3 : d == 'D' ? 5 : 1;   // FIXED, CDROM, NO_ROOT_DIR
}

STDCALL(KERNEL32, GetVolumeInformationA, 8) {
    std::string r = read_cstr(arg(0));
    log("GetVolumeInformationA(\"%s\")\n", r.c_str());
    if (arg(1)) write_cstr(arg(1), toupper(r.empty() ? 'C' : r[0]) == 'D' ? "MOTORACER" : "SYSTEM", arg(2));
    if (arg(3)) WR32(arg(3), 0x19980709);
    if (arg(4)) WR32(arg(4), 255);
    if (arg(5)) WR32(arg(5), 0);
    if (arg(6)) write_cstr(arg(6), toupper(r.empty() ? 'C' : r[0]) == 'D' ? "CDFS" : "FAT32", arg(7));
    return 1;
}

// =====================================================================================
// Private profile (INI) files
// =====================================================================================
STDCALL(KERNEL32, GetPrivateProfileStringA, 6) {
    Ini ini = ini_load(ini_host_path(arg(5)));
    std::string def = read_cstr(arg(2));
    std::string sec = lower(read_cstr(arg(0))), key = lower(read_cstr(arg(1)));
    std::string v = def;
    if (arg(0) && arg(1)) {
        auto s = ini.find(sec);
        if (s != ini.end()) { auto k = s->second.find(key); if (k != s->second.end()) v = k->second; }
    }
    return write_cstr(arg(3), v, arg(4));
}

STDCALL(KERNEL32, GetPrivateProfileIntA, 4) {
    Ini ini = ini_load(ini_host_path(arg(3)));
    auto s = ini.find(lower(read_cstr(arg(0))));
    if (s != ini.end()) {
        auto k = s->second.find(lower(read_cstr(arg(1))));
        if (k != s->second.end()) return static_cast<uint32_t>(atoi(k->second.c_str()));
    }
    return arg(2);
}

STDCALL(KERNEL32, WritePrivateProfileStringA, 4) {
    std::string path = ini_host_path(arg(3));
    Ini ini = ini_load(path);
    std::string sec = lower(read_cstr(arg(0)));
    if (!arg(1)) ini.erase(sec);
    else if (!arg(2)) ini[sec].erase(lower(read_cstr(arg(1))));
    else ini[sec][lower(read_cstr(arg(1)))] = read_cstr(arg(2));
    mkdir("windows", 0755);
    std::ofstream out(path);
    for (auto &[s, kv] : ini) {
        out << "[" << s << "]\r\n";
        for (auto &[k, v] : kv) out << k << "=" << v << "\r\n";
    }
    return 1;
}
