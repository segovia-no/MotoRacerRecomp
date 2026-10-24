# Easy installation without toolchains (plan 2026-10-18; E1, E2 done)

Owner request: make installing simpler than "install tools / containers, then recompile". Decision
(owner, 2026-10-18): **option A** — keep the rule that nothing translated from `game.EXE` is ever
distributed, but ship everything else prebuilt, so the player's machine only translates and compiles
the game code, with tools that come in the download. Option B (prebuilt complete binaries, as
Zelda64Recomp / UnleashedRecomp ship) stays out of scope: it would distribute code translated from
`game.EXE`.

## Today
| Platform | The player installs | Then |
|---|---|---|
| macOS | Xcode Command Line Tools, Homebrew, `cmake` `sdl3` `innoextract`, a Python venv (pefile, capstone) | `setup.sh`: unpack, translate, build all, `MotoRacer.app` |
| Steam Deck / Linux | podman (preinstalled on SteamOS) + ~2 GB of images (Steam Runtime "sniper" SDK, a Debian container) | build inside the container |
| Windows | Python (winget), llvm-mingw, innoextract, CMake, Ninja (pinned downloads into `build/`) | `setup.cmd` → `setup.ps1` |

Measured (MacBook, 10 cores, 2026-10-18): translation 12.8 s (Python, one core); compiling the
generated C (68 files, 33 MB) at `-O2` 6 s wall / 40 s CPU. The runtime, SDL3 (built from source on
Linux) and the toolchain set-up are the rest. So the player's machine has little real work: the
friction is the tools.

## Target
Download one archive for the platform, open the app, pick the GOG installer, wait for a progress
bar (~30 s on a desktop, ~1–2 min on a Deck), play. No Homebrew, no containers, no Python, no
compiler installed system-wide. A new release rebuilds the game code by itself on first start.

## Design
### 1. Prebuilt host program + game-code module built on the player's machine
Split today's single binary in two:
- **Host** (`motoracer` / `MotoRacer.app` / `motoracer.exe`), built by CI: runtime, SDL3, shaders,
  the native reimplementations (`native_*.cpp`, already public source). It contains nothing
  translated from `game.EXE` (CI builds it against no game code at all, see 15-ports-plan.md P4).
- **Game module** (`gamecode.dylib` / `.so` / `.dll`), built at install: the translator's C only,
  compiled and linked into a shared library that the host loads at start.

Why a module rather than linking the final executable on the player's machine: the final link
would need what the host needs — on macOS the SDK's frameworks (Metal, Cocoa, … through SDL), on
Windows the system import libraries, on Linux the glibc-2.29 sysroot of the sniper SDK. A module of
plain C needs only a C compiler and its libc stubs, and the host can be signed / tested once.

Interface (both directions are small):
- module → host: guest memory and state (`M`, `R`, `F`), the import shims (`imp_*`, 284), the x86
  helpers and hooks the translator emits. The host exports them (Windows: `dllexport` + an import
  library shipped with the host for linking the DLL; macOS: `-bundle_loader` / `-undefined
  dynamic_lookup`; Linux: `-rdynamic`).
- host → module: the dispatch table and the ~26 guest functions the runtime calls by name
  (`f_<va>` / `f_<va>_t` from the native hooks, e.g. `f_0049c590_t`). The translator emits one
  exported table of these pointers (`recomp_module_api`), checked against a version number; the
  runtime calls through it instead of linking the symbols.
- Cost to check: globals reached through the GOT / import table in generated code (`MEM_LOCAL` and
  `REGS_LOCAL` already load them once per function). Gate: `MOTORACER_FRAME_WORK` benchmark,
  instructions per frame within 1–2% of the static build.
- The PSP stays a static developer build (pspdev), unaffected.

### 2. Bundled compiler: `zig cc`
One pinned toolchain for the three desktop platforms (clang inside, its own libc headers / stubs):
- targets `aarch64-macos` / `x86_64-macos`, `x86_64-windows-gnu`, `x86_64-linux-gnu.2.29` (the
  glibc floor of today's Deck build), no SDK or container needed for a C-only module;
- ~45 MB compressed per platform; pruned to the one target's headers it is smaller;
- options as today: `-O2 -fno-strict-aliasing`, `-ffp-contract` as now, files compiled in parallel
  (one job per core). Fallback if zig fails a target: clang from llvm-mingw (Windows) / Apple clang
  when the Command Line Tools exist (macOS).

### 3. Self-contained translator
The translator is Python (pefile, capstone). Options, in order of preference:
1. Freeze it with the runtime it needs (python-build-standalone + vendored wheels, or PyInstaller):
   no code changes, ~20–30 MB.
2. Later, port it to C++ inside the host (no Python at all, faster than 12.8 s); large, not needed
   for the first release.

### 4. First-run installer inside the host
- SDL3 file dialog: "Choose the GOG installer (`setup_moto_racer_2.1.0.11.exe`)"; also accepts an
  already-unpacked folder containing `game.EXE`.
- Unpack with a bundled `innoextract` (zlib licence; prebuilt per platform by CI), check
  `game.EXE`'s hash, translate, compile, link, load. A progress screen with the step and a log file
  for support.
- Cache key = hash(`game.EXE`) + translator version + compiler flags: a new release with an
  unchanged translator reuses the module; otherwise it rebuilds once, without asking for the
  installer again (the unpacked files stay in the data folder as today).

### 5. Release packaging (CI, GitHub Actions)
- macOS: `MotoRacer.app` in a `.dmg`/zip (universal or arm64 + x86-64), translator, zig, innoextract
  inside the bundle's `Resources`.
- Windows: portable zip (`motoracer.exe` + tools folder), optional Start-menu shortcut on first run.
- Linux / Steam Deck: tarball built in the sniper SDK container by CI (the player never runs a
  container), plus the existing "add to Steam" step from `setup_linux.sh`.
- Saves, settings and game files stay where they are today (`~/Library/Application
  Support/MotoRacerRecomp`, `~/.local/share/MotoRacerRecomp`, `%LOCALAPPDATA%\MotoRacerRecomp`).
- The source build (`setup.sh`, `setup.cmd`, by hand) stays for developers.

## Risks / open points
- **macOS Gatekeeper:** a downloaded unsigned app is blocked ("cannot be opened"). Without a
  Developer ID ($99/year) and notarisation, players need right-click › Open once (or `xattr -dr
  com.apple.quarantine`). The module built locally is not quarantined; zig ad-hoc signs arm64
  output (to verify). If the host is ever notarised with the hardened runtime, loading a locally
  built module needs the `disable-library-validation` entitlement.
- **Windows SmartScreen / antivirus:** unsigned exe and a DLL compiled on the spot may be flagged.
- **Steam Deck:** zig's glibc-2.29 target vs today's sniper build: verify on the owner's Deck.
- **Release size:** ~80–120 MB per platform with zig + frozen Python; prune zig's targets.
- **Legal line:** the host contains the hand-written native reimplementations and the hook tables
  (already public source); no translated code and no `game.EXE` data. CI never sees `game.EXE`.

## Phases
| Phase | Work | Check |
|---|---|---|
| E1 | Host / module split: exported host API, module API table, loader; source build produces both | e2e race + check mode on macOS; frame-work benchmark vs static |
| E2 | Build the module with `zig cc` from a script (translate → compile → link), all three OSes | same game on macOS, Windows (owner's PC), Deck |
| E3 | Frozen translator + bundled innoextract | runs on a machine without Python / Homebrew |
| E4 | First-run installer UI in the host (file dialog, progress, cache key) | fresh user account install |
| E5 | CI release packaging (P4 of 15-ports-plan.md extended), README "Download" section | owner installs from a release on Mac, PC and Deck |

## E1 done: host / game-code module split (2026-10-18)
`cmake -DMOTORACER_GAME_MODULE=ON` builds `motoracer` (1.3 MB on macOS: runtime + SDL, no translated
code) and `gamecode.so` / `gamecode.dll` (3.6 MB) next to it; the default build is unchanged (game
code linked in), and so is the PSP build.
- **Generated side** (`recomp_main.py`): every build exports one `RecompModule` (`recomp.h`):
  version, dispatch table, `recomp_fallbacks` (the `f_<va>_t` bodies of the functions with a native
  version, by address), the import table and the import slots. With `--module` each import shim
  `imp_<DLL>_<NAME>` is a thunk through `recomp_slots[i]` (or `import_unimplemented`) instead of a
  weak fallback that the runtime's strong definition replaces at link time. The module is compiled
  with hidden visibility (only `recomp_module` is exported, calls between guest functions stay
  direct) and links against the program's symbols (`ENABLE_EXPORTS`; macOS `-bundle_loader`).
- **Host side**: `rt::guest_code()` (`guest_code.cpp`) returns the linked `recomp_module` or loads
  the module at the first use (`MOTORACER_GAME_MODULE=<path>`, else `gamecode<suffix>` in the
  program's folder via `SDL_GetBasePath`), checks the version and fills the slots from a registry
  that `STDCALL` / `CDECL` build (`rt::ImportShim`, name → shim). `dispatch.cpp`, `loader.cpp` and
  `support.cpp` read the tables through it. The 26 guest functions the runtime calls by name
  (native hooks, check mode) are forwarders in module builds (`GUEST_FN` / `GUEST_FALLBACK`).
- **Platform adapter** `plat::Modules` (open / symbol / suffix): `dlopen` (POSIX), `LoadLibraryW`
  (Windows, not built yet), none on the PSP.
- **Checks (macOS)**: race with the module: log "4106 functions, 284 of 284 import shims", contact
  sheet normal; check mode: 38.4 M checks, 0 mismatches; PSP build boots and races.
- **Cost**: race-to-race comparisons vary ~10% with the scene (a first A/B showed +16%, the repeat
  −8%), so the gate ran on the bike-selection screen (fixed scene, `MOTORACER_FRAME_WORK`, PSP
  profile, two runs each, alternating): 14.07 M instructions per frame both ways (+0.03%), cycles
  within run-to-run noise. No measurable cost.
- Not yet: `MotoRacer.app` does not carry the module (`make_app.sh`), Windows / Linux module builds
  (E2, with the bundled compiler).

## E2 done: the module built with a bundled `zig cc` (2026-10-24)
`tools/build_gamecode.py --exe game.EXE --out DIR --zig ZIG [--target macos|linux|windows]
[--arch arm64|x86_64] [--host-lib LIB]`: translates with `--module`, compiles the 66 files in
parallel with `zig cc` (flags as the CMake `gamecode` target + `-fPIC -fvisibility=hidden -g0
-fno-sanitize=all`), links `gamecode.so` / `gamecode.dll` stripped (`-s`). zig's caches go under
DIR. Targets: `aarch64/x86_64-macos.11.0` (`-undefined dynamic_lookup`), `x86_64-linux-gnu.2.29`
(the Deck build's glibc floor), `x86_64-windows-gnu` (linked against the program's import library).
Pinned: **zig 0.16.0** (2026-04-13), SHA-256 per platform from ziglang.org's index (aarch64-macos
`b23d70de…1489`, x86_64-linux `70e49664…3d00`, x86_64-windows `68659eb5…5a7e`).
- **Program without game code**: with `MOTORACER_GAME_MODULE=ON` and no `GAME_EXE`, CMake builds the
  program only (no Python, no translation) — for CI. Windows: the `.exe` exports its symbols
  (`--export-all-symbols`) and comes with `libmotoracer.dll.a`; the module reads the program's data
  (`R`, `F`, `M`, `eh_resume_va`, `prof_region`) through `__declspec(dllimport)`
  (`RECOMP_HOST_DATA` in `recomp.h`). Both sides use the UCRT, so a guest exception's `longjmp`
  from the program into the module's `setjmp` works (seen under Wine: three nonlocal jumps while
  loading, the race continues).
- **macOS** (zig on the Mac, no Xcode SDK needed): 20–25 s for translate + compile + link on 10
  cores; the module is ad-hoc signed by zig's linker. Race OK; check mode 43 M checks, 0
  mismatches; fixed-scene benchmark same instructions as the Apple-clang module (14.07 M per frame),
  cycles 3–6% lower.
- **Linux / Deck**: the program built in the sniper SDK container (glibc 2.29), the module
  cross-built by zig on the Mac, a full race headless in Docker (Xvfb, lavapipe). zig running on
  Linux itself (in the sniper container, x86-64 emulated on the Mac) builds it as well: 67 s there.
- **Windows**: the program cross-built with llvm-mingw, the DLL by zig, a full race under Wine 8.
  zig running on Windows itself is untested (owner's PC).
- Module sizes: 3.5 MB (macOS), 3.7 MB (Linux), 3.5 MB (Windows); 26 MB before `-g0` / `-s` (zig
  adds debug info by default).
- Still for later phases: the translator needs Python (E3), the program does not build the module
  itself yet (E4), packaging (E5).
