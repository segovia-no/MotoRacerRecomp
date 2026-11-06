// Native versions of hot race functions, group C (docs/25-psp-port.md, the PSP hardware profile).
// Hooked through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_0046bf80_t(void);
extern "C" void f_00461aa0_t(void);
extern "C" void f_00490220_t(void);
extern "C" void f_00403a80_t(void);
extern "C" void f_004b0f60_t(void);
extern "C" void f_0040c960_t(void);

namespace {
using namespace native;
}  // namespace

extern "C" int native_hot_46bf80(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_461aa0(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_490220(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_403a80(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_4b0f60(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_40c960(void) { return 0; }   // not native yet: translated
