# Installing Moto Racer Recomp

> **You need your own copy of the game.** Buy *Moto Racer* on [GOG](https://www.gog.com/game/moto_racer) and download the offline installer (`setup_moto_racer_2.1.0.11.exe`). The downloads contain no game code or assets; everything is made from your installer on your own computer.

1. **Download** the package for your system from the [Releases page](https://github.com/segovia-no/MotoRacerRecomp/releases):

   | System | Download | Needs |
   |---|---|---|
   | Mac (Apple Silicon) | `MotoRacer-macos-arm64.zip` | macOS 12 or later |
   | Windows | `MotoRacer-windows-x86_64.zip` | Windows 10 or 11, 64-bit |
   | Steam Deck / Linux | `MotoRacer-linux-x86_64.tar.xz` | x86-64, glibc 2.29 or later |

2. **Put the GOG installer in your Downloads folder** (`setup_moto_racer_2.1.0.11.exe`: GOG › Moto Racer › Download offline backup game installers).
3. **Unpack the download and start Moto Racer** (details for your system below). On the first start it shows a setup screen:
   - It finds the GOG installer in your Downloads folder: choose **Install**. Elsewhere? Choose **Choose installer...** and select it.
   - It unpacks the game files and builds the game code on your computer, with a progress bar. This takes a minute or two and happens only once (and once after an update).
   - Then the game starts. From now on it starts straight away.

Everything needed for this comes in the download (Python, a C compiler and an installer unpacker, about 300 MB). Nothing is installed system-wide and nothing else is downloaded. Keep about 1.5 GB free.

No package for your system, or you want the latest code? [Build from source](../dev/building.md). The PSP has no download: its package is built on your computer ([PSP](#psp) below).

## Mac
- Double-click the zip, then move **MotoRacer.app** to your Applications folder and open it.
- The app is not notarized by Apple, so the first open is blocked ("Apple could not verify…"). Choose **Done**, then open **System Settings › Privacy & Security**, scroll down and choose **Open Anyway** next to MotoRacer. (macOS 12–14: right-click the app › **Open** › **Open**.) You only do this once.
- Still blocked? In Terminal: `xattr -dr com.apple.quarantine /Applications/MotoRacer.app`

## Windows
- Right-click the zip › **Extract All…**, then open the **MotoRacer** folder and double-click **motoracer.exe**.
- If SmartScreen says "Windows protected your PC": **More info › Run anyway** (the program is not code-signed).
- Keep `motoracer.exe` next to its `kit` folder. To start it from the Start menu or the desktop, right-click `motoracer.exe` › **Show more options › Send to › Desktop (create shortcut)**, or pin it to Start.

## Steam Deck
1. In **Desktop Mode**, download the GOG installer and the Linux package with the browser (both land in Downloads).
2. In Dolphin (the file manager), right-click `MotoRacer-linux-x86_64.tar.xz` › **Extract › Extract archive here**. Move the **MotoRacer** folder wherever you like, e.g. your home folder.
3. Add it to Steam: in Steam choose **Games › Add a Non-Steam Game to My Library… › Browse…**, pick **MotoRacer/motoracer** and **Add Selected Programs**.
4. Start it once from Steam in Desktop Mode, so the setup screen can build the game (it finds the installer in Downloads; choose **Install**).
5. Back in **Game Mode**, start *Moto Racer* from your library (under Non-Steam).
   - Controls: Steam gives new non-Steam games a keyboard layout. Once, open the game's controller settings (Steam button › Controller settings) and choose **Gamepad with Joystick Trackpad**; Steam remembers it.
   - The game starts in 16/9 MODE at the Deck's native **1280×800**. Docked to a TV? Choose e.g. 1920×1080 in Options › SCREEN MODE.

## Linux PC
Unpack and run:
```sh
tar -xJf ~/Downloads/MotoRacer-linux-x86_64.tar.xz -C ~
~/MotoRacer/motoracer
```
Keep `motoracer` next to its `kit` folder.

## PSP
Runs on every PSP model (PSP-1000 included) with custom firmware (e.g. ARK-4). The package is built on a Mac, Linux or Windows PC from your GOG installer:
1. Get the source (`git clone https://github.com/segovia-no/MotoRacerRecomp.git`, or Code › Download ZIP) and put the installer in your Downloads folder.
2. Build: `./setup.sh --psp` in a terminal in the source folder (macOS / Linux), or `setup.cmd --psp` in a Command Prompt there (Windows: builds in WSL). The first build downloads the PSP toolchain and takes several minutes.
3. Copy `build/psp-hw/PSP` to the root of the memory stick (merge it with the `PSP` folder there).
4. On the PSP: Game › Memory Stick › **Moto Racer Recompiled**.

Requirements per system, what the script installs, controls: [psp-build.md](psp-build.md).

## Updating
- **Mac**: replace MotoRacer.app with the new one.
- **Windows, Linux, Steam Deck**: extract the new package over the old MotoRacer folder, replacing its files. Do not delete the folder first: it also holds your game files, saves and settings (`game/`, `gamecode/`, `logs/`).

On the next start it rebuilds the game code once ("UPDATING"); saves, best times and settings are kept (see [Settings](playing.md#settings)). Coming from 0.1.2 or earlier on Windows / Linux: the first start moves your game files from the old per-user folder into the MotoRacer folder.

PSP: `git pull`, run `./setup.sh --psp` (or `setup.cmd --psp`) again and copy `build/psp-hw/PSP/GAME/MOTORACER/EBOOT.PBP` over the one on the memory stick (keeping the stick's `game` folder keeps your best times and settings).

## Where things are
Windows, Linux and Steam Deck packages keep everything in their own MotoRacer folder (next to `motoracer` and `kit/`):

| | macOS | Windows / Linux / Steam Deck | PSP (memory stick) |
|---|---|---|---|
| Data folder (game files, compiled game code, saves, settings) | `~/Library/Application Support/MotoRacerRecomp` | the MotoRacer folder: `game/`, `gamecode/` | `PSP/GAME/MOTORACER` (game files in `game/`) |
| Log | `~/Library/Logs/MotoRacerRecomp/motoracer.log` | `MotoRacer/logs/motoracer.log` | `PSP/GAME/MOTORACER/motoracer.log` |

If the MotoRacer folder is somewhere you cannot write to (e.g. under Program Files), and for source builds, the per-user folders are used instead: `~/.local/share/MotoRacerRecomp` (log: `~/.local/state/MotoRacerRecomp`) on Linux, `%LOCALAPPDATA%\MotoRacerRecomp` (log: `logs\` in it) on Windows.

## Uninstall
Delete the MotoRacer folder (Windows / Linux / Steam Deck: that is everything), or MotoRacer.app and the data folder above (Mac), or the `PSP/GAME/MOTORACER` folder (PSP). Source builds also made `~/Applications/MotoRacer.app` (macOS), `~/.local/share/applications/motoracer-recomp.desktop` (Linux) or a Start menu shortcut (Windows).

Problems? See [troubleshooting.md](troubleshooting.md).
