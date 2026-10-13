# 14 — Draw distance and level of detail

Owner request (2026-10-14): DETAIL LEVELS 5 is still too aggressive for modern hardware — extend the
draw distance and LOD thresholds, mainly for opponent bikes and track sections.

## Track sections
- The track object at `0x52c108` (methods `0x40c560` ctor, `0x40c7e0` find section, `0x40ce60` build
  visible list → `0x5dabd8`, count `0x5dabcc`) draws `[track + 0x24]` sections ahead of the camera
  (one behind; half/half when the camera faces sideways, by the dot product of camera and section
  direction).
- `[track + 0x24]` is a constant **10**, set only by the constructor (`0x40c5b4`), the same at every
  detail level. None of the per-detail values below changes the track draw distance (measured).
- 20 sections ≈ 44k units of depth instead of ≈ 27k; 30 sections reach ≈ 52k but let you see inside
  the mountains on Speed Bay (owner) — the track data hides geometry with distance assumptions.

## Per-detail-level table
`0x51d1b8 + 36 × level` (DETAIL LEVELS 1..5 = level 0..4, global `0x4dc098`), filled per track from
its configuration by `0x428580` (first value via `0x4257e0`, the next four via `0x424c00`):

| Offset | Key | Detail 5 | Use |
|---|---|---|---|
| +0 | LenRLPlane | 3500 | multiplies the x/z of the in-front sign models (`0x46e675`) — ×3 made the signs 3× wide; not a culling distance |
| +4 | LenBPlane | 18000 | opponents culled beyond 2 × LenBPlane (`0x458b80` loop over `0x584421`, test `0x46e8b0`); camera setup `0x46e320` |
| +8 | FlatZ | 8000 | copied to `0x6d1f70` (`0x410e47`); raising it adds geometry |
| +12 | HorizFlatZ | 0 | — |
| +16 | MediumZone (int) | 5500 | opponent bike medium-model distance (copied per player `0x410190`) |
| +20 | HightZone (int) | 3500 | opponent bike high-model distance |
| +24/+28/+32 | gouraudTexturedZ / texturedZ / gouraudZ | 32000 / 48000 / 52000 | shading falls back to textured, then gouraud, then flat beyond these |

Other levels: 1 = 3500/18000/4000/0/5500/3500/0/0/0, 2 = …/3000/…/3500/0/0/0/4000,
3 = …/15000/4000/…/8000/12000, 4 = …/4000/12000/22000.

## Implementation
- Setting `DrawDistance=2` (`motoracer.ini [Recomp]`, 1 = original; dev override
  `MOTORACER_DRAW_DISTANCE`).
- Translator hook `game_hook_detail_loaded` at `0x4287ea` (right after the table load) scales, for
  every level: LenBPlane, FlatZ and the three shading distances × d, MediumZone/HightZone × 2d.
  LenRLPlane and HorizFlatZ are left alone.
- The per-frame tick sets the track's section count from 10 to 10 × d whenever it reads 10.
- Detail 5 at d = 2: opponents drawn to 72000, high/medium bike models to 14000/22000, 20 sections.
- Measured with a temporary per-frame depth histogram (E2E, keyboard throttle): ~60 fps unchanged at
  1920×1080; start-grid comparison shows the whole pack and the scenery past the arch.
