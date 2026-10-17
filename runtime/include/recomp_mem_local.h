/* Guest memory through a local copy of M (`mb`, declared by MEM_LOCAL at the top of every
 * translated function), so stores through it don't make the compiler reload the global M.
 * recomp.h includes this for generated code (RECOMP_GENERATED); hand-written native code includes
 * it after runtime.h / recomp.h (its own inline helpers keep using M). */
#undef MEMBASE
#undef MEM_LOCAL
#undef RD8
#undef RD16
#undef RD32
#undef RD64
#undef RDF32
#undef RDF64
#undef WR8
#undef WR16
#undef WR32
#undef WR64
#undef WRF32
#undef WRF64
#undef RD16A
#undef RD32A
#undef RD64A
#undef RDF32A
#undef RDF64A
#undef WR16A
#undef WR32A
#undef WR64A
#undef WRF32A
#undef WRF64A
#undef AL_
#define MEMBASE mb
#define MEM_LOCAL() uint8_t *const mb = M
#define RD8(a) (mb[(uint32_t)(a)])
#define RD16(a) ({ uint16_t r_; memcpy(&r_, mb + (uint32_t)(a), 2); r_; })
#define RD32(a) ({ uint32_t r_; memcpy(&r_, mb + (uint32_t)(a), 4); r_; })
#define RD64(a) ({ uint64_t r_; memcpy(&r_, mb + (uint32_t)(a), 8); r_; })
#define RDF32(a) ({ float r_; memcpy(&r_, mb + (uint32_t)(a), 4); r_; })
#define RDF64(a) ({ double r_; memcpy(&r_, mb + (uint32_t)(a), 8); r_; })
#define WR8(a, v) (mb[(uint32_t)(a)] = (uint8_t)(v))
#define WR16(a, v) do { uint16_t w_ = (v); memcpy(mb + (uint32_t)(a), &w_, 2); } while (0)
#define WR32(a, v) do { uint32_t w_ = (v); memcpy(mb + (uint32_t)(a), &w_, 4); } while (0)
#define WR64(a, v) do { uint64_t w_ = (v); memcpy(mb + (uint32_t)(a), &w_, 8); } while (0)
#define WRF32(a, v) do { float w_ = (v); memcpy(mb + (uint32_t)(a), &w_, 4); } while (0)
#define WRF64(a, v) do { double w_ = (v); memcpy(mb + (uint32_t)(a), &w_, 8); } while (0)
/* proven-aligned accesses (align_accesses in translate.py): one load/store on strict-alignment CPUs */
#define AL_(a, n) __builtin_assume_aligned(mb + (uint32_t)(a), n)
#define RD16A(a) ({ uint16_t r_; memcpy(&r_, AL_(a, 2), 2); r_; })
#define RD32A(a) ({ uint32_t r_; memcpy(&r_, AL_(a, 4), 4); r_; })
#define RD64A(a) ({ uint64_t r_; memcpy(&r_, AL_(a, 4), 8); r_; })
#define RDF32A(a) ({ float r_; memcpy(&r_, AL_(a, 4), 4); r_; })
#define RDF64A(a) ({ double r_; memcpy(&r_, AL_(a, 4), 8); r_; })
#define WR16A(a, v) do { uint16_t w_ = (v); memcpy(AL_(a, 2), &w_, 2); } while (0)
#define WR32A(a, v) do { uint32_t w_ = (v); memcpy(AL_(a, 4), &w_, 4); } while (0)
#define WR64A(a, v) do { uint64_t w_ = (v); memcpy(AL_(a, 4), &w_, 8); } while (0)
#define WRF32A(a, v) do { float w_ = (v); memcpy(AL_(a, 4), &w_, 4); } while (0)
#define WRF64A(a, v) do { double w_ = (v); memcpy(AL_(a, 4), &w_, 8); } while (0)
