# Troubleshooting

- **Log**: `~/Library/Logs/MotoRacerRecomp/motoracer.log` on macOS, `MotoRacer/logs/motoracer.log` in the Windows / Linux / Steam Deck package (source builds: `~/.local/state/MotoRacerRecomp/` on Linux, `%LOCALAPPDATA%\MotoRacerRecomp\logs\` on Windows); the previous run is kept as `motoracer.log.1`. Please attach it to bug reports. On the PSP: `PSP/GAME/MOTORACER/motoracer.log`.
- **Setup screen error**: it shows the last lines of the failing step and the log path; attach the log to a [bug report](https://github.com/segovia-no/MotoRacerRecomp/issues).
- **Reinstall the game files**: delete the `game` and `gamecode` folders in the data folder (the MotoRacer folder for Windows / Linux / Steam Deck packages) ([where things are](install.md#where-things-are)) and start again; to keep saves and settings, first copy `game/data/CONFIG.BPT`, `game/data/bestscor.dat`, `game/motoracer.ini` and `game/recomp_state.txt` aside. Source builds: `./setup.sh path/to/installer.exe` again.
- **Reset settings**: delete the `[Recomp]` section of `motoracer.ini` and `recomp_state.txt` in the game folder; the game's own options are in `data/CONFIG.BPT`.
- **Game files not found**: they are looked for in this order: `--game-dir DIR`, `$MOTORACER_GAME_DIR`, `<data dir>/MotoRacerRecomp/game` (`~/Library/Application Support` on macOS, `$XDG_DATA_HOME` or `~/.local/share` on Linux, `%LOCALAPPDATA%` on Windows), `./extracted/app`.
- **"Not the GOG game.EXE"**: the build only accepts the GOG `game.EXE` (checked by hash), because the recompiler's analysis is specific to that binary.
- **Uninstall**: see [install.md](install.md#uninstall).
- **PSP problems**: building (toolchain, packages) and the PSP not listing the game: [psp-build.md](psp-build.md#troubleshooting). A crash on the PSP leaves `PSP/GAME/MOTORACER/motoracer.log` on the memory stick: attach it to a [bug report](https://github.com/segovia-no/MotoRacerRecomp/issues).
