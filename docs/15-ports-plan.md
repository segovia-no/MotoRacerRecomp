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

### P0: portability layer on macOS — done 2026-10-14
Adapter pattern: the runtime calls interfaces, and each OS supplies adapter classes behind them.

**`runtime/src/platform/platform.h`** defines five interfaces. `plat::host()` returns the build's adapters.

| Interface | Used by | POSIX adapter (`posix.cpp`) | Windows adapter (P3) |
|---|---|---|---|
| `VirtualMemory`: reserve / commit / make_inaccessible | memory.cpp | `mmap` NORESERVE; commit is a no-op | `VirtualAlloc` reserve + commit |
| `Execution`: run_on_large_stack | main.cpp | `mmap` stack + `call_on_stack` (`stack_arm64.S`, `stack_x86_64_sysv.S`) | thread or fiber with a big stack |
| `CrashReporter`: install / write_backtrace | dispatch.cpp | `sigaction` + `sigaltstack`, `backtrace_symbols_fd` | vectored exception handler |
| `FileSystem` + `File` | kernel32.cpp, support.cpp, main.cpp, dispatch.cpp | `open`/`read`/`lseek`/`ftruncate`, `opendir`, `stat` | `CreateFileW` etc. (UTF-8 → UTF-16) |
| `HostPaths`: data_dir / log_dir | main.cpp, dispatch.cpp | `paths_macos.cpp` (~/Library/…), `paths_xdg.cpp` (XDG) | `%APPDATA%` |

- CMake picks the adapter files from the OS and CPU. An unknown OS or CPU stops the configure step with a pointer
  to this document.
- Outside `platform/`, the runtime contains no POSIX or macOS headers or calls. Exceptions:
  - standard C/C++ (`fopen`, `<fstream>`, `strcasecmp`);
  - SDL, for timing: `x86_rdtsc` now uses `SDL_GetTicksNS`.
- Other host-independent replacements:
  - the Win32 `FindFirstFile` wildcard is our own matcher in place of `fnmatch`;
  - `FileTimeToSystemTime` uses `std::chrono` calendar types in place of `gmtime_r`.
- **Guest memory commits:**
  - at start: the fixed low region (stack, image, relocated tables), TEB/PEB and the thunk range;
  - heap and VirtualAlloc blocks, as they are handed out.
  This is a no-op on POSIX. Windows needs it so that the 1.8 GB arenas are not committed up front.

**Shaders:**
- The 4 programs are now GLSL 450 in SDL_GPU's binding layout:
  - vertex: uniforms in set 1;
  - fragment: sampler in set 2, uniforms in set 3.
- Sources: `runtime/shaders/{d3d,overlay}.{vert,frag}`.
- `tools/build_shaders.sh` (glslang + spirv-cross, dev only) writes the committed
  `runtime/src/gpu/shaders_gen.h`. It holds SPIR-V and MSL; MSL uses `--msl-decoration-binding`, so
  buffer/texture/sampler index = binding.
- `gpu/shaders.{h,cpp}` holds one `ShaderBackend` adapter per format. `create()` picks the first format that
  `SDL_GetGPUShaderFormats(dev)` accepts. The device is created with every shipped format: Metal on macOS,
  Vulkan elsewhere.
- DXIL can be added later as one more backend.

**Verification (macOS):**
- An E2E race (`p0_base` = old build from a worktree, `p0_new`) at 1920×1080 16/9, D3D FILTERED.
- Static frames are pixel-identical: title, select race, track and HUD during the countdown. The remaining diffs
  are only animated objects (balloon, birds, water, countdown blink) from run timing.
- A software-renderer menu run is fine.
- Measured memory in a race: about 180 MB resident, about 410 MB physical footprint including Metal buffers.
  The "4 GB" is reserved *address space*, not RAM.

### P1 + P2: Linux and Steam Deck — implemented 2026-10-14 (awaiting test on the owner's Deck)
**Build environment: Valve's Steam Runtime 3 "sniper" SDK** (`registry.gitlab.steamos.cloud/steamrt/sniper/sdk`)

| Item | What it has | How we handle it |
|---|---|---|
| Base | Debian 11, glibc 2.31 | — |
| CMake | 3.25 | — |
| C/C++ compiler | gcc-14 (libstdc++ linked statically by default) | `gcc-14`; its clang 11 is too old |
| SDL3 | 3.4.16, but linked straight to about 50 desktop libraries | not used; `MOTORACER_BUNDLED_SDL=ON` builds SDL3 3.4.16 from the release tarball (SHA-256 pinned) as a static library |
| Python | 3.9, no pip | `tools/linux/sdk_build.sh` creates a venv `--without-pip` and bootstraps pip with get-pip.py |

- The bundled SDL loads X11, Wayland, libdecor, PipeWire, PulseAudio, ALSA, Vulkan, udev and dbus with
  `dlopen`. Result: one binary that needs only glibc ≥ 2.29 and runs directly on SteamOS (glibc 2.41) and other
  distributions, with no Steam Linux Runtime needed.
- Recompiler dependencies are pinned in `tools/recomp/requirements.txt`: pefile 2024.8.26, capstone 5.0.9.

**Portability fixes found by gcc/libstdc++:** a missing `#include <memory>` (dsound.cpp; libc++ had included
it indirectly). Nothing else; the generated C compiles with gcc-14 unchanged.

**Setup** (`./setup.sh` → `tools/linux/setup_linux.sh`): podman (rootless, preinstalled on SteamOS 3.5+) or docker.
1. Unpack the installer: with the host's innoextract if present, otherwise innoextract from a throwaway
   `debian:bookworm-slim` container.
2. Append the `[Recomp]` defaults (`tools/default_settings.ini`, shared with macOS).
3. Build in the SDK container; the log goes to `build/linux/build.log`.
4. Install:
   - binary at `~/.local/share/MotoRacerRecomp/bin/motoracer` (debug info stripped);
   - icon `motoracer.png`, taken from the GOG `.ico`'s 256 px PNG (`tools/linux/ico_to_png.py`);
   - menu entry `~/.local/share/applications/motoracer-recomp.desktop`;
   - Steam library entry via `steamos-add-to-steam`, only when Steam is already running, detached and limited to 20 s
     (`NO_STEAM=1` skips it); otherwise the script prints the manual steps.
   - First Deck test (2026-10-14): the script hung at this step. `steamos-add-to-steam` passes a `steam://` link to
     Steam and waits; with Steam not running in Desktop Mode it started Steam and never returned. Fixed as above.
   - Owner report (2026-10-16): every re-run added another library entry. The script now looks for the install path
     (`~/.local/share/MotoRacerRecomp/bin/motoracer`) in Steam's binary `userdata/*/config/shortcuts.vdf` files
     (`grep -a`) and adds the game only if no shortcut has it. The binary is replaced in place, so the existing entry
     starts the new build. Duplicates made earlier must be removed in Steam by hand.

**Re-running setup (all platforms, 2026-10-16):**
- With an existing install (`MotoRacer.app`, `bin/motoracer`, `MotoRacer.exe`), setup asks
  "Rebuild and replace it? [Y/n]" when run interactively; non-interactive runs continue.
- Player files: `data/CONFIG.BPT` (options, controls, speedometer style), `data/bestscor.dat` (best times),
  `motoracer.ini` (launcher + `[Recomp]` settings), `data/Champ*.sav` (championships, not in the installer),
  `recomp_state.txt` (runtime), `windows/game.ini`. A plain re-run never extracts again. Before, giving the
  installer again overwrote the first three with the GOG originals; now files that already exist are kept
  (`install_game.sh`, `setup_linux.sh`, `setup.ps1`). Tested by extracting over a folder with modified copies.

**Platform adapters added:**
- `paths_xdg.cpp` (data `$XDG_DATA_HOME`, log `$XDG_STATE_HOME`; parents created);
- `stack_x86_64_sysv.S`;
- `HostInfo::is_steam_deck()`: `SteamDeck=1`, which Steam sets on a Deck, or `ID=steamos` in `/etc/os-release`.

**16:10 screen modes** (needed for the Deck's native 1280×800):
- The 16/9 MODE list now holds both shapes: 1280×720, 1280×800, 1920×1080, 1920×1200, 2560×1440, 2560×1600 and
  3840×2160.
- The game computes the band once, in the viewport setup `0x499b80`, on two paths (`lea eax,[eax+eax*8]` =
  `(W/16)·9`, then `round(band·4/3·H/W)` by ftol). The viewport height `0x68b008`, centre `0x624f88` and bar offset
  `0x6d1040` all derive from the result.
- Hook `game_hook_band` at `0x499C8D` / `0x499CF8` (eax = rounded band, ebp = W, edi = H) sets W·10/16 for
  16:10 entries.
- The game keys modes by area, so a 16:10 entry uses its 4:3 mode plus 2 rows (1280×962 ↔ 1280×800). Pixels are
  then 0.2% off square (focal lengths ∝ H/480), which is invisible.
- `present_blt` accepts 16:10 bands.
- The HUD follows the band edges with no changes.

**First-run defaults:**
- **Resolution:** the largest band, at least 1280 wide and up to 2560, that fits the display in its shape (16:10
  if the display is nearer 1.6 than 1.78). Deck: 1280×800; 1080p TV: 1920×1080; the owner's 1440p Mac:
  2560×1440, unchanged.
- **Input:** on a Steam Deck, CONTROLLER = JOYSTICK even if the pad isn't enumerated yet; the Xbox preset then
  applies.

**Verification:**
- **macOS:** E2E race at 1920×1200 16:10. The label reads 1920*1200, the render target is 1920×1200, and the
  frame is full 16:10 with the HUD at the edges.
- **Linux x86-64, headless** (Docker amd64 on the Mac, Xvfb + Mesa lavapipe Vulkan):
  - menus and a full race on Vulkan with the SPIR-V shaders;
  - simulated Deck first run (1280×800 screen, `SteamDeck=1`, no state): JOYSTICK, 16/9 MODE at 1280×800,
    a 1280×800 render target.
  - The Xbox preset needs a real pad: the container has none, so the game fell back to the keyboard.
- **Container extraction:** produces the GOG `game.EXE` (hash checked).
- **First on-device test** (owner, 2026-10-14): the game runs in Game Mode at 1280×800. Three problems, all fixed:
  - the setup hung at `steamos-add-to-steam` (see above);
  - MASTER VOLUME started at 0 (10-input-audio.md);
  - the gamepad was dead until the joypad config page, and the JOYSTICK default didn't stick: the Steam virtual pad
    appears after launch, and pads were opened lazily (10-input-audio.md).
- **Retest after the fixes** (owner, 2026-10-14): the gamepad works from launch, JOYSTICK is the default, and
  performance in Game Mode is "flawless".
- **Still manual:** Steam gives a new non-Steam shortcut the "Keyboard (WASD) and Mouse" template. Choose
  "Gamepad with Joystick Trackpad" once.

### P3: Windows — implemented 2026-10-14 (verified under Wine; awaiting the owner's PC)
**Toolchain: llvm-mingw** (clang 23, mingw-w64, UCRT), not MSYS2.
- It is the same toolchain for the cross-build on the Mac (`tools/windows/mingw-cross.cmake`) and the player
  build on Windows (`setup.ps1`). The Windows release zip is portable, pinned and SHA-256 checked; there is no
  system-wide install.
- `setup.ps1` gets CMake 4.4.3 and Ninja 1.13.2 from pip into the build venv. Python itself (3.12) comes from
  winget, per user, if missing.

**Adapters** (`platform/win32.cpp`):

| Interface | Implementation |
|---|---|
| VirtualMemory | `VirtualAlloc` reserve/commit. A vectored exception handler commits any other untouched page in the reservation on first access (POSIX overcommit semantics); guard ranges excluded. |
| Execution | a **fiber** with a 512 MB stack on the main thread (no trampoline asm, no linker stack flag) |
| CrashReporter | `SetUnhandledExceptionFilter` + `SIGABRT`; `SetThreadStackGuarantee` leaves room after a stack overflow; backtrace as module+offset from `RtlCaptureStackBackTrace` |
| FileSystem | UTF-16 Win32 APIs; `rename` = `MoveFileExW(REPLACE_EXISTING)`, as POSIX |
| HostPaths | `%LOCALAPPDATA%/MotoRacerRecomp` (not roaming: about 100 MB of game files), log in `logs/` |
| HostInfo | `SteamDeck=1` (Windows on a Deck) |

**Program:**
- GUI subsystem (`-mwindows`) and a static C++ runtime with SDL3 bundled. The only imports are system DLLs and
  the UCRT.
- `SDL_main.h` provides `WinMain`.
- A generated `motoracer.rc` embeds `runtime/src/platform/motoracer.manifest` (activeCodePage UTF-8, PerMonitorV2
  DPI, Windows 10/11) and the GOG icon.

**Portability fixes the MinGW build found:**
- Generated code used BSD `_setjmp`. `RECOMP_SETJMP`/`RECOMP_LONGJMP` (recomp.h) are `_setjmp`/`_longjmp` on POSIX
  and the CRT `setjmp`/`longjmp` on Windows, which unwinds through SEH.
- `asprintf` (com.cpp thunk names) → `std::string` storage.
- Missing `<cstdlib>` (support.cpp, winmm_joy.cpp).
- Log flushed after every write, because the Windows CRT treats `_IOLBF` as full buffering. A killed run left an
  empty log.

**GPU:** the device is created with SPIR-V + MSL, so SDL picks **Vulkan** on Windows. A DXIL backend (D3D12) is
optional later, for machines without Vulkan drivers.

**Verification:**
- Cross-built `motoracer.exe` under Wine 8 (Docker amd64, Xvfb, lavapipe): menus and a full race on Vulkan.
- The log is written to `%LOCALAPPDATA%\MotoRacerRecomp\logs`.
- innoextract 1.9 (Windows, 32-bit) under Wine unpacks the GOG installer; the `game.EXE` hash matches.
- `setup.ps1` parses (PowerShell parser). It has not run on Windows yet.

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
- [x] P0 portability layer + cross-compiled shaders
- [x] P1 Linux (container build, headless-verified)
- [x] P2 Steam Deck: verified on the owner's Deck (2026-10-14). Pending: automatic Steam Input template
- [x] P3 Windows: implemented, verified under Wine; on-PC test pending
- [ ] P4 CI
