// Windows adapters: VirtualAlloc guest space, a fiber for the large stack, UTF-16 file APIs,
// unhandled-exception crash reports, %LOCALAPPDATA% folders.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

#include "platform.h"

namespace plat {

namespace {

std::wstring wide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

int64_t unix_time(const FILETIME &ft) {
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>((t - 116444736000000000ull) / 10000000ull);
}

// ---------- virtual memory ----------
// The reservation is committed explicitly where the runtime knows it is used (memory.cpp), and
// lazily, page by page, on first access anywhere else in it: that matches POSIX overcommit, so a
// guest read just past an allocated block behaves the same on every host. Guard ranges stay
// inaccessible and fault as usual.
uint8_t *res_base = nullptr;
uint64_t res_size = 0;
struct Range { uintptr_t lo, hi; };
Range guards[4];
int guard_count = 0;

LONG CALLBACK lazy_commit(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = static_cast<uintptr_t>(r->ExceptionInformation[1]);
    uintptr_t base = reinterpret_cast<uintptr_t>(res_base);
    if (!res_base || a < base || a >= base + res_size) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < guard_count; i++)
        if (a >= guards[i].lo && a < guards[i].hi) return EXCEPTION_CONTINUE_SEARCH;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
        return EXCEPTION_CONTINUE_SEARCH;   // committed already: a real protection fault
    void *page = reinterpret_cast<void *>(a & ~static_cast<uintptr_t>(0xFFF));
    return VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION
                                                                   : EXCEPTION_CONTINUE_SEARCH;
}

class WinVirtualMemory final : public VirtualMemory {
public:
    uint8_t *reserve(uint64_t size) override {
        void *p = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
        if (!p) return nullptr;
        res_base = static_cast<uint8_t *>(p);
        res_size = size;
        AddVectoredExceptionHandler(1, lazy_commit);
        return res_base;
    }
    bool commit(void *addr, uint64_t size) override {
        return size == 0 || VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }
    bool make_inaccessible(void *addr, uint64_t size) override {
        if (guard_count == 4) return false;
        uintptr_t lo = reinterpret_cast<uintptr_t>(addr);
        guards[guard_count++] = {lo, lo + size};
        VirtualFree(addr, size, MEM_DECOMMIT);   // fine if it was never committed
        return true;
    }
};

// ---------- execution ----------
// A fiber gives the recompiled code its deep stack while staying on the main thread (SDL's
// window and event handling belong to the thread that created them).
struct FiberCall {
    void (*fn)();
    void *main_fiber;
};

void CALLBACK fiber_main(void *param) {
    auto *call = static_cast<FiberCall *>(param);
    call->fn();
    SwitchToFiber(call->main_fiber);   // a fiber routine must not return
}

class WinExecution final : public Execution {
public:
    bool run_on_large_stack(uint64_t bytes, void (*fn)()) override {
        bool converted = false;
        void *self = GetCurrentFiber();
        if (!self || self == reinterpret_cast<void *>(0x1E00)) {   // not a fiber yet
            self = ConvertThreadToFiber(nullptr);
            if (!self) return false;
            converted = true;
        }
        FiberCall call{fn, self};
        void *fiber = CreateFiberEx(0, static_cast<SIZE_T>(bytes), FIBER_FLAG_FLOAT_SWITCH, fiber_main, &call);
        if (!fiber) return false;
        SwitchToFiber(fiber);
        DeleteFiber(fiber);
        if (converted) ConvertFiberToThread();
        return true;
    }
};

// ---------- crash reporting ----------
void (*fault_handler)(const Fault &) = nullptr;

LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    Fault f{};
    f.code = static_cast<int>(r->ExceptionCode);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        f.kind = Fault::MemoryAccess;
        f.address = static_cast<uintptr_t>(r->ExceptionInformation[1]);
    } else {
        f.kind = Fault::Other;
    }
    if (fault_handler) fault_handler(f);
    TerminateProcess(GetCurrentProcess(), 2);
    return EXCEPTION_EXECUTE_HANDLER;
}

void on_abort(int) {
    Fault f{};
    f.kind = Fault::Abort;
    f.code = SIGABRT;
    if (fault_handler) fault_handler(f);
    TerminateProcess(GetCurrentProcess(), 2);
}

class WinCrashReporter final : public CrashReporter {
public:
    void install(void (*on_fault)(const Fault &)) override {
        fault_handler = on_fault;
        ULONG reserve = 64 * 1024;   // room for the report after a stack overflow
        SetThreadStackGuarantee(&reserve);
        SetUnhandledExceptionFilter(on_unhandled);
        signal(SIGABRT, on_abort);
    }
    void write_backtrace(FILE *out) override {
        // module+offset per frame; symbolise with llvm-addr2line / the build's map if needed
        void *frames[48];
        USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; i++) {
            HMODULE mod = nullptr;
            wchar_t name[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(frames[i]), &mod))
                GetModuleFileNameW(mod, name, MAX_PATH);
            const wchar_t *base = wcsrchr(name, L'\\');
            fprintf(out, "  %2u %s+0x%llx\n", i, utf8(base ? base + 1 : name).c_str(),
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod)));
        }
        fflush(out);
    }
};

// ---------- files ----------
FileInfo info_from(DWORD attrs, const FILETIME &mtime, uint64_t size) {
    FileInfo i;
    i.exists = true;
    i.is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    i.size = size;
    i.mtime = unix_time(mtime);
    return i;
}

class WinFile final : public File {
public:
    explicit WinFile(HANDLE h) : h_(h) {}
    ~WinFile() override { CloseHandle(h_); }
    int64_t read(void *dst, uint32_t n) override {
        DWORD got = 0;
        return ReadFile(h_, dst, n, &got, nullptr) ? static_cast<int64_t>(got) : -1;
    }
    int64_t write(const void *src, uint32_t n) override {
        DWORD put = 0;
        return WriteFile(h_, src, n, &put, nullptr) ? static_cast<int64_t>(put) : -1;
    }
    int64_t seek(int64_t offset, Seek from) override {
        LARGE_INTEGER d, pos;
        d.QuadPart = offset;
        DWORD how = from == Seek::Begin ? FILE_BEGIN : from == Seek::Current ? FILE_CURRENT : FILE_END;
        return SetFilePointerEx(h_, d, &pos, how) ? pos.QuadPart : -1;
    }
    bool truncate_at_position() override { return SetEndOfFile(h_) != 0; }
    FileInfo info() override {
        BY_HANDLE_FILE_INFORMATION fi;
        if (!GetFileInformationByHandle(h_, &fi)) return {};
        return info_from(fi.dwFileAttributes, fi.ftLastWriteTime,
                         (static_cast<uint64_t>(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow);
    }

private:
    HANDLE h_;
};

class WinFileSystem final : public FileSystem {
public:
    std::unique_ptr<File> open(const std::string &path, bool rd, bool wr, Disposition disp, FileError *err) override {
        DWORD access = (rd ? GENERIC_READ : 0) | (wr ? GENERIC_WRITE : 0);
        DWORD how = OPEN_EXISTING;
        switch (disp) {
        case Disposition::CreateNew: how = CREATE_NEW; break;
        case Disposition::CreateAlways: how = CREATE_ALWAYS; break;
        case Disposition::OpenAlways: how = OPEN_ALWAYS; break;
        case Disposition::TruncateExisting: how = TRUNCATE_EXISTING; break;
        case Disposition::OpenExisting: break;
        }
        HANDLE h = CreateFileW(wide(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, how, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (err)
                *err = (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) ? FileError::Exists
                       : e == ERROR_ACCESS_DENIED                           ? FileError::AccessDenied
                                                                            : FileError::NotFound;
            return nullptr;
        }
        if (err) *err = FileError::None;
        return std::make_unique<WinFile>(h);
    }
    FileInfo stat(const std::string &path) override {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(wide(path.empty() ? "." : path).c_str(), GetFileExInfoStandard, &d)) return {};
        return info_from(d.dwFileAttributes, d.ftLastWriteTime,
                         (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow);
    }
    std::vector<std::string> list_dir(const std::string &path) override {
        std::vector<std::string> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(wide((path.empty() ? "." : path) + "\\*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return names;
        do {
            std::string n = utf8(fd.cFileName);
            if (n != "." && n != "..") names.push_back(std::move(n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return names;
    }
    bool make_dir(const std::string &path) override {
        return CreateDirectoryW(wide(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    }
    bool remove_file(const std::string &path) override { return DeleteFileW(wide(path).c_str()) != 0; }
    bool rename(const std::string &from, const std::string &to) override {
        return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;   // POSIX semantics
    }
    bool set_current_dir(const std::string &path) override { return SetCurrentDirectoryW(wide(path).c_str()) != 0; }
};

// ---------- folders ----------
// %LOCALAPPDATA%\MotoRacerRecomp (not roaming: the game files are ~100 MB), log in \logs.
// Forward slashes: the runtime joins paths with '/', which Windows accepts.
class WinPaths final : public HostPaths {
public:
    std::string data_dir() override {
        const wchar_t *base = _wgetenv(L"LOCALAPPDATA");
        if (!base || !*base) return "";
        std::string s = utf8(base) + "/MotoRacerRecomp";
        for (char &c : s)
            if (c == '\\') c = '/';
        return s;
    }
    std::string log_dir() override {
        std::string d = data_dir();
        return d.empty() ? d : d + "/logs";
    }
};

class WinHostInfo final : public HostInfo {
public:
    bool is_steam_deck() override {   // Windows installed on a Deck, launched through Steam
        const char *e = getenv("SteamDeck");
        return e && !strcmp(e, "1");
    }
};

}  // namespace

Host &host() {
    static WinVirtualMemory memory;
    static WinExecution execution;
    static WinCrashReporter crash;
    static WinFileSystem fs;
    static WinPaths paths;
    static WinHostInfo info;
    static Host h{memory, execution, crash, fs, paths, info};
    return h;
}

}  // namespace plat
