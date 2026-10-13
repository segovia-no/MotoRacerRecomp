# Phase 1 — Function Discovery

Tool: `tools/recomp/discover_main.py <game.EXE> build/analysis/discovery.json`
(`tools/recomp/recomp/discover.py`, `pe_image.py`). Refuses any EXE whose SHA-256 is not the GOG build.

## Result (GOG game.EXE)
| Metric | Value |
|---|---|
| Decoded instructions | 243,456 |
| Functions | **4,106** (1 entry point, 1,648 call targets, 1,709 data-section pointers, 619 code-pointer operands, 129 shared-tail promotions) |
| Jump tables | 97 (46 MSVC two-level with byte index table) |
| Unresolved indirect `jmp` | 17 — all `jmp [reg+off]` virtual tail calls (MFC/CRT thunks) → runtime dispatch |
| Indirect `call` sites | 1,073 (vtables, function pointers) → runtime dispatch |
| Overlapping decodes / bad paths / shared insns | 0 / 0 / 0 |
| `.text` coverage | 97.1 % code + 5,960 B data tables; 80 gaps = 4,264 B **dead code** |
| Runtime | ~3.4 s |

Hopper (via REA) reports 9,219 procedures — it over-splits and also decodes data as code (see `0x4af2f2`). Our own discovery is the source of truth for the recompiler.

## Seeds
1. PE entry `0x4b6420`.
2. Direct `call rel32` targets.
3. Base-relocation slots **outside** `.text` whose value points into `.text` → vtables, callback tables, C++ EH tables (unwind funclets, catch blocks).
4. Base-relocated `push imm` / `mov reg, imm` operands pointing into `.text` (WndProc, thread procs, EH handler stubs).
   (Arithmetic immediates like `sub eax, 0x4a2ebf` in the VirtualProtect call are excluded.)

## Patterns learned
- **MSVC C++ EH**: every function with destructors registers a handler stub
  `mov eax, offset FuncInfo; jmp __CxxFrameHandler`. The common handler (`0x4b28e0`) is only reached by `jmp`, never `call`. Unwind funclets also tail-jump into destructors. → any jump target reached from ≥2 functions is promoted to its own function.
- **Switch tables**: `cmp idx, N; ja default; [xor eax,eax; mov al, byte [idx+IDX] | movzx eax, byte [idx+IDX]]; jmp [eax*4+TBL]`. The bound scan stays inside the block and follows the index register through the byte table. (A previous version grabbed an unrelated earlier `cmp eax,1` and truncated `0x4b4784`'s table — fixed.)
- **Byte lookup tables in `.text`**: CRT printf/scanf state machines etc. are referenced by `mov r8, [reg+disp]`; marked as data.
- **Developer-disabled code**: e.g. `0x466d80` starts `push…; nop; jmp 0x466ea8`, skipping 285 B of old code.
- **No-return imports**: `ExitProcess`, `RaiseException`, etc. stop fall-through (8 call sites).

## Dead code (unreferenced gaps, not translated)
Largest: `0x444bd3` (861 B), `0x4baa8e` (722 B), `0x4ba586` (592 B, CRT x87 helper wrappers), `0x466d8b` (285 B, disabled), `0x4bc520` (224 B, CRT `_control87` helper with its own 4-entry table). Verified: no relocated pointer and no `call/jmp rel32` from live code targets any gap.

## Next
- If runtime dispatch ever misses, the faulting VA tells us which seed is missing.
- Feed `discovery.json` to the translator (phase 2).
