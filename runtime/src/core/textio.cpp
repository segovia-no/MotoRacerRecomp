#include "core/textio.h"

#include <cstdio>

namespace rt {

bool read_file(const std::string &path, std::vector<uint8_t> &data) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    data.clear();
    uint8_t buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    return true;
}

bool read_lines(const std::string &path, std::vector<std::string> &lines) {
    std::vector<uint8_t> data;
    if (!read_file(path, data)) return false;
    lines.clear();
    std::string cur;
    for (uint8_t c : data) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur.push_back(static_cast<char>(c));
    }
    if (!cur.empty()) lines.push_back(cur);
    return true;
}

bool write_file(const std::string &path, const std::string &data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

}  // namespace rt
