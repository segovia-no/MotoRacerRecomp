// Host platform adapters.
//
// The runtime reaches the host operating system only through the interfaces below (window,
// input, audio, GPU and timing go through SDL, which is portable already). Each OS supplies
// adapter classes implementing them; CMake compiles exactly one set:
//   posix_*.cpp    macOS and Linux (virtual memory, files, crash signals)
//   paths_macos.cpp / paths_xdg.cpp   per-OS folder conventions
//   stack_*.S      call_on_stack trampoline per CPU / object format
// and plat::host() hands out the instances. Port = add adapters, no changes elsewhere.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace plat {

// ---------- virtual memory ----------
// The guest address space is reserved once; regions are committed before first use. Hosts
// that commit lazily (POSIX overcommit) may treat commit() as a no-op.
class VirtualMemory {
public:
    virtual ~VirtualMemory() = default;
    virtual uint8_t *reserve(uint64_t size) = 0;          // nullptr on failure
    virtual bool commit(void *addr, uint64_t size) = 0;    // make readable/writable
    virtual bool make_inaccessible(void *addr, uint64_t size) = 0;
};

// ---------- execution ----------
class Execution {
public:
    virtual ~Execution() = default;
    // Runs fn on a fresh stack of `bytes` on the calling (main) thread: recompiled code nests one
    // host frame per guest call, and the window system needs the main thread. False if the stack
    // could not be allocated (fn did not run).
    virtual bool run_on_large_stack(uint64_t bytes, void (*fn)()) = 0;
    // Calls tick() about every interval_us from a timer (thread or interrupt), concurrently with
    // the game (the sampling profiler, prof.cpp). False if not available.
    virtual bool start_sampler(uint32_t interval_us, void (*tick)()) = 0;
};

// ---------- crash reporting ----------
struct Fault {
    enum Kind { MemoryAccess, Abort, Other } kind;
    uintptr_t address;   // faulting data address (MemoryAccess), else 0
    int code;            // signal / exception code, for the log
};

class CrashReporter {
public:
    virtual ~CrashReporter() = default;
    // on_fault runs on an emergency stack and must not return.
    virtual void install(void (*on_fault)(const Fault &)) = 0;
    virtual void write_backtrace(FILE *out) = 0;
};

// ---------- files ----------
struct FileInfo {
    bool exists = false;
    bool is_dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;   // seconds since 1970-01-01 UTC
};

enum class Disposition { OpenExisting, CreateNew, CreateAlways, OpenAlways, TruncateExisting };
enum class FileError { None, NotFound, Exists, AccessDenied };
enum class Seek { Begin, Current, End };

class File {
public:
    virtual ~File() = default;
    virtual int64_t read(void *dst, uint32_t n) = 0;          // bytes read, -1 on error
    virtual int64_t write(const void *src, uint32_t n) = 0;   // bytes written, -1 on error
    virtual int64_t seek(int64_t offset, Seek from) = 0;      // new position, -1 on error
    virtual bool truncate_at_position() = 0;
    virtual FileInfo info() = 0;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual std::unique_ptr<File> open(const std::string &path, bool read, bool write, Disposition disp,
                                       FileError *err) = 0;
    virtual FileInfo stat(const std::string &path) = 0;
    virtual std::vector<std::string> list_dir(const std::string &path) = 0;   // without . and ..
    virtual bool make_dir(const std::string &path) = 0;                       // true if it exists afterwards
    virtual bool remove_file(const std::string &path) = 0;
    virtual bool rename(const std::string &from, const std::string &to) = 0;
    virtual bool set_current_dir(const std::string &path) = 0;
};

// ---------- folders ----------
class HostPaths {
public:
    virtual ~HostPaths() = default;
    virtual std::string data_dir() = 0;   // per-user app data: <...>/MotoRacerRecomp (game files live in /game)
    virtual std::string log_dir() = 0;    // where motoracer.log goes
};

// ---------- device ----------
// ---------- loadable code ----------
// The game-code module (MOTORACER_GAME_MODULE, docs/28-easy-install.md).
class Modules {
public:
    virtual ~Modules() = default;
    virtual void *open(const std::string &path, std::string *error) = 0;   // nullptr on failure
    virtual void *symbol(void *module, const char *name) = 0;             // nullptr if missing
    virtual const char *suffix() = 0;                                      // ".so", ".dll"
};

class HostInfo {
public:
    virtual ~HostInfo() = default;
    // Steam Deck / SteamOS handheld: built-in gamepad, possibly not enumerated at the first look.
    virtual bool is_steam_deck() = 0;
};

struct Host {
    VirtualMemory &memory;
    Execution &execution;
    CrashReporter &crash;
    FileSystem &fs;
    HostPaths &paths;
    HostInfo &info;
    Modules &modules;
};

// The adapters for the OS this binary was built for.
Host &host();

}  // namespace plat
