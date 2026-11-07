# Building the PSP version

One script turns the GOG installer into a folder for the PSP's memory stick. It runs on macOS, Linux
and Windows (through WSL). Port details and history: [25-psp-port.md](../notes/25-psp-port.md).

## What you need
- The GOG offline installer `setup_moto_racer_2.1.0.11.exe` (GOG › Moto Racer › Download offline backup
  game installers). The script looks for it in your Downloads folder.
- A PSP with custom firmware (e.g. ARK-4 on a PSP-1000; any model works: the package fits the PSP-1000's
  memory and also runs on PSP-2000/3000/Go).
- About 2 GB free on the computer while building, 135 MB on the memory stick.
- macOS: Xcode Command Line Tools (`xcode-select --install`) and [Homebrew](https://brew.sh).
- Linux: a Debian/Ubuntu, Fedora, Arch or openSUSE system (x86-64 or arm64); the script installs its
  packages with your package manager after asking (sudo).
- Windows 10/11: WSL with a Linux distribution (Ubuntu is used when installed). Without it, open
  PowerShell as administrator, run `wsl --install`, restart, open Ubuntu once to create your user.

## Build
Get the source (`git clone https://github.com/segovia-no/MotoRacerRecomp.git`, or Code › Download ZIP).

macOS / Linux, in a terminal in the source folder:
```sh
./setup.sh --psp ~/Downloads/setup_moto_racer_2.1.0.11.exe
```

Windows: double-click **`setup.cmd --psp`** (or drop the installer onto it). It runs the same script in
WSL; type your Linux password when asked (once, for the packages). Explorer opens the result.

The first build downloads the PSP toolchain (pspdev, ~170 MB, checksum-verified, kept in `build/pspdev`)
and recompiles `game.EXE` for the PSP: several minutes. Later runs (after a `git pull`) only rebuild
what changed; no argument is needed once the game is unpacked.

## Install on the PSP
Copy the folder **`build/psp-hw/PSP`** to the root of the memory stick (merge it with the `PSP` folder
there). The game is in `PSP/GAME/MOTORACER` (the EBOOT and your game files, the CD music as MP3). On the
PSP: Game › Memory Stick › **Moto Racer Recompiled**.

Controls: Cross accelerate, Square brake, analog stick / D-pad steer, R / L gear up / down, Circle
stunts / turbo, Start pause, Select speedometer. Menus: D-pad, Cross confirm, Circle back.

## What the script does
1. Installs missing packages: `cmake`, `make`, `innoextract`, `ffmpeg`, Python 3 with `venv`, `curl`
   (macOS: Homebrew, also `libmpc gmp mpfr zstd` for the toolchain's compiler; Linux: apt / dnf /
   pacman / zypper, asking first; `NO_INSTALL=1` only reports them).
2. Downloads the pinned pspdev release for your system into `build/pspdev` (SHA-256 checked), unless
   `PSPDEV` points at a working toolchain.
3. Creates the recompiler's Python environment (`.venv`: pefile, capstone, Pillow).
4. Unpacks the installer to the desktop version's game folder (`~/Library/Application
   Support/MotoRacerRecomp/game`, `~/.local/share/MotoRacerRecomp/game`; `GAME_DIR` to change it). An
   existing install is reused, with your settings and best times.
5. Configures and builds `build/psp` (`psp` toolchain file, `MOTORACER_PSP_1000=ON`).
6. Home-menu images (`scripts/psp/xmb_assets.sh`): with a desktop build in `build/cmake`, the title
   screen is rendered and cut into the icon and background; otherwise the GOG game icon.
7. Packages `build/psp-hw/PSP/GAME/MOTORACER` (`scripts/psp/package_hw.sh`): stripped EBOOT, game files
   without the Windows launcher, CD tracks as 128 kbps MP3, draw distance 1 (the original).

Under WSL with the source on a Windows drive, the build files go to `~/.cache/motoracer-psp` on the
Linux side (faster; Python environments do not work on the mapped drive); only the package is written
to `build\psp-hw`.

## Troubleshooting
- "the PSP compiler does not run": the details are in `build/pspdev-check.log`; usually a missing
  library (GMP, MPFR, MPC, zstd) — install it and run the script again.
- The game does not show up on the PSP: the folder must be `ms0:/PSP/GAME/MOTORACER/EBOOT.PBP`, and
  the PSP needs custom firmware (official firmware runs only signed games).
- A crash on the PSP leaves `PSP/GAME/MOTORACER/motoracer.log`: please attach it to a bug report.
- Faster code: developers can train profile-guided optimization with PPSSPP (`scripts/psp/pgo.sh`,
  macOS); the regular build does without it.
