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
| +16 | MediumZone (int) | 5500 | copied into the camera object (`0x410190`, ctor default `0x46d7d6`); read by camera code that compares opponents' distances (`0x46ceb4`) — **not** the bike LOD |
| +20 | HightZone (int) | 3500 | likewise (camera object +0x118) |
| +24/+28/+32 | gouraudTexturedZ / texturedZ / gouraudZ | 32000 / 48000 / 52000 | shading falls back to textured, then gouraud, then flat beyond these |

Other levels: 1 = 3500/18000/4000/0/5500/3500/0/0/0, 2 = …/3000/…/3500/0/0/0/4000,
3 = …/15000/4000/…/8000/12000, 4 = …/4000/12000/22000.

## Opponent bike models
- Not driven by the table. `0x45f160` computes d = |bike − camera| / FOV scale (`0x49a150`), stores
  it at `[bike + 0x634]` and sets `[bike + 0x638]`: d < 1000 → 0 (full model), 1000..2500 → 2,
  2500..3000 → 3 (reduced models), ≥ 3000 (`0x4d1040`) → 4 (sprite). The draw routine `0x45f290`
  takes the model tables `0x53a6ac`/`0x53a6d8` by that level. `[bike + 0x634]` is also read at
  `0x45e2c5` for a 900..1000 blend next to the full-model threshold.
- Owner: "the 3D bike opponents change to sprites" quite abruptly; scaling HightZone/MediumZone or
  FlatZ did not change it (owner-checked A/B).

## Implementation
- Setting `DrawDistance=2` (`motoracer.ini [Recomp]`, 1 = original; dev override
  `MOTORACER_DRAW_DISTANCE`).
- Translator hook `game_hook_detail_loaded` at `0x4287ea` (right after the table load) scales, for
  every level: LenBPlane, FlatZ and the three shading distances × d. LenRLPlane, HorizFlatZ and the
  camera zones are left alone.
- Translator hook `game_hook_bike_lod` at `0x45f1e0` (just before the threshold compare) divides
  ST(0) = d by 3d, moving every bike threshold 3d further out (blend included).
- The per-frame tick sets the track's section count from 10 to 10 × d whenever it reads 10.
- At d = 2: 20 sections, opponents drawn to 72000, full bike model to 6000, reduced models to
  15000/18000, sprites beyond 18000 (owner-confirmed A/B: bikes stay 3D down the whole straight).
- Measured with a temporary per-frame depth histogram (E2E, keyboard throttle): ~60 fps unchanged at
  1920×1080; start-grid comparison shows the whole pack and the scenery past the arch.
