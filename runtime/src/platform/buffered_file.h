// Read-only files through a 32 KB buffer, for every platform adapter (docs/25-psp-port.md, loading
// time). The game's loaders read field by field — the track's .trk alone is ~190,000 reads of 2-10
// bytes — and each unbuffered read is a system call (~0.1 ms on the PSP). Reads come from the
// buffer; seeks only move the position (the file does not change while it is open read-only); large
// reads go straight to the destination.
#pragma once
#include <algorithm>
#include <cstring>
#include <memory>

#include "platform.h"

namespace plat {

class BufferedReadFile final : public File {
public:
    BufferedReadFile(std::unique_ptr<File> raw, int64_t size) : raw_(std::move(raw)), size_(size) {}
    int64_t read(void *dst, uint32_t n) override {
        uint8_t *out = static_cast<uint8_t *>(dst);
        uint32_t done = 0;
        while (done < n) {
            if (pos_ >= buf_pos_ && pos_ < buf_pos_ + buf_len_) {
                const uint32_t off = static_cast<uint32_t>(pos_ - buf_pos_);
                const uint32_t k = std::min(n - done, buf_len_ - off);
                memcpy(out + done, buf_.get() + off, k);
                done += k;
                pos_ += k;
                continue;
            }
            if (pos_ >= size_) break;
            if (raw_->seek(pos_, Seek::Begin) < 0) return done ? done : -1;
            if (n - done >= kBuf) {   // large read: straight into the destination
                const int64_t r = raw_->read(out + done, n - done);
                if (r < 0) return done ? done : -1;
                done += static_cast<uint32_t>(r);
                pos_ += r;
                break;
            }
            const int64_t r = raw_->read(buf_.get(), kBuf);
            if (r <= 0) break;
            buf_pos_ = pos_;
            buf_len_ = static_cast<uint32_t>(r);
        }
        return done;
    }
    int64_t write(const void *, uint32_t) override { return -1; }
    int64_t seek(int64_t offset, Seek from) override {
        const int64_t p = from == Seek::Begin ? offset : from == Seek::Current ? pos_ + offset : size_ + offset;
        if (p < 0) return -1;
        pos_ = p;
        return p;
    }
    bool truncate_at_position() override { return false; }
    FileInfo info() override { return raw_->info(); }

private:
    static constexpr uint32_t kBuf = 32 * 1024;
    std::unique_ptr<File> raw_;
    int64_t size_, pos_ = 0, buf_pos_ = 0;
    uint32_t buf_len_ = 0;
    std::unique_ptr<uint8_t[]> buf_ = std::make_unique<uint8_t[]>(kBuf);
};

// The adapters' open(): read-only files get the buffer.
inline std::unique_ptr<File> buffered_if_read_only(std::unique_ptr<File> f, bool wr) {
    if (wr || !f) return f;
    const int64_t size = static_cast<int64_t>(f->info().size);
    return std::make_unique<BufferedReadFile>(std::move(f), size);
}

}  // namespace plat
