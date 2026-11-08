## What and why
<!-- What this changes, and the issue it fixes (e.g. "Fixes #4"). -->

## Tested on
<!-- Platforms and how: a race, the menus, check mode for native code, PPSSPP or a real PSP, ... -->
- [ ] macOS
- [ ] Windows
- [ ] Linux / Steam Deck
- [ ] PSP

## Checklist
- [ ] No game files or anything generated from `game.EXE` (translated C, binaries, PGO counts, snapshots) in the commits
- [ ] Builds from a clean checkout (`./setup.sh`, or the steps in docs/dev/building.md)
- [ ] Native rewrites: checked with `MOTORACER_NATIVE=check` (no `MISMATCH` in the log) and in a normal race; findings added to `docs/decomp/`
- [ ] Docs updated where behaviour, settings or build steps changed
