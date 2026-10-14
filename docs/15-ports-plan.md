# Linux, Steam Deck and Windows ports — plan (2026-10-14)

Goal: the same source-only model as on macOS. Each player builds from their own GOG installer with one
setup command on their own machine. No prebuilt game binaries. Targets:

| Target | Arch | GPU backend (SDL_GPU) | Toolchain |
|---|---|---|---|
| macOS (today) | arm64 | Metal (MSL) | Xcode CLT clang + Homebrew |
| Linux desktop | x86-64 (+ aarch64 for free) | Vulkan (SPIR-V) | distro clang/gcc, cmake |
| Steam Deck | x86-64 (SteamOS) | Vulkan (SPIR-V) | Steam Runtime "sniper" SDK container via podman |
| Windows 10/11 | x86-64 (+ arm64 later) | D3D12 (DXIL), Vulkan fallback | MSYS2 CLANG64 (clang + mingw-w64) |

## What is already portable
- **Generated code** is plain C11. It uses `uint32_t` guest addresses and `memcpy` loads, one
  `__builtin_parity`, and `setjmp` for SEH frames. It needs clang or gcc, not MSVC `cl`. It is little-endian
  only, which every target is. Self-modifying code is resolved at translation time (05-self-modifying-code.md),
  so nothing depends on the host.
- **Runtime** is about 8.6k lines of C++20 on SDL3: window, events, gamepads, audio (WAV CD tracks through
  SDL) and GPU through SDL_GPU. Guest path lookup is already case-insensitive (`match_ci` in
  win32/support.cpp), which Linux needs.
- **Recompiler** is Python with pefile and capstone. It runs on any OS.

## What is host-specific (inventory)
| Area | Where | macOS today | Linux | Windows |
|---|---|---|---|---|
| 4 GB guest space | memory.cpp `memory_init` | `mmap` NORESERVE + `mprotect` null guard | same | `VirtualAlloc(MEM_RESERVE)`, then commit per region (image, stack, EXT, heap/valloc arenas on allocation); `PAGE_NOACCESS` guard |
| Deep host stack (512 MB) | main.cpp + stack_switch_arm64.S | `call_on_stack` (arm64 Mach-O asm) | x86-64 SysV asm; aarch64 ELF variant (no `_` prefix) | linker `-Wl,--stack,0x20000000`, no asm needed |
| Crash report | dispatch.cpp | `sigaction` + `sigaltstack` + `backtrace` | same (glibc execinfo) | `AddVectoredExceptionHandler` / `SetUnhandledExceptionFilter`, `CaptureStackBackTrace` |
| Shaders | d3d_gpu.cpp `kShaders` (4 entry points, MSL source) | MSL | SPIR-V | DXIL (or SPIR-V on Vulkan) |
| Game, log dirs | main.cpp, dispatch.cpp, setup.sh, install_game.sh | `~/Library/Application Support/…`, `~/Library/Logs/…` | `$XDG_DATA_HOME/MotoRacerRecomp/game`, log in `$XDG_STATE_HOME` | `%APPDATA%\MotoRacerRecomp\game`, log beside it |
| POSIX file APIs | kernel32.cpp, support.cpp (`dirent`, `stat`, `unistd`, `strcasecmp`) | yes | yes | mingw-w64 provides them; non-ASCII paths need UTF-8 (manifest `activeCodePage=UTF-8`) |
| Fullscreen hotkey | host_window.cpp | Cmd+F, Alt+Enter | add F11 | add F11 |
| Packaging | tools/make_app.sh (`sips`, `iconutil`, `codesign`) | .app | `.desktop` + PNG icon from the GOG `.ico` | Start-menu shortcut; `.rc` icon from the GOG `.ico` |
| Setup | setup.sh (brew) | yes | apt/dnf/pacman detection; SDL3 from source if the distro lacks it | `setup.ps1` → MSYS2 (`winget`), then setup.sh in the CLANG64 shell |
| E2E tools | e2e.sh (`perl` alarm, `build/cmake/motoracer`) | yes | yes (+ `xvfb-run` for headless) | via MSYS2 bash |

## Phases

### P0: portability layer on macOS (no behaviour change)
1. Add `runtime/src/platform/platform.h` with one implementation file per OS (`platform_posix.cpp`,
   `platform_win32.cpp`). It covers:
   - guest reserve and commit;
   - stack allocation and `call_on_stack`;
   - data and log directories, using `SDL_GetPrefPath` on Linux and Windows;
   - crash handlers.
   `memory.cpp`, `main.cpp` and `dispatch.cpp` call only this layer.
2. **Shaders:**
   - Rewrite the 4 entry points once in HLSL in `runtime/shaders/*.hlsl`.
   - Compile them offline with **SDL_shadercross** to MSL, SPIR-V and DXIL.
   - Commit the outputs as C headers in `runtime/shaders/gen/`. They are our own code (MIT), so this is allowed.
   - Add `tools/build_shaders.sh` to regenerate them.
   - Create the device with every format we have and pick the matching blob at runtime from
     `SDL_GetGPUShaderFormats`.
   - Check the shadercross binding layout: vertex uniforms in space1, fragment uniforms in space3,
     sampler/texture in space2.
3. Move the arm64 asm under `runtime/src/platform/` and select it by CMake system and processor.
4. **Verification:** the macOS E2E scenarios (menus, race at 1920×1080 16/9, software renderer) must give
   snapshots identical to the pre-change ones (`tools/e2e_sheet.py` side by side, plus a pixel diff). The difftest
   must show 0 failures.

### P1: Linux desktop (also the base for Steam Deck)
1. Add `stack_switch_x86_64.S` (SysV: save `rsp` and the frame, switch, call, restore). Also build the ELF
   aarch64 variant.
2. CMake: on non-Apple systems, drop the `app` target and add an `install` target:
   - `~/.local/share/MotoRacerRecomp/bin/motoracer`;
   - a `.desktop` entry;
   - an icon extracted from the GOG `.ico` with a small Python ICO→PNG step in place of `sips`.
3. Make `setup.sh` multi-OS:
   - detect `apt` / `dnf` / `pacman` for cmake, clang, python3-venv and innoextract;
   - if `find_package(SDL3)` fails, build SDL3 with CMake `FetchContent` from a pinned release tag. This is
     needed for older distros such as Ubuntu 24.04 and Debian 12.
4. `install_game.sh`: the default destination comes from the OS.
5. **Verification without Linux hardware, on this Mac:**
   - run Docker/OrbStack `linux/arm64` containers with Mesa **lavapipe** (software Vulkan) and `xvfb-run`;
   - run the same E2E scenarios headless and diff the snapshots against macOS;
   - then repeat in a `linux/amd64` container under Rosetta to cover the x86-64 asm and build.

### P2: Steam Deck
SteamOS has a read-only root and no compilers, but it ships **podman** (with distrobox on 3.5 and later).
1. `setup.sh --steamdeck` (or auto-detect `/etc/os-release` `ID=steamos`) runs the build inside the
   **Steam Runtime 3 "sniper" SDK** image (`registry.gitlab.steamos.cloud/steamrt/sniper/sdk`, glibc 2.31):
   - SDL3 is built from source and bundled as `libSDL3.so` with `RPATH=$ORIGIN`, so the binary runs on any
     SteamOS 3.x;
   - innoextract, pefile and capstone are installed inside the container only.
2. Install the game to `~/.local/share/MotoRacerRecomp/` and add it to Steam as a non-Steam game:
   - use `steamos-add-to-steam` where available, otherwise print the Desktop Mode steps;
   - optionally set the "Steam Linux Runtime 3.0 (sniper)" compatibility tool for guaranteed libraries.
3. Deck defaults:
   - **First-run resolution:** choose the largest 16:9 band that fits the display. Today the code assumes
     ≥1080 and falls back to 1920×1080. The Deck panel is 1280×800, so it must get 1280×720. Fix this in
     game_hooks.cpp; it helps small laptops too.
   - Always fullscreen (gamescope forces it anyway); `ControllerPreset=1`; VSync 60.
   - Steam Input: the default "Gamepad" template exposes an Xbox pad to SDL, so our preset applies. Esc is
     Start/B, which already works.
   - **Check:** keyboard-only screens (best-lap name entry?). Steam's on-screen keyboard (Steam+X) types
     through SDL key events. Test it.
4. **Later (roadmap):** a native **16:10** picture (1280×800) in place of 1280×720 letterboxed.
5. **Verification:** the sniper SDK container runs on any x86-64 Linux, including the P1 `linux/amd64` setup. A
   final check on real Deck hardware needs the owner's Deck.

### P3: Windows
1. **Toolchain: MSYS2 CLANG64.** Reasons:
   - clang compiles the generated C unchanged;
   - mingw-w64 supplies `dirent`, `unistd` and `strcasecmp`;
   - pacman has `mingw-w64-clang-x86_64-{toolchain,cmake,ninja,sdl3,python,innoextract}`;
   - its bash runs `setup.sh` and the E2E scripts.
   Visual Studio + clang-cl + vcpkg stays possible later, but it needs more `#ifdef`s.
2. `platform_win32.cpp`:
   - **Memory:** reserve 4 GB, then commit:
     - image sections at load;
     - stack, TEB and EXT at init;
     - heap and VirtualAlloc arenas page-wise as `Arena::alloc` hands out blocks (avoids a 1.8 GB commit
       charge).
   - **Crashes:** vectored exception handler that reports guest faults.
   - **Stack:** main-thread stack through the linker.
3. **SEH/`setjmp`:** mingw-w64 x64 `longjmp` unwinds through frames (via `RtlUnwindEx`), and clang emits the
   unwind tables it needs. If it misbehaves or is slow, switch the generated code to `__builtin_setjmp` /
   `__builtin_longjmp`. **Gate:** the difftest plus an E2E race that triggers the CRT exception paths.
4. **App manifest:**
   - UTF-8 active code page, for non-ASCII user folders;
   - PerMonitorV2 DPI awareness;
   - WIN32 subsystem (no console).
   Icon `.rc` built locally from the GOG `.ico`.
5. **GPU:** D3D12 by default (any DX12-class GPU from 2015 on); Vulkan as fallback through SDL's driver hint.
6. **Setup:**
   - `setup.ps1` installs MSYS2 with `winget` if it is missing, then runs `setup.sh` in the CLANG64
     environment;
   - output goes to `%LOCALAPPDATA%\Programs\MotoRacerRecomp\` (exe + SDL3.dll) plus a Start-menu shortcut;
   - **bonus:** detect an existing GOG install in the registry (`HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\…`)
     and copy its files in place of the installer.
7. **Verification:**
   - needs Windows: real hardware, or Windows 11 ARM in UTM/Parallels running the x64 build under
     emulation;
   - VM GPUs may lack D3D12, so test Vulkan or WARP there and treat real hardware as the final check;
   - an arm64-Windows native build (llvm-mingw aarch64) is an optional extra.

### P4: CI without game code
GitHub Actions matrix (macos-14, ubuntu-24.04, windows-2022/MSYS2):
- build the runtime, shaders and recompiler unit tests;
- link against a small stub `gen/` (empty dispatch table), so no game code or `game.EXE` is ever needed.

This catches portability regressions on every push and keeps the repo rules intact.

## Order and size
P0 (1–2 sessions) → P1 (1–2) → P2 (1) → P3 (2–3) → P4 (1). Linux first because the Deck builds on it. Windows is
independent after P0.

## Risks
- **SDL3 availability:** older distros lack it. Mitigation: pinned FetchContent fallback.
- **Shader translation drift** (MSL→HLSL rewrite). Mitigation: P0 pixel-diff gate on macOS before any port
  ships.
- **Windows `longjmp` across generated frames.** Mitigation: `__builtin_setjmp` fallback.
- **glibc mismatch on the Deck.** Mitigation: build in sniper (glibc 2.31).
- **No owner hardware for Windows/Deck yet.** Mitigation: container and VM testing, with the final check on
  real devices.

## Status
- [ ] P0 portability layer + cross-compiled shaders
- [ ] P1 Linux
- [ ] P2 Steam Deck
- [ ] P3 Windows
- [ ] P4 CI
