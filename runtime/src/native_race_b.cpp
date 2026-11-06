// Native versions of hot race functions, group B (docs/25-psp-port.md, the PSP hardware profile).
// Hooked through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_00469c60_t(void);
extern "C" void f_00467930_t(void);
extern "C" void f_0048e180_t(void);
extern "C" void f_00466a80_t(void);
extern "C" void f_0045b4e0_t(void);

namespace {
using namespace native;
}  // namespace

extern "C" int native_hot_469c60(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_467930(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_48e180(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_466a80(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_45b4e0(void) { return 0; }   // not native yet: translated
