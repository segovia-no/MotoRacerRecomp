// Handle table, path mapping, module registry and resource lookup.
#include <strings.h>

#include <cstdlib>
#include <map>
#include <vector>

#include "platform/platform.h"
#include "win32.h"

extern "C" {
typedef struct { uint32_t iat_va; const char *dll; const char *name; RecompFn fn; } ImportEntry;
extern const ImportEntry import_table[];
extern const uint32_t import_table_len;
}

namespace w32 {

// ---------- handles ----------
namespace {
std::map<uint32_t, std::unique_ptr<Object>> handles;
uint32_t next_handle = 0x100;
}  // namespace

uint32_t handle_add(std::unique_ptr<Object> obj) {
    uint32_t h = next_handle;
    next_handle += 4;
    handles[h] = std::move(obj);
    return h;
}

Object *handle_get(uint32_t h) {
    auto it = handles.find(h);
    return it == handles.end() ? nullptr : it->second.get();
}

bool handle_close(uint32_t h) { return handles.erase(h) != 0; }

// ---------- paths ----------
const char *const GUEST_ROOT = "C:\\MOTORACER";
std::string guest_cwd = GUEST_ROOT;

static std::string upper(std::string s) {
    for (auto &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string guest_full_path(const std::string &p) {
    std::string s = p;
    for (auto &c : s) if (c == '/') c = '\\';
    if (s.size() >= 2 && s[1] == ':') {
        if (s.size() == 2) s += "\\";
    } else if (!s.empty() && s[0] == '\\') {
        s = guest_cwd.substr(0, 2) + s;
    } else {
        s = guest_cwd + (guest_cwd.back() == '\\' ? "" : "\\") + s;
    }
    // normalise . and ..
    std::vector<std::string> parts;
    size_t i = 3;
    while (i <= s.size()) {
        size_t j = s.find('\\', i);
        if (j == std::string::npos) j = s.size();
        std::string part = s.substr(i, j - i);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        i = j + 1;
    }
    std::string out = s.substr(0, 3);
    for (size_t k = 0; k < parts.size(); k++) out += (k ? "\\" : "") + parts[k];
    return out;
}

// Find `name` in host directory `dir` ignoring case.
static std::string match_ci(const std::string &dir, const std::string &name) {
    auto &fs = plat::host().fs;
    std::string exact = dir.empty() ? name : dir + "/" + name;
    if (fs.stat(exact).exists) return exact;
    for (const std::string &e : fs.list_dir(dir))
        if (strcasecmp(e.c_str(), name.c_str()) == 0) return dir.empty() ? e : dir + "/" + e;
    return exact;
}

std::string host_path(const std::string &guest_path) {
    std::string full = guest_full_path(guest_path);
    std::string rest = full.substr(3);   // after "X:\"
    std::string root = upper(GUEST_ROOT).substr(3);
    if (upper(rest).rfind(root, 0) == 0) rest = rest.substr(root.size());
    // Both the install dir and the CD drive resolve under the game directory (cwd).
    std::string host;
    size_t i = 0;
    while (i < rest.size()) {
        if (rest[i] == '\\') { i++; continue; }
        size_t j = rest.find('\\', i);
        if (j == std::string::npos) j = rest.size();
        host = match_ci(host, rest.substr(i, j - i));
        i = j;
    }
    return host.empty() ? "." : host;
}

// ---------- modules ----------
namespace {
const char *const known_dlls[] = {"KERNEL32", "USER32", "GDI32", "ADVAPI32", "WINMM", "DDRAW",
                                  "DSOUND", "DINPUT", "DPLAYX", "COMCTL32", "COMDLG32",
                                  "WINSPOOL", "SHELL32", "CTL3D32", "OLE32"};
}

uint32_t module_handle_for(const std::string &dll_name) {
    std::string n = upper(dll_name);
    size_t slash = n.find_last_of("\\/");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    size_t dot = n.find('.');
    if (dot != std::string::npos) n = n.substr(0, dot);
    for (size_t i = 0; i < sizeof known_dlls / sizeof *known_dlls; i++)
        if (n == known_dlls[i]) return 0x10000000u + 0x10000u * static_cast<uint32_t>(i);
    if (n == "GAME") return HMODULE_GAME;
    return 0;
}

uint32_t proc_address(uint32_t hmodule, const std::string &name) {
    (void)hmodule;
    for (uint32_t i = 0; i < import_table_len; i++)
        if (name == import_table[i].name) return RD32(import_table[i].iat_va);
    return 0;
}

// ---------- resources ----------
namespace {
uint32_t rsrc_base() {
    uint32_t pe = rt::IMAGE_BASE + RD32(rt::IMAGE_BASE + 0x3C);
    return rt::IMAGE_BASE + RD32(pe + 24 + 96 + 2 * 8);   // data directory 2 = resources
}

std::u16string rsrc_name(uint32_t base, uint32_t off) {
    uint32_t a = base + (off & 0x7FFFFFFF);
    uint16_t n = RD16(a);
    std::u16string s;
    for (uint16_t i = 0; i < n; i++) s.push_back(static_cast<char16_t>(toupper(RD16(a + 2 + 2 * i))));
    return s;
}

// Find an entry in a resource directory by id, or by name when id has the string marker.
uint32_t rsrc_lookup(uint32_t base, uint32_t dir, uint32_t id, const std::string &name, bool any) {
    uint16_t named = RD16(dir + 12), ids = RD16(dir + 14);
    for (uint32_t i = 0; i < uint32_t(named + ids); i++) {
        uint32_t e = dir + 16 + 8 * i;
        uint32_t n = RD32(e), off = RD32(e + 4);
        bool match = any;
        if (!match && !name.empty() && (n & 0x80000000)) {
            std::u16string s = rsrc_name(base, n);
            std::string a;
            for (char16_t c : s) a.push_back(static_cast<char>(c));
            match = upper(name) == a;
        } else if (!match && name.empty() && !(n & 0x80000000)) {
            match = n == id;
        }
        if (match) return off;
    }
    return 0xFFFFFFFF;
}
}  // namespace

uint32_t resource_id_from_arg(uint32_t v, std::string *name_out) {
    if (v < 0x10000) return v;
    std::string s = rt::read_cstr(v);
    if (!s.empty() && s[0] == '#') return static_cast<uint32_t>(atoi(s.c_str() + 1));
    *name_out = s;
    return 0;
}

uint32_t find_resource(uint32_t type, uint32_t name, uint32_t lang) {
    uint32_t base = rsrc_base();
    std::string tname, nname;
    uint32_t tid = resource_id_from_arg(type, &tname), nid = resource_id_from_arg(name, &nname);
    uint32_t o1 = rsrc_lookup(base, base, tid, tname, false);
    if (o1 == 0xFFFFFFFF || !(o1 & 0x80000000)) return 0;
    uint32_t o2 = rsrc_lookup(base, base + (o1 & 0x7FFFFFFF), nid, nname, false);
    if (o2 == 0xFFFFFFFF || !(o2 & 0x80000000)) return 0;
    uint32_t o3 = rsrc_lookup(base, base + (o2 & 0x7FFFFFFF), lang, "", lang == 0xFFFFFFFF);
    if (o3 == 0xFFFFFFFF || (o3 & 0x80000000)) return 0;
    return base + o3;   // IMAGE_RESOURCE_DATA_ENTRY
}

uint32_t resource_data(uint32_t entry_va) { return rt::IMAGE_BASE + RD32(entry_va); }
uint32_t resource_size(uint32_t entry_va) { return RD32(entry_va + 4); }

}  // namespace w32
