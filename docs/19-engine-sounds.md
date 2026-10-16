# Engine sounds: the player's, the opponents', trackside

Owner question (2026-10-16): some versions let you hear the opponents' engines rev dynamically instead
of a steady hum — is that in this build's code?

## Two sound systems in a race (race frame 0x47d660, at 0x47d8f2)
| Flag | Update | What it does |
|---|---|---|
| `0x4db65c` | `0x4079e0` → `0x406290` | **Bike engines.** Loops over the bikes (count `0x509bac`, positions from the array at `0x584489`): the player's engine (sound object `0x5034b0`, bike `0x581218`) and the nearest opponents. `0x406290` computes pitch from the bike's **rpm** (`bike+0x24+0xf8` = `+0x11c`) / max rpm (`params+0x74`), clamped 0.05..1, through pitch tables per engine type (`0x4db618`/`0x4db5a8` or `0x4db638`/`0x4db5e0`, chosen by `+0x120`); volume from distance, pan from direction. |
| `0x4db660` | `0x405f30` → `0x405bc0` → `0x405030` | **Positional emitters** (sample per slot from `0x519ff8`; slots `0x503568` count, entries `0x50356c`, objects `0x5037ec`, 0x40 each): volume by distance (`[slot+0x30]` range), pan by angle, pitch = fixed base `[slot+0x28]` (+ a ramp with descriptor flag 0x40, + a sine wobble with flag 0x10). No rpm input. Measured: each started once at one pitch (one-shot / trackside sounds). |

- Both flags are zeroed whenever the sound system starts (`0x408720`) and by the SOUNDS / VOICES options
  (`0x408b30`, `0x408f30`, which also zero the config values `MotoState` `0x5028a0` / `AmbiantState`
  `0x50354c`). At every race start `0x4084e0` (from `0x47bf70`) sets **both to 1** if SOUNDS is on and
  both are 0. So they are on in every race.
- Toggles: `0x408900` (bike engines, `0x4db65c`), `0x408800` (emitters, `0x4db660`); they start / stop the
  sounds.
- **Hidden developer menu: F11 in a race** (key handler `0x431250`, case `0x4321f9`; again F11 closes it)
  opens a 7-item list built by `0x43a400`: CD, SND, MOTO, AMB, VOICE, DISP, HIDE, with callbacks
  `0x43b680` (music), `0x43b690` (`0x408900`), `0x43b6a0` (sounds), `0x43b6b0` (voices), `0x43b6c0`
  (`0x408800`). The race keeps running. Which label belongs to which flag is not confirmed.

## Measurements (E2E, SetFrequency calls per sound buffer per 2 s)
- Racing: the player's engine buffer gets ~50–110 pitch updates, 28,000–44,000 Hz; emitter buffers get one.
- Player idling on the grid: the player's buffer stays at 16,450 Hz; four more buffers get a pitch update
  every frame (16,570 / 16,670 / 16,770 / 16,870 Hz) — the four nearest opponents; after GO they continue
  at 9,700–10,900 Hz and fade out as the bikes leave.
- Bike array dump (`0x581218 + 12809·i`, speed `+0x118`, rpm `+0x11c`) mid-race: the player (slot 1)
  speed 76–229, rpm 2,800–11,400; **all 23 opponents speed 2, rpm 800 (idle) for the whole race**.

## Answer
The per-opponent engine sound is in this build and on by default: the nearest opponents each have an
engine voice driven every frame by the same rpm model as the player's. But the AI moves its bikes with a
separate system and never updates their rpm (it stays at the idle 800), so every opponent plays a
near-constant pitch: the "humming". A dynamic version needs opponent rpm, e.g. derived from the AI
bike's real speed along the track with a simple gearbox, written to `bike+0x11c` before the sound update.

## Mistake made during the investigation
A first attempt treated the flags as "off by default" and added settings defaulting to off that switched
them off at race start: the owner then heard no engine at all (the player's engine is in the `0x4db65c`
system). Reverted, not committed.
