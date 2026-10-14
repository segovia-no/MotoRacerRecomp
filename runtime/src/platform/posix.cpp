// POSIX adapters (macOS, Linux): mmap virtual memory, fd-based files, signal crash reports.
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "platform.h"

extern "C" void call_on_stack(void *stack_top, void (*fn)(void));   // stack_*.S

namespace plat {

HostPaths &os_paths();   // paths_macos.cpp / paths_xdg.cpp

namespace {

class PosixVirtualMemory final : public VirtualMemory {
public:
    uint8_t *reserve(uint64_t size) override {
        // NORESERVE + overcommit: pages cost RAM only once touched, so commit() is a no-op.
        void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        return p == MAP_FAILED ? nullptr : static_cast<uint8_t *>(p);
    }
    bool commit(void *, uint64_t) override { return true; }
    bool make_inaccessible(void *addr, uint64_t size) override { return mprotect(addr, size, PROT_NONE) == 0; }
};

class PosixExecution final : public Execution {
public:
    bool run_on_large_stack(uint64_t bytes, void (*fn)()) override {
        void *stack = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (stack == MAP_FAILED) return false;
        call_on_stack(static_cast<char *>(stack) + bytes, fn);
        return true;
    }
};

void (*fault_handler)(const Fault &) = nullptr;

void on_signal(int sig, siginfo_t *si, void *) {
    Fault f{};
    f.code = sig;
    if (sig == SIGSEGV || sig == SIGBUS) {
        f.kind = Fault::MemoryAccess;
        f.address = reinterpret_cast<uintptr_t>(si->si_addr);
    } else {
        f.kind = sig == SIGABRT ? Fault::Abort : Fault::Other;
    }
    fault_handler(f);
    _exit(2);
}

class PosixCrashReporter final : public CrashReporter {
public:
    void install(void (*on_fault)(const Fault &)) override {
        fault_handler = on_fault;
        static char altstack[1 << 16];
        stack_t ss{};
        ss.ss_sp = altstack;
        ss.ss_size = sizeof altstack;
        sigaltstack(&ss, nullptr);
        struct sigaction sa{};
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
    }
    void write_backtrace(FILE *out) override {
        void *frames[48];
        int n = backtrace(frames, 48);
        fflush(out);
        backtrace_symbols_fd(frames, n, fileno(out));   // no malloc: safe in a signal handler
    }
};

FileInfo info_from(const struct stat &st) {
    FileInfo i;
    i.exists = true;
    i.is_dir = S_ISDIR(st.st_mode);
    i.size = static_cast<uint64_t>(st.st_size);
    i.mtime = static_cast<int64_t>(st.st_mtime);
    return i;
}

class PosixFile final : public File {
public:
    explicit PosixFile(int fd) : fd_(fd) {}
    ~PosixFile() override { close(fd_); }
    int64_t read(void *dst, uint32_t n) override { return ::read(fd_, dst, n); }
    int64_t write(const void *src, uint32_t n) override { return ::write(fd_, src, n); }
    int64_t seek(int64_t offset, Seek from) override {
        int whence = from == Seek::Begin ? SEEK_SET : from == Seek::Current ? SEEK_CUR : SEEK_END;
        return lseek(fd_, offset, whence);
    }
    bool truncate_at_position() override { return ftruncate(fd_, lseek(fd_, 0, SEEK_CUR)) == 0; }
    FileInfo info() override {
        struct stat st;
        return fstat(fd_, &st) == 0 ? info_from(st) : FileInfo{};
    }

private:
    int fd_;
};

class PosixFileSystem final : public FileSystem {
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
        return std::make_unique<PosixFile>(fd);
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

}  // namespace

Host &host() {
    static PosixVirtualMemory memory;
    static PosixExecution execution;
    static PosixCrashReporter crash;
    static PosixFileSystem fs;
    static Host h{memory, execution, crash, fs, os_paths()};
    return h;
}

}  // namespace plat
