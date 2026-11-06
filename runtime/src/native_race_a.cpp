// Native versions of hot race functions, group A (docs/25-psp-port.md, the PSP hardware profile).
// Hooked through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_0046b120_t(void);
extern "C" void f_0046bdc0_t(void);
extern "C" void f_0046cd20_t(void);
extern "C" void f_0046c970_t(void);
extern "C" void f_00410010_t(void);
extern "C" void f_0040fd40_t(void);

namespace {
using namespace native;
}  // namespace

extern "C" int native_hot_46b120(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_46bdc0(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_46cd20(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_46c970(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_410010(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_40fd40(void) { return 0; }   // not native yet: translated
