// PSP adapters (docs/25-psp-port.md): no virtual memory, newlib files, folders next to the EBOOT.
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "platform.h"
#include "buffered_file.h"

extern "C" void call_on_stack(void *stack_top, void (*fn)(void));   // stack_mips.S

namespace plat {

uint8_t *host_stack = nullptr;   // the recompiled code's stack (high-water mark: memory stats)
size_t host_stack_bytes = 0;

namespace {

// No MMU: the guest address space is one zeroed block (compact layout, runtime.h); commit is a no-op
// and nothing can be made inaccessible.
class PspVirtualMemory final : public VirtualMemory {
public:
    uint8_t *reserve(uint64_t size) override {
        if (size > 0xFFFFFFFFull) return nullptr;
        return static_cast<uint8_t *>(calloc(1, static_cast<size_t>(size)));
    }
    bool commit(void *, uint64_t) override { return true; }
    bool make_inaccessible(void *, uint64_t) override { return true; }
};

class PspExecution final : public Execution {
public:
    bool run_on_large_stack(uint64_t bytes, void (*fn)()) override {
        void *stack = malloc(static_cast<size_t>(bytes));
        if (!stack) return false;
        memset(stack, 0xA5, static_cast<size_t>(bytes));   // high-water mark (MOTORACER_MEM_STATS)
        host_stack = static_cast<uint8_t *>(stack);
        host_stack_bytes = static_cast<size_t>(bytes);
        call_on_stack(static_cast<char *>(stack) + (bytes & ~15ull), fn);
        return true;
    }
};

// PPSSPP and the PSP report crashes themselves.
class PspCrashReporter final : public CrashReporter {
public:
    void install(void (*)(const Fault &)) override {}
    void write_backtrace(FILE *) override {}
};

FileInfo info_from(const struct stat &st) {
    FileInfo i;
    i.exists = true;
    i.is_dir = S_ISDIR(st.st_mode);
    i.size = static_cast<uint64_t>(st.st_size);
    i.mtime = static_cast<int64_t>(st.st_mtime);
    return i;
}

class PspFile final : public File {
public:
    explicit PspFile(int fd) : fd_(fd) {}
    ~PspFile() override { close(fd_); }
    int64_t read(void *dst, uint32_t n) override { return ::read(fd_, dst, n); }
    int64_t write(const void *src, uint32_t n) override { return ::write(fd_, src, n); }
    int64_t seek(int64_t offset, Seek from) override {
        int whence = from == Seek::Begin ? SEEK_SET : from == Seek::Current ? SEEK_CUR : SEEK_END;
        return lseek(fd_, static_cast<off_t>(offset), whence);
    }
    bool truncate_at_position() override { return false; }   // no ftruncate in the PSP newlib (SetEndOfFile)
    FileInfo info() override {
        struct stat st;
        return fstat(fd_, &st) == 0 ? info_from(st) : FileInfo{};
    }

private:
    int fd_;
};

class PspFileSystem final : public FileSystem {
public:
    std::unique_ptr<File> open(const std::string &path, bool rd, bool wr, Disposition disp, FileError *err) override {
        int flags = wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
        switch (disp) {
        case Disposition::CreateNew: flags |= O_CREAT | O_EXCL; break;
        case Disposition::CreateAlways: flags |= O_CREAT | O_TRUNC; break;
        case Disposition::OpenAlways: flags |= O_CREAT; break;
        case Disposition::TruncateExisting: flags |= O_TRUNC; break;
        case Disposition::OpenExisting: break;
        }
        int fd = ::open(path.c_str(), flags, 0644);
        if (fd < 0) {
            if (err) *err = errno == EEXIST ? FileError::Exists : errno == EACCES ? FileError::AccessDenied : FileError::NotFound;
            return nullptr;
        }
        if (err) *err = FileError::None;
        return buffered_if_read_only(std::make_unique<PspFile>(fd), wr);
    }
    FileInfo stat(const std::string &path) override {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 ? info_from(st) : FileInfo{};
    }
    std::vector<std::string> list_dir(const std::string &path) override {
        std::vector<std::string> names;
        if (DIR *d = opendir(path.empty() ? "." : path.c_str())) {
            while (dirent *e = readdir(d)) {
                std::string n = e->d_name;
                if (n != "." && n != "..") names.push_back(std::move(n));
            }
            closedir(d);
        }
        return names;
    }
    bool make_dir(const std::string &path) override { return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST; }
    bool remove_file(const std::string &path) override { return unlink(path.c_str()) == 0; }
    bool rename(const std::string &from, const std::string &to) override { return ::rename(from.c_str(), to.c_str()) == 0; }
    bool set_current_dir(const std::string &path) override { return chdir(path.c_str()) == 0; }
};

// Everything lives next to the EBOOT (the working directory at start, e.g. ms0:/PSP/GAME/MOTORACER):
// game files in ./game, log and settings there too.
class PspPaths final : public HostPaths {
public:
    std::string data_dir() override { return dir(); }
    std::string log_dir() override { return dir(); }

private:
    static std::string dir() {
        static const std::string d = [] {
            char buf[512];
            return getcwd(buf, sizeof buf) ? std::string(buf) : std::string(".");
        }();
        return d;
    }
};

class PspHostInfo final : public HostInfo {
public:
    bool is_steam_deck() override { return false; }
};

}  // namespace

Host &host() {
    static PspVirtualMemory memory;
    static PspExecution execution;
    static PspCrashReporter crash;
    static PspFileSystem fs;
    static PspPaths paths;
    static PspHostInfo info;
    static Host h{memory, execution, crash, fs, paths, info};
    return h;
}

}  // namespace plat

// bytes of the host stack ever used (memory stats)
size_t psp_host_stack_used() {
    size_t i = 0;
    while (i < plat::host_stack_bytes && plat::host_stack[i] == 0xA5) i++;
    return plat::host_stack_bytes - i;
}
