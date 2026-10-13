# Phase 4 — DirectDraw (software renderer) and C++ Exceptions

Status (2026-10-13): **intro plays on macOS** (confirmed by project owner); 640×480×16 frames presented through SDL3/Metal.

## COM objects (`runtime/src/dx/com.*`)
- A guest interface pointer → 8 bytes of guest memory `[vtable VA][id]`. Vtables are guest arrays of thunk VAs, so guest code calls methods with ordinary `call [ecx+N]`.
- One host object may expose several interfaces (`IDirectDraw` + `IDirectDraw2`, `IDirectDrawSurface/2/3`); each gets its own guest pointer (`expose`).
- `COM_METHOD(Name, nargs)` = stdcall shim, `self` = guest `this`, args via `arg(1..n)`.
- Every unimplemented vtable slot is its own trap: `unimplemented COM method IDirectDrawSurface slot 30 (this=…, called from …)`.

## DirectDraw (`runtime/src/dx/ddraw.cpp`)
- Surfaces are guest-heap buffers, so the game's software rasterizer writes them directly after `Lock`. Pitch = `width*bytes` rounded to 4.
- Pixel format RGB565 (masks F800/07E0/001F); 8-bit palettized surfaces supported for presentation.
- `Flip` rotates buffer memory through the flip chain and presents the front buffer. Direct writes to a front buffer (Lock/Unlock or Blt without Flip) are presented from the message pump at most every 15 ms (`ddraw_tick`).
- `Blt`: color fill, copy, nearest stretch, mirroring, source color key (surface key or `DDBLTFX` override). `BltFast` with source key.
- Caps: blit, stretch, color key, color fill; **no 3D caps**. `QueryInterface(IID_IDirect3D)` → `E_NOINTERFACE`, so the game logs "D3D not available" and uses its **software renderer**.
- Display modes enumerated: 320×200 … 1024×768 in 8 and 16 bpp.
- `GetDC` on surfaces returns a fake HDC; GDI drawing onto surfaces is not rendered yet.

### Observed DirectDraw usage
1. `DirectDrawEnumerateA` → `DirectDrawCreate` → QI `IDirectDraw2` → `GetAvailableVidMem`, `EnumDisplayModes`, `GetCaps`, `GetDisplayMode` → release (probe at `0x475150`).
2. Later (from `0x41e79e`): create again, `SetCooperativeLevel(hwnd, EXCLUSIVE|FULLSCREEN)`, `SetDisplayMode(640,480,16)`, primary + 1 back buffer, QI `IDirectDrawSurface2` on the back buffer, QI `IDirect3D` (refused), `GetPixelFormat`.
3. Three 16-bit system-memory offscreen surfaces (256×128, 256×256, 256×256).
4. Video mode is re-initialised several times during boot (each screen sets it up again).
5. Rendering: `IsLost` → `Lock(back)` → draw → `Unlock` → `Flip`.

## Presentation (`runtime/src/host_video.cpp`)
SDL_Renderer (Metal), streaming texture in the surface's native format (RGB565 directly), logical size = surface size with letterboxing, linear filtering, vsync on.
Debug: `MOTORACER_SNAPSHOT_DIR=<dir>` saves every 60th frame as BMP (`sips -s format png` to convert).

## C++ exceptions / SEH (`runtime/src/eh.cpp`)
The game uses C++ exceptions in normal operation (e.g. parsing `Data\Demo\EA.dem` throws and is caught). MSVC implements them on Win32 SEH:

1. `_CxxThrowException` → `RaiseException(0xE06D7363, …)`.
2. Our `RaiseException` builds `EXCEPTION_RECORD`/`CONTEXT` on the guest stack and calls each handler on the `fs:[0]` chain (cdecl, 4 args).
3. `__CxxFrameHandler` (`0x4b28e0`) finds the catch, calls `RtlUnwind(frame, ReturnPoint, …)` — we call inner handlers with `EXCEPTION_UNWINDING`, pop records and return normally (both call sites pass their own return address as `TargetIp`, verified at `0x4b28a7` and `0x4b2d93`).
4. The catch funclet runs, then `_JumpToContinuation` (`0x4b283c`: `mov esp,[ebx-4]; mov ebp,[ebp-4]; jmp eax`) jumps into the catching function — a **non-local jump** for the host.

Translator support:
- `mov fs:[0], esp` (228 sites, the MSVC EH prologue) also emits `eh_register(R.esp, &eh_jb); if (_setjmp(eh_jb)) { call_indirect(eh_resume_va); return; }` — the function becomes resumable.
- `mov fs:[0], reg` / `pop fs:[0]` (unlink) emit `eh_unlink(new_head)` to drop dead records.
- `jmp` at `0x4b283c` and `0x4b2aff` (SE-translator guard) emit `nonlocal_jump(target)`: longjmp to the function owning the nearest registration record at/above the new `esp`, which then calls the continuation. Continuations are already separate recompiled functions (they are reloc-referenced from catch funclets, so discovery made them entries).

## Bug found: functions whose entry is not their lowest address
EH unwind funclets like `0x4731da` (`lea ecx,[ebp-0x36c]; jmp 0x4263d0`) own the destructor at `0x4263d0` (lower address, reached by no one else). The translator emitted bodies in address order and execution began at the *lowest* address, skipping the `lea` → null `this` → fault at guest address 8. Fixed: such functions start with `goto L_<entry>`. 10 functions affected. The instruction-level difftest can't see this class of bug; whole-function control flow is only validated by running the game.

## Not done yet
- DirectSound returns `DSERR_NODRIVER` → silent (game logs `CfxServer(create):directSoundCreate failed!` to `data/delphine.err` and continues).
- CD music: `mciSendCommandA(MCI_OPEN)` fails → no music (GOG ships tracks as `02.wav`–`13.wav`).
- DirectInput: `LoadLibrary("DINPUT.DLL")` + `GetProcAddress("DirectInputCreateA")` returns 0.
- `GetDC` text rendering on surfaces; Direct3D path; windowed/fullscreen toggles.

## Frame pacing of Blt-presented frames (2026-10-14)
The software renderer's main menu does not flip: each loop it Blts its 640×480 offscreen frame to the
primary (D3D debug Blt summary: ~62k full-screen Blts per 15 s). Real hardware would only show it at
the display rate; our Blt returned immediately, so the menu looped ~4000×/s and its checker
background animation (advanced once per loop) raced. A full-screen copy Blt into the primary front
buffer now presents the frame and waits for the next 1/60 s tick (`pace_frame`, `SDL_DelayPrecise`,
resynchronising when the game is slower). Races are unaffected: they flip (or Blt a partial 16:9 band)
and the game caps them itself (`-FrameRateMax60`).
