// Minimal COM for host-implemented DirectX objects.
//
// A guest interface pointer points at 8 bytes of guest memory: [vtable VA][object id]. The
// vtable is a guest array of thunk addresses, so guest code calls methods normally
// (`call [ecx+N]`). One host object may expose several interfaces (IDirectDraw, IDirectDraw2, ...),
// each with its own guest pointer.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>

#include "runtime.h"

namespace dx {

constexpr uint32_t S_OK = 0;
constexpr uint32_t E_NOINTERFACE = 0x80004002;
constexpr uint32_t E_FAIL = 0x80004005;
constexpr uint32_t E_NOTIMPL = 0x80004001;
constexpr uint32_t E_INVALIDARG = 0x80070057;

struct Guid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; };
Guid read_guid(uint32_t va);
bool guid_eq(const Guid &a, const char *text);   // text like "6C14DB80-A733-11CE-A521-0020AF0BE560"
std::string guid_str(const Guid &g);

struct ComObj {
    uint32_t refs = 1;
    std::map<std::string, uint32_t> faces;   // interface name -> guest pointer
    virtual ~ComObj() = default;
};

using Method = void (*)();

// Builds (once per interface) a guest vtable with `slots` entries. `methods` fills the first
// slots in order; nullptr or missing entries become "unimplemented Iface::slot N" traps.
uint32_t make_vtable(const char *iface, std::initializer_list<Method> methods, int slots);

// Exposes `obj` through interface `iface` (creating the guest pointer on first use).
uint32_t expose(ComObj *obj, const char *iface, uint32_t vtable);
ComObj *lookup(uint32_t guest_this);
template <class T> T *obj(uint32_t guest_this) {
    T *o = dynamic_cast<T *>(lookup(guest_this));
    if (!o) rt::fatal("COM call on unknown object 0x%08x", guest_this);
    return o;
}
uint32_t add_ref(uint32_t guest_this);
uint32_t release(uint32_t guest_this);   // deletes the host object at zero

}  // namespace dx

// COM method shim: `self` = guest this pointer; arguments are rt::arg(1..NARGS).
#define COM_METHOD(NAME, NARGS)                                                          \
    static uint32_t NAME##_impl(uint32_t self);                                          \
    static void NAME() {                                                                \
        if (rt::trace_imports) rt::trace_call("COM", #NAME, (NARGS) + 1);               \
        uint32_t r_ = NAME##_impl(rt::arg(0));                                           \
        R.eax = r_;                                                                     \
        R.esp += 4 + 4 * ((NARGS) + 1);                                                 \
    }                                                                                   \
    static uint32_t NAME##_impl([[maybe_unused]] uint32_t self)
