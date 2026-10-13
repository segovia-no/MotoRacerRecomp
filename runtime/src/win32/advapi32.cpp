// ADVAPI32 registry: an in-memory tree persisted to <game dir>/registry.txt.
//
// File format, one value per line:  KEY\PATH<TAB>name<TAB>type<TAB>hex-bytes
#include <fstream>
#include <map>
#include <sstream>

#include "win32.h"

using namespace rt;
using namespace w32;

namespace {

struct Value { uint32_t type; std::string data; };
struct Key { std::map<std::string, Value> values; };

std::map<std::string, Key> keys;   // full upper-case path -> key
std::map<uint32_t, std::string> open_keys;
uint32_t next_hkey = 0x0F0000;
bool loaded = false;
const char *kFile = "registry.txt";

std::string upper(std::string s) { for (auto &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c))); return s; }

void load() {
    if (loaded) return;
    loaded = true;
    std::ifstream in(kFile);
    std::string line;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string path, name, type, hex;
        std::getline(ss, path, '\t');
        std::getline(ss, name, '\t');
        std::getline(ss, type, '\t');
        std::getline(ss, hex, '\t');
        Value v{static_cast<uint32_t>(std::stoul(type)), ""};
        for (size_t i = 0; i + 1 < hex.size(); i += 2) v.data.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        keys[path].values[upper(name)] = v;
    }
}

void save() {
    std::ofstream out(kFile);
    for (auto &[path, k] : keys)
        for (auto &[name, v] : k.values) {
            out << path << '\t' << name << '\t' << v.type << '\t';
            for (unsigned char c : v.data) { char b[3]; snprintf(b, 3, "%02x", c); out << b; }
            out << '\n';
        }
}

std::string root_name(uint32_t h) {
    switch (h) {
    case 0x80000000: return "HKEY_CLASSES_ROOT";
    case 0x80000001: return "HKEY_CURRENT_USER";
    case 0x80000002: return "HKEY_LOCAL_MACHINE";
    case 0x80000003: return "HKEY_USERS";
    default: {
        auto it = open_keys.find(h);
        return it == open_keys.end() ? "" : it->second;
    }
    }
}

std::string join(uint32_t parent, uint32_t sub) {
    std::string base = root_name(parent);
    std::string s = sub ? read_cstr(sub) : "";
    return upper(s.empty() ? base : base + "\\" + s);
}

uint32_t open(const std::string &path) {
    uint32_t h = next_hkey;
    next_hkey += 4;
    open_keys[h] = path;
    return h;
}

}  // namespace

STDCALL(ADVAPI32, RegOpenKeyExA, 5) {
    load();
    std::string p = join(arg(0), arg(1));
    if (!keys.count(p)) {
        log("RegOpenKeyExA(%s) -> not found\n", p.c_str());
        return 2;   // ERROR_FILE_NOT_FOUND
    }
    WR32(arg(4), open(p));
    return 0;
}

STDCALL(ADVAPI32, RegCreateKeyExA, 9) {
    load();
    std::string p = join(arg(0), arg(1));
    bool existed = keys.count(p);
    keys[p];
    WR32(arg(7), open(p));
    if (arg(8)) WR32(arg(8), existed ? 2 : 1);   // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
    return 0;
}

STDCALL(ADVAPI32, RegCloseKey, 1) { open_keys.erase(arg(0)); return 0; }

STDCALL(ADVAPI32, RegQueryValueExA, 6) {
    load();
    auto k = keys.find(root_name(arg(0)));
    std::string name = upper(read_cstr(arg(1)));
    if (k == keys.end() || !k->second.values.count(name)) {
        log("RegQueryValueExA(%s, \"%s\") -> not found\n", root_name(arg(0)).c_str(), name.c_str());
        return 2;
    }
    const Value &v = k->second.values[name];
    if (arg(3)) WR32(arg(3), v.type);
    uint32_t cap = arg(5) ? RD32(arg(5)) : 0;
    if (arg(5)) WR32(arg(5), static_cast<uint32_t>(v.data.size()));
    if (arg(4)) {
        if (cap < v.data.size()) return 234;   // ERROR_MORE_DATA
        memcpy(M + arg(4), v.data.data(), v.data.size());
    }
    return 0;
}

STDCALL(ADVAPI32, RegSetValueExA, 6) {
    load();
    std::string p = root_name(arg(0));
    keys[p].values[upper(read_cstr(arg(1)))] = {arg(3), std::string(host_ptr(arg(4)), arg(5))};
    save();
    return 0;
}

STDCALL(ADVAPI32, RegDeleteValueA, 2) {
    load();
    keys[root_name(arg(0))].values.erase(upper(read_cstr(arg(1))));
    save();
    return 0;
}

STDCALL(ADVAPI32, RegDeleteKeyA, 2) {
    load();
    keys.erase(join(arg(0), arg(1)));
    save();
    return 0;
}
