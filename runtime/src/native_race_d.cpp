// Native versions of hot race functions, group D (docs/25-psp-port.md, the PSP hardware profile).
// Hooked through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_00493820_t(void);
extern "C" void f_00493490_t(void);
extern "C" void f_00492b10_t(void);
extern "C" void f_00485e50_t(void);
extern "C" void f_004b3400_t(void);

namespace {
using namespace native;
}  // namespace

extern "C" int native_hot_493820(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_493490(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_492b10(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_485e50(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_4b3400(void) { return 0; }   // not native yet: translated
