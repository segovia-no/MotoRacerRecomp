# Record layouts

Offsets in hex. "?" = role not established. Sizes where known.

## Bike (0x3209 bytes; array at 0x584421, count [0x509bac], index 0 = the player)

| off | type | meaning |
|---|---|---|
| +0x00 | int | drawn this frame (0x458b80 sets 0 / 1) |
| +0x04 | int | human-controlled flag (with +0x08: the player's bike) |
| +0x08 | int | the player's bike (1 for bike 0) |
| +0x0c..+0x24 | float[3]x2 | vector pairs read by 0x490220 (angle of an object) |
| +0x30 | float[3] | position |
| +0x68 | int | active (raced) |
| +0x78 | int | model index (per-model table 0x5092b0, 292 bytes each) |
| +0x7c | int | gear |
| +0xb4 | float | max speed |
| +0xbc | float[12] | orientation matrix (rows: right +0xbc, up +0xc8, forward +0xd4; +0xe0 translation, zeroed) |
| +0xec | float | speed |
| +0x110 | float | steering (AI: clamped to +-pi/4 scaled) |
| +0x118 | float | integrated speed (AI) |
| +0x11c | float | engine revs |
| +0x120 | float | acceleration (AI, clamped -50..40) |
| +0x124 | float | speed (for lean / ratios) |
| +0x170 | int | passed to the faces call when drawing the far model |
| +0x18c | float[3] | heading (unit vector) |
| +0x198 | float[3] | forward vector (= +0xd4 + +0xe0, normalized) |
| +0x1a4 | float | target speed |
| +0x1b4 | float[3] | front point (wheel contact) |
| +0x1dc | ptr | model geometry table entry (0x51ac58 + 112 * lod) |
| +0x338 | float[3] | back point |
| +0x344 | float[3] | previous position (speed = distance / dt) |
| +0x360 | ptr | model entry (0x52c240 + 112 * lod) |
| +0x638 | int | level of detail (> 1: the simple model path of 0x45f290) |
| +0x2dac | int | ? (cleared by the step) |
| +0x2db8 | int | airborne (0 = on the ground) |
| +0x2dc8 | int | flag set by 0x46b120, restored around 0x466d70 |
| +0x2dd0 | int | own index |
| +0x2e1d | float | lean angle (smoothed with +0x2e21) |
| +0x2e21 | float | previous lean |
| +0x2e29 | int | ? (lean shaping: without it the steering scale is at least 20) |
| +0x2e39 | float | lean target offset |
| +0x2e3d | float[3] | angles (rotation of the rider / body) |
| +0x2e49 | float[3] | angles (another body part) |
| +0x2e55 | float[12] | frame from the back to the front point |
| +0x2e85 | int | airborne flag (step) |
| +0x2e8d | float | lean change per frame |
| +0x2e91 | float | sine of the lean phase |
| +0x2e95 | float | vertical speed on landing |
| +0x2e99 | float | lean phase |
| +0x2e9d | float | lean speed |
| +0x2ea1 | float | lean angle (shortest way to the target) |
| +0x2ea5 | float | ? (lean target term) |
| +0x2ea9 | float[3]x6 | six contact points (0x461aa0) |
| +0x3085 | float[...]| object read by 0x490220 for the ground angle |
| +0x307d | float | crash tumble angle (> 0 while tumbling) |
| +0x30b5 | float[3] | centre of four extra ground points (+0x30ed..) |
| +0x30c1 | int | off the track / crashed |
| +0x30dd | float | lean while crashed |
| +0x319d | int | ? (cleared by the step) |
| +0x31c1 | int | track sector index |
| +0x31c5 | ? | out argument of the track collision 0x40ca80 |
| +0x31d1 | float | blend towards the aim (decays by [0x581014]) |
| +0x31d5 | float | lateral offset on the track (aim point) |
| +0x31d9 | int | target bike / speed bonus factor (0x46cd20) |

Per-model table 0x5092b0 (292 bytes = 73 dwords per model): +0x00 / +0x04 / +0x08 / +0x20
distances (front / back points), +0x10 max speed constant (0x5092c0), +0x28 lean clamp
(0x5092d8), +0xe8 dirt-bike flag (0x509398: AI steering, road vs dirt code paths), +0xf8 ...

## Track (object at 0x52c108)

| off | meaning |
|---|---|
| +0 | sectors (252 bytes each) |
| +4 | sector count (positions wrap at it: 0x40ec30) |
| +8 | chunks (112 bytes each) |
| +0x10 | chunk count |
| +0x20 | current sector |
| +0x24 | draw range (chunks) |

Sector (252 bytes): +0x00 centre x y z, +0x0c a 4x3 matrix (x axis +0x0c, translation +0x30),
+0x3c chunk index, +0x40 plane (normal + d; also the direction compared with the camera axis),
+0x80 / +0x90 floor planes (above / below the plane at +0xa0), +0xa0 plane, +0xb0 half width,
+0xb4 int16[] object ids (count +0xd4), +0xe0 flag (0x465d60: lighting 0x10 / 0x1f), +0xf8 surface
type mask (7 bits).

Chunk (112 bytes): +0 faces, +4 vertices, +0x2c face count, +0x30 vertex count.

## Geometry

Transformed vertex (28 bytes, pool 0x6d20d8, count [0x68b014]): +0 x, +4 y, +8 z (camera space,
floats), +0xc light (int, flags << 13), +0x10 screen x, +0x14 screen y (ints), +0x18 1/z.

Face / polygon record (64 bytes): +0 material (u16), +2 16-bit colour (565), +4 four (u, v) pairs
(the first repeated after the last used), +0x2c four u16 vertex indexes (first repeated, then
0xffff; 0xea60 = unused), +0x36 texture / special index, +0x38 flags (bit 0: may be type 0xd,
bit 1: depth bias / clip), +0x3a material copy, +0x3c colour expanded (each 5/6-bit field << 3).

Render node (24 bytes, pool counted by [0x655d14], lists per depth bucket at 0x655d38): face,
depth, next, verts, texture handle (special faces) or 0, variant or -1.

Clip vertex (28 bytes): screen x, y (int), light (int), u/z, v/z, 1/z, z. Lists of up to 8 with the
count at +0xe0.

Model node tree (0x464190): +0 first child, +4 parent, +8 next sibling, +0xc angles, +0x18 world
matrix (12 floats), +0x48 pivot.

Model (lighting, 0x422540): +0x10 normals (12 bytes each), +0x30 normal count.

## Camera (table at 0x5161c0, 0x238 bytes each, current [0x4dc0f8])

+0x68 matrix (4x3), +0x98 position, +0xd4 / +0xe4 / +0xf4 / +0x104 frustum planes (normal + d).

## Font (0x492b10 / 0x493490 / 0x493100)

+0x18 glyph pixels per character (256 pointers), +0x416/+0x418 16-bit palette, +0x618 character
widths (256 ints), +0xa18 line height, +0xa1c line spacing, +0xa20 character spacing, +0xa38 /
+0xa3c GE texture present, +0x1244 texture handle, +0x1248 per-character uv (64 bytes each),
+0x5484 four allocations, +0x5494..+0x5499 gradient end / start colours (r g b bytes), +0x549a
palette (3 bytes per entry), +0x549e palette entry count. Glyph method: [[font] + 0x18].

## Sprite (menu, 0x43ede0)

+0 x, +4 y, +8 w, +0xc h, +0x14 mode, +0x18 scroll x (tile), +0x2c pixels.

## Track view object for 0x40ce60

See Track: the same fields; [ecx + 0x20] = the sector drawn around.
