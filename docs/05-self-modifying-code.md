# Self-Modifying Code (SMC)

## Discovery
- `VirtualProtect` has one caller: function at **`0x4af3f0`** (Hopper mis-identifies it as part of garbage proc `0x4af2f2`; bytes before it are a jump/lookup table).
- It calls `VirtualProtect(0x4a2ec0, 0x4aef10 - 0x4a2ebf, PAGE_EXECUTE_READWRITE /*0x40*/, &old)` → ~48 KB of `.text` made writable.
- Tool: `tools/smc_scan.py` (linear sweep of `.text`, finds memory operands with absolute disp inside the range).

## Patch sites (46 writes, all `mov [abs], reg`)
All writes target **operand bytes** (imm32 / imm8 / disp32), never opcodes. Placeholders in the original code are `0x12345678` (imm32/disp32) and `0x12` (imm8).

| Patched routine (inner loop) | Patcher(s) | Writes |
|---|---|---|
| `0x4a4e18`..`0x4a4e8b` span loop | `0x4a4e04`, setup at `0x4a4e92`..`0x4a4f14` | 13 |
| `0x4a4f??`..`0x4a4fdd` span loop | `0x4a4f74`, `0x4a4ff7`..`0x4a506e` | 13 |
| `0x4a5b0f`..`0x4a5c2e` (two-three loop copies) | `0x4a5b94`..`0x4a5c06` | 20 |

Example (loop at `0x4a4e18`): 16-bit RGB565 pixel blend/shade span.
- `and eax, imm32` @`0x4a4e24` (imm at `0x4a4e25`) — color mask
- `shl edx, imm8` @`0x4a4e21` (imm at `0x4a4e23`) — shift count
- `mov ax, [ecx*2 + disp32]` @`0x4a4e34` (disp at `0x4a4e38`) — lookup table base
- `add esi, imm32`/`adc ebx, imm32`/`adc cl, imm8` — fixed-point u/v steps
- `cmp edi, imm32` @`0x4a4e81` (imm at `0x4a4e83`) — span end pointer

## Recompiler handling
1. Keep a table of SMC operand slots: `{insn_va, operand_index, slot_va, width}`.
2. When translating an instruction whose immediate/displacement bytes overlap a slot, emit a **runtime load from guest memory** (`MEM32(slot_va)` / `MEM8(slot_va)`) instead of the constant.
3. Map `.text` into guest memory writable so the patcher's stores land naturally.
4. Debug builds: checksum the rest of `.text` each frame to catch unknown SMC (writes via register pointers would not be found by the static scan).

## Open questions
- Exact roles of the three routines (gouraud? translucency? fog?) — see later rasterizer analysis.
- Are there register-indirect writes into `.text`? (static scan can't see them; rely on runtime checksum).
