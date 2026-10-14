#include "com.h"

#include <array>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace rt;

namespace dx {

namespace {

std::map<uint32_t, ComObj *> objects;   // guest pointer -> host object

// One trap function per (vtable, slot) so the fatal message can name the method.
struct TrapInfo { const char *iface; int slot; };
constexpr int MAX_TABLES = 32, MAX_SLOTS = 64;
TrapInfo trap_info[MAX_TABLES][MAX_SLOTS];
int tables_used = 0;

template <int T, int S> void trap() {
    fatal("unimplemented COM method %s slot %d (this=0x%08x, called from 0x%08x)", trap_info[T][S].iface,
          trap_info[T][S].slot, arg(0), RD32(R.esp));
}

template <int T, size_t... S> constexpr std::array<Method, sizeof...(S)> traps_for(std::index_sequence<S...>) {
    return {&trap<T, static_cast<int>(S)>...};
}

template <size_t... T> constexpr auto all_traps(std::index_sequence<T...>) {
    return std::array<std::array<Method, MAX_SLOTS>, sizeof...(T)>{
        traps_for<static_cast<int>(T)>(std::make_index_sequence<MAX_SLOTS>{})...};
}

const auto trap_table = all_traps(std::make_index_sequence<MAX_TABLES>{});

}  // namespace

Guid read_guid(uint32_t va) {
    Guid g{};
    if (!va) return g;
    g.d1 = RD32(va);
    g.d2 = RD16(va + 4);
    g.d3 = RD16(va + 6);
    for (int i = 0; i < 8; i++) g.d4[i] = RD8(va + 8 + static_cast<uint32_t>(i));
    return g;
}

std::string guid_str(const Guid &g) {
    char b[40];
    snprintf(b, sizeof b, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.d1, g.d2, g.d3, g.d4[0], g.d4[1],
             g.d4[2], g.d4[3], g.d4[4], g.d4[5], g.d4[6], g.d4[7]);
    return b;
}

bool guid_eq(const Guid &a, const char *text) { return guid_str(a) == text; }

uint32_t make_vtable(const char *iface, std::initializer_list<Method> methods, int slots) {
    if (tables_used >= MAX_TABLES || slots > MAX_SLOTS) fatal("make_vtable: too many tables/slots");
    int t = tables_used++;
    uint32_t vt = heap_alloc(4 * static_cast<uint32_t>(slots));
    auto it = methods.begin();
    for (int s = 0; s < slots; s++) {
        Method m = it != methods.end() ? *it++ : nullptr;
        trap_info[t][s] = {iface, s};
        static std::deque<std::string> names;   // thunk names live as long as the thunks
        names.push_back(std::string(iface) + "::slot" + std::to_string(s));
        WR32(vt + 4 * static_cast<uint32_t>(s), register_thunk(m ? m : trap_table[t][s], names.back().c_str()));
    }
    return vt;
}

uint32_t expose(ComObj *obj, const char *iface, uint32_t vtable) {
    auto it = obj->faces.find(iface);
    if (it != obj->faces.end()) return it->second;
    uint32_t p = heap_alloc(8, true);
    WR32(p, vtable);
    WR32(p + 4, 0xC0FFEE00u | static_cast<uint32_t>(objects.size() & 0xFF));
    objects[p] = obj;
    obj->faces[iface] = p;
    return p;
}

ComObj *lookup(uint32_t guest_this) {
    auto it = objects.find(guest_this);
    return it == objects.end() ? nullptr : it->second;
}

uint32_t add_ref(uint32_t guest_this) {
    ComObj *o = lookup(guest_this);
    return o ? ++o->refs : 0;
}

uint32_t release(uint32_t guest_this) {
    ComObj *o = lookup(guest_this);
    if (!o) return 0;
    if (--o->refs) return o->refs;
    for (auto &[name, p] : o->faces) {
        objects.erase(p);
        heap_free(p);
    }
    delete o;
    return 0;
}

}  // namespace dx
