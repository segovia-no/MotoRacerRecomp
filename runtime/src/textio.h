// Small file helpers on stdio. (iostreams bring libstdc++'s locale machinery, ~0.5 MB of code that
// counts on the PSP-1000: docs/25-psp-port.md.)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rt {
// The file's lines without '\n' (a '\r' stays, as with std::getline). False if it cannot be opened.
bool read_lines(const std::string &path, std::vector<std::string> &lines);
bool read_file(const std::string &path, std::vector<uint8_t> &data);
bool write_file(const std::string &path, const std::string &data);   // binary: no newline translation
}  // namespace rt
