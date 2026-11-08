# Contributing

Thanks for helping. Bug reports, ideas and pull requests are all welcome.

## Reporting a problem or an idea
Open an [issue](https://github.com/segovia-no/MotoRacerRecomp/issues/new/choose) and pick **Bug report** or **Feature request**. For bugs: the version, your platform and what happened; screenshots and, for crashes, the log file (`motoracer.log`, see [troubleshooting](docs/guides/troubleshooting.md)) help a lot.

Please check the [open issues](https://github.com/segovia-no/MotoRacerRecomp/issues) first, and never attach game files (`game.EXE`, `data/`, the CD tracks) or anything built from them.

## Pull requests
Pull requests come from your own fork:
1. Fork the repository on GitHub and clone your fork.
2. Build from source with your GOG installer: [docs/dev/building.md](docs/dev/building.md) (PSP: [docs/dev/psp.md](docs/dev/psp.md)). The repository layout and how the pieces fit together: [docs/dev/architecture.md](docs/dev/architecture.md).
3. Make your change on a branch, test it (below), and open a pull request against `main`. The pull request template has a short checklist.

Small, focused pull requests are easiest to review. For larger changes, open an issue first so we can agree on the approach.

### Testing
- Play the part you changed on the platforms you have; say in the pull request where you tested.
- Scripted end-to-end runs: `scripts/dev/e2e.sh` / `scripts/dev/e2e_race.sh` ([notes/13-e2e-testing.md](docs/notes/13-e2e-testing.md)); PSP in PPSSPP: `scripts/psp/e2e_psp.sh`.
- Native rewrites of game functions (`runtime/src/game/natives/`): run a race with `MOTORACER_NATIVE=check` (no `MISMATCH` lines in the log) **and** a normal race, and add what you learned about the game's code to [docs/decomp/](docs/decomp/README.md).
- Translator changes: `recompiler/tests/difftest.py` checks it instruction by instruction against the Unicorn emulator.

### Rules
- **No game content in the repository**: no game files, no generated C, binaries, PGO counts or snapshots made from `game.EXE`. Everything game-specific is made from each player's own copy.
- Keep OS-specific code behind the platform adapters (`runtime/src/platform/`).
- Match the style of the surrounding code; comments explain *why*.
- Update the docs when behaviour, settings or build steps change.

By contributing you agree that your contributions are released under the project's [MIT License](LICENSE).
