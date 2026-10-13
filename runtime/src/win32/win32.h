// Shared state for the Win32 shims.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "runtime.h"

namespace w32 {

constexpr uint32_t INVALID_HANDLE = 0xFFFFFFFFu;
constexpr uint32_t HMODULE_GAME = rt::IMAGE_BASE;

// Win32 error codes used by the shims.
enum : uint32_t {
    ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5,
    ERROR_INVALID_HANDLE = 6, ERROR_NO_MORE_FILES = 18, ERROR_FILE_EXISTS = 80,
    ERROR_INSUFFICIENT_BUFFER = 122, ERROR_MOD_NOT_FOUND = 126, ERROR_PROC_NOT_FOUND = 127,
    ERROR_ALREADY_EXISTS = 183,
};

// ---------- kernel objects ----------
struct Object {
    virtual ~Object() = default;
};

uint32_t handle_add(std::unique_ptr<Object> obj);
Object *handle_get(uint32_t h);
template <class T> T *handle_as(uint32_t h) { return dynamic_cast<T *>(handle_get(h)); }
bool handle_close(uint32_t h);

// ---------- paths ----------
// Guest-visible install directory. The CD drive maps to the same host directory.
extern const char *const GUEST_ROOT;   // "C:\\MOTORACER"
std::string host_path(const std::string &guest_path);   // resolves case-insensitively
std::string guest_full_path(const std::string &guest_path);
extern std::string guest_cwd;          // always absolute, backslashes

// ---------- modules ----------
uint32_t module_handle_for(const std::string &dll_name);   // 0 if unknown
uint32_t proc_address(uint32_t hmodule, const std::string &name);

// ---------- resources (.rsrc of game.EXE) ----------
uint32_t find_resource(uint32_t type, uint32_t name, uint32_t lang = 0xFFFFFFFF);  // data-entry VA
uint32_t resource_data(uint32_t entry_va);
uint32_t resource_size(uint32_t entry_va);
// type/name arguments may be MAKEINTRESOURCE ids or guest string pointers
uint32_t resource_id_from_arg(uint32_t v, std::string *name_out);

}  // namespace w32
