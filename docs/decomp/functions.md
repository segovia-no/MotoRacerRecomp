# Functions

By address. Names are descriptive (not from symbols). "Native" = rewritten in
`runtime/src/` (file in brackets) and checked against the translated x86.

| address | name | where |
|---|---|---|
| 0x403710 | mat_identity | native_race_e.cpp |
| 0x403740 | mat_rot_translate | native_race_e.cpp |
| 0x4039c0 | mat_copy | native_race_e.cpp |
| 0x403a80 | mat_mul | native_race_c.cpp |
| 0x40c7e0 | track_find_sector | native_geom.cpp |
| 0x40c8b0 | track_floor_height | native_race_c.cpp |
| 0x40c960 | track_point_on_sector | native_race_c.cpp |
| 0x40ce60 | draw_track | native_race_e.cpp |
| 0x40d3b0 | nearest_sector_per_type | native_race_e.cpp |
| 0x40ec30 | track_wrap | analysis |
| 0x40fc90 | vec_set | analysis |
| 0x40fcb0 | vec_length | analysis |
| 0x40fcd0 | vec_normalize | native_race_c.cpp |
| 0x40fd20 | vec_set | analysis |
| 0x40fd40 | mat_rotation | native_race_a.cpp, native_race_c.cpp, native_race_e.cpp |
| 0x40fe80 | mat_inverse | native_race_c.cpp, native_race_e.cpp |
| 0x40ff80 | vec_copy | analysis |
| 0x40ffa0 | vec_neg | analysis |
| 0x40ffc0 | vec_add | native_race_c.cpp |
| 0x410010 | mat_apply | native_race_a.cpp, native_race_c.cpp |
| 0x41c050 | tex_load_row24 | native_load.cpp |
| 0x41dde0 | rs_alpha_blend | native_render.cpp |
| 0x41e1b0 | exec_blend_ops | native_race_e.cpp |
| 0x41e2b0 | rs_wrap | native_render.cpp |
| 0x41e380 | rs_state_1a | native_render.cpp |
| 0x4221d0 | model_swap_materials | analysis |
| 0x422540 | model_vertex_light | native_race_e.cpp |
| 0x422b90 | poly_record_init | native_geom.cpp |
| 0x42f690 | file_buffered_entry | native_load.cpp |
| 0x43ede0 | sprite_blit | native_menu.cpp |
| 0x43f0f0 | menu_tile_background | native_menu.cpp |
| 0x4570d0 | vec_sub | analysis |
| 0x458b80 | draw_other_bikes | native_race_e.cpp |
| 0x458e20 | dir_angles | native_race_c.cpp |
| 0x45b4e0 | angle_blend | native_race_b.cpp |
| 0x461aa0 | bike_contact_frame | native_race_c.cpp |
| 0x462110 | draw_cockpit | native_render.cpp |
| 0x464190 | node_tree_matrices | native_race_e.cpp |
| 0x465d60 | bike_light_mode | analysis |
| 0x466a80 | bike_lean_update | native_race_b.cpp |
| 0x467930 | bike_speed_revs | native_race_b.cpp |
| 0x469c60 | bike_lean_points | native_race_c.cpp |
| 0x46b120 | bike_drive | native_race_c.cpp |
| 0x46bd00 | mat_angles | analysis |
| 0x46bdc0 | mat_look_at | native_race_a.cpp |
| 0x46bf80 | bike_step | native_race_c.cpp |
| 0x46c970 | track_in_window | native_race_a.cpp |
| 0x46cd20 | bike_pick_target | native_race_a.cpp |
| 0x46d270 | mat_angles2 | analysis |
| 0x46d490 | mat_set_translation | analysis |
| 0x46d4f0 | vec_copy | analysis |
| 0x46d540 | vec_scale | analysis |
| 0x46d5e0 | vec_dot | analysis |
| 0x46d610 | vec_cross | native_race_c.cpp |
| 0x46e8b0 | sphere_visible | native_race_e.cpp |
| 0x473a90 | pack_rgb | native_race_e.cpp |
| 0x485e50 | quad_to_render_list | native_race_d.cpp |
| 0x48e180 | bike_gear | native_race_b.cpp |
| 0x490220 | object_angle | native_race_c.cpp |
| 0x490b90 | palette_gradient | native_race_e.cpp |
| 0x492b10 | font_text_size | native_race_d.cpp |
| 0x493490 | font_draw_string | native_race_d.cpp |
| 0x493820 | font_print | native_race_d.cpp |
| 0x495bd0 | palette_to_pixels | native_race_e.cpp |
| 0x497080 | clip_screen | native_geom.cpp |
| 0x497e40 | clip_near | native_geom.cpp |
| 0x499460 | clip_slot_triangle | native_geom.cpp |
| 0x49a160 | render_begin_frame | native_race_e.cpp |
| 0x49b2c0 | transform_project | native_geom.cpp |
| 0x49b7f0 | draw_faces | native_geom.cpp |
| 0x49c590 | poly_setup | native_geom.cpp |
| 0x49ca70 | poly_setup_special | native_geom.cpp |
| 0x4af570 | render_flush_lists | native_render.cpp |
| 0x4b0f60 | segment_plane | native_race_c.cpp |
| 0x4b10c0 | plane_distance | analysis |
| 0x4b3048 | _ftol | analysis |
| 0x4b32d0 | crt_qsort_swap | native_race_e.cpp |
| 0x4b3400 | crt_strncmp | native_race_d.cpp |
| 0x4b8800 | crt_output | native_load.cpp |
| 0x4c5e9d | CFile_Read | native_load.cpp |

## 0x403710 mat_identity

0x403710: identity 4x3 matrix (thiscall: ecx = out); eax = out, edx = 1.0f

## 0x403740 mat_rot_translate

0x403740: rotation from three angles and a translation (thiscall: ecx = 4x3 out; args angles, t)
Per angle (radians) * [0x4d00d0] (_ftol) indexes the sine table 0x5fcb80: a = T[i & 0x3fff],
b = T[(i - 0x1000) & 0x3fff]. The rows as the x87 code computes them, with the float temporaries it
stores and reloads (t10 = b0 a2, t0c = a0 b2, t08 = b0 b2, t14 = a0 a2); row 3 = t.
eax = out, ecx / edx = the bits of t.z / t.x.

## 0x4039c0 mat_copy

0x4039c0 / 0x403a20: copy a 4x3 matrix (thiscall: ecx = dst; arg src; ret 4)
eax = dst, edx = the last dword (the x86's copy registers ebx / ebp are restored).

## 0x403a80 mat_mul

0x403a80: 3x4 matrix product (stdcall dst, a, b; ret 0xc)
dst = a * b for the game's 3x3 rotation + translation matrices (12 floats: rows of 3, then the
translation, which is a's translation through b's rotation plus b's). All twelve results are
computed before the first store (dst may be a or b). Leaves eax = dst, ecx / edx = the bits of the
last two floats (the x86's copy loop), x87 unchanged.

## 0x40c7e0 track_find_sector

0x40c7e0: track sector search (thiscall; this = {sectors, count}; start index, point, float *out)
From a start index (>= count: 0), walks the sectors (252 bytes, a plane at +0x40) until the point
changes side of a sector's plane (0x4b10c0: dot(normal, point) + d, call counter [0x709378]):
on the negative side it steps back (wrapping to count - 1) to the first sector whose distance is
not negative and returns it; else it steps forward ((i + 1) % count) to the last one before a
negative distance. *out (if not NULL) = the last distance computed in the loop. ~340 calls per
race frame, ~1,000 plane tests. Registers / x87 status as the translated code leaves them.

## 0x40c8b0 track_floor_height

0x40c8b0 (thiscall track; sector index, point; st0): the height of the sector's floor plane at
the point (plane at +0x80, or +0x90 when the point is below the plane at +0xa0), 0 without sectors
or for a vertical plane

## 0x40c960 track_point_on_sector

0x40c960: point on a sector's plane (thiscall this = {sectors, count}; out, index, point; ret 0xc)
count <= 0: out = 0. Else the segment from the point to the point raised by [0x4d02d4] (y) is cut
with the sector's plane at +0x80 (point on or above the plane at +0xa0, 0x4b10c0) or +0x90 (below).
Sectors are 252 bytes. Leaves eax = out, ecx / edx = bits of out.x / out.z.

## 0x40ce60 draw_track

0x40ce60: draw the track around a sector (thiscall: ecx = track view; sector, camera, out; ret 0xc)
Track view: sectors [+0] (252 bytes), sector count [+4], chunks [+8] (112 bytes, count [+0x10]),
range [+0x24]; [+0x20] = the sector. By the camera's view axis (the inverse of its matrix +0x68 applied
to (0, 0, 1)) against the sector's direction +0x40: n chunks ahead and 1 behind, n/2 each way, or 1
ahead and n behind, around the sector's chunk +0x3c. Each chunk with geometry: the camera matrix
(rotation 0 with the translation -position +0x98 (0x403740), times the camera's (0x403a80)), the
transform 0x49b2c0 and the faces 0x49b7f0. Then the track objects of the sectors cur .. cur + 4 * ahead
+ 4 and cur - 1 .. cur - 4 * behind - 4 (virtual draw [vtbl + 0xc] when [0x4dc098]), their indexes
into out. eax = the number of indexes. Callers read no other register.

## 0x40d3b0 nearest_sector_per_type

0x40d3b0: per surface type, the nearest sector around the current one (thiscall: ecx = track; out, n; ret 8)
Track: sectors [ecx] (252 bytes: centre x y z at +0, type mask +0xf8), count [ecx + 4], current [ecx + 0x20].
Over the 2n sectors from current - n (wrapped once), for each of the 7 mask bits a sector has, the
sector with the smallest squared distance (floats as the x86 rounds them; ties keep the first) goes to
out[bit]. eax = 2n, ecx = 2n (0 without sectors to scan), edx = the first sector.

## 0x40ec30 track_wrap

stdcall (pos; ret 4): track position wrapped into 0 .. [0x52c10c] - 1 (adds / subtracts the count).

## 0x40fc90 vec_set

thiscall (out; a, b, c; ret 0xc): out = (a, b, c); eax = out.

## 0x40fcb0 vec_length

thiscall (this = v): st0 = sqrt((x*x + y*y) + z*z).

## 0x40fcd0 vec_normalize

0x40fcd0: normalize in place (unless the length is [0x4d02d0]); 0x40fcb0: the length (st0)

## 0x40fd20 vec_set

thiscall (this = float[3] out; a, b, c; ret 0xc): out = (a, b, c) as dwords; eax = out. 0x40fc90 is the same function body.

## 0x40fd40 mat_rotation

0x40fd40: rotation matrix from three angles (thiscall: ecx = 4x3 matrix out, arg = angles)
Each angle (radians) * 4096 / (pi / 2) truncated (_ftol) indexes the 16384-entry sine table
0x5fcb80: a = T[i & 0x3fff], b = T[(i - 0x1000) & 0x3fff]. Rows 0..2 as the x87 sequence computes
them (including the float stores it reads back), row 3 = 0. eax = out, ecx = the bits of b2,
edx = the high half of the last _ftol.

0x40fd40: rotation matrix from three angles through the sine table [0x5fcb80] (16384 entries per
turn, index = _ftol(angle * [0x4d0300]); the cosine is the entry 0x1000 before), no translation

0x40fd40 (rotation from three angles, the sine table 0x5fcb80; translation 0), as native_race_a.cpp

## 0x40fe80 mat_inverse

0x40fe80: inverse of a rotation + translation (transpose, -R^T t)

0x40fe80: inverse of a rotation + translation (transpose, -R^T t), as native_race_c.cpp

## 0x40ff80 vec_copy

thiscall (this = dst; src; ret 4): copy 3 dwords; eax = dst.

## 0x40ffa0 vec_neg

thiscall (this = v; out; ret 4): out = -v (fchs on each float).

## 0x40ffc0 vec_add

0x40ffc0 (out = a + b), 0x4570d0 (out = a - b), 0x46d590 (out = v * (1 / s))

## 0x410010 mat_apply

0x410010: out = M * v + t (stdcall: out, v, M; 3x3 rows at M+0, translation at M+0x24)
Results: eax = out, ecx / edx = the bits of out.x / out.z (the x86 copies the result through them).

0x410010: o = m * v (rotation, then the translation m[9..11])

## 0x41c050 tex_load_row24

0x41c050 (texture file loader): the 24-bit pixel loop of a row, 0x41cdb6..0x41d087
Per pixel three bytes c1, c2, c3 (getc), optional colour-key fix-ups ([ebp+0x1c]: pure green
0/255/0 -> black, near-black lifted so it is not keyed), an alpha value (format with alpha bits:
0 for near-black, else 1, or for the format with alpha mode 4: 15, or 8 inside a rectangle; inside
another rectangle the translated code converts the colour with double-precision helpers), then
each channel rounded to its bit count and shifted into a 16-bit pixel. Rows that touch that
conversion rectangle, and 128-wide rows that skip every other source pixel, run the translated
loop. Exit state as the translated loop leaves it (0x41d08d).

## 0x41dde0 rs_alpha_blend

0x41dde0(enable, additive): alpha-blend render states; disabling when already off emits nothing

## 0x41e1b0 exec_blend_ops

0x41e1b0: Direct3D render states into an execute buffer (thiscall: ecx = writer, arg on; ret 4)
[ecx + 8] = the write pointer: a D3DOP_STATERENDER header (8, 8, count 3), then three (state, value)
pairs: on: (0x1b, [0x4e4f10]), (0x13, 5), (0x14, 6); off: (0xf, 0), (0x1b, 0), (0x15, [0x53bc10]).
[0x4e4f2c] = on, [0x4e4f3c] = -100. eax = the new pointer; edx = [0x53bc10] when off.

## 0x41e2b0 rs_wrap

0x41e2b0(on): render states 0x11 / 0x12 = 2 (on) or 1 (off), cached in [0x4e4f34]; off when [0x53be0c] is clear

## 0x41e380 rs_state_1a

0x41e380(value, force): render state 0x1a = value, cached in [0x4e4f38] unless force

## 0x4221d0 model_swap_materials

thiscall (model; on; ret 4): swaps the material ids of the model's faces (+0 of each 64-byte face, count [+0x2c]): on: 0x10 -> 0x15, 0x26 -> 0x27; off: back.

## 0x422540 model_vertex_light

0x422540: per-vertex light of a model (thiscall: ecx = model; ? , light (3), matrix (12), out; ret 0x50)
The light vector goes through the matrix's inverse (rotation transposed, translation -R^T t, applied
with the translation as the x86 does) and is normalized (unless its length is [0x4d0664]); for each of
the model's [+0x30] normals ([+0x10], 12 bytes): t = _ftol(K - (n . -l) * K) (K = [0x4d069c]),
_ftol((t + [0x4d06a0]) * [0x4d0680]) + 8, at most 31 (signed), << 13 into the light field (+0xc) of
the output vertices (28 bytes). Nothing without normals ([+0x10] == 0). Callers read no register.

## 0x422b90 poly_record_init

0x422b90: polygon record init (thiscall, no arguments, returns this)
Four (u, v) pairs at +4 are constructed by 0x403170, which only returns this; then the vertex
indexes +0x2c..+0x34 = 0xea60 (unused), +0x36 / +0x38 = 0, +0x3c = -1.

## 0x42f690 file_buffered_entry

0x42f690 for file handle h: the entry when the next byte is in its buffer, else 0

## 0x43ede0 sprite_blit

0x43ede0: sprite blit (thiscall, 4 stack args, ret 0x10)
ecx = sprite {x, y, w, h at +0..+0xc, mode +0x14, pixels +0x2c}; arg1 = mode (5: the sprite's),
arg2 / arg3 = percentages 0..100 that crop the sprite from the top / right. Clipped to the screen,
copied into the back buffer row by row in one of four ways (jump table 0x43f0d4):
  0 plain copy (rep movsd), 1 colour key 0 (pixels of value 0 skipped),
  2 / 3 every other pixel (the checker "stipple"), the start shifting by one pixel per row: plain /
  colour key. The sprite's pixels are created on first use (0x43eb00) and mode 4 is a tiled
  background (0x43f0f0): both stay translated (the native returns 0 before changing anything).
Callers read no register the function leaves (eax / ecx / edx are reloaded).

## 0x43f0f0 menu_tile_background

0x43f0f0: tiled background (thiscall, no stack args, plain ret)
The menu's scrolling background: [0x49d0f0] (ecx = [0x4dc0c0], two zero args) returns the back
buffer's pixels; all 480 rows get the tile row ((y + r) & 127) of the 256-pixel-wide tile at
[ecx+0x2c], from x = [ecx+0x18] & 127, 128 pixels repeated 5 times (640). The x86 has a plain
path and an MMX one (flag [0x4dbcf4]) that write the same pixels. ~600 KB per menu frame.

## 0x4570d0 vec_sub

stdcall (out, a, b; ret 0xc): out = a - b; eax = out.

## 0x458b80 draw_other_bikes

0x458b80: draw the other bikes (stdcall: skip list, its length; ret 8)
For bikes 1 .. [0x509bac] - 1 (0x3209 bytes each from 0x584421; [bike] = drawn this frame): an
active bike ([+0x68]) inside the camera's view (0x46e8b0, camera [0x4dc0f8] of the table 0x5161c0,
radius 512) is drawn by 0x462110 in the replay view ([0x4dc008] and [0x508834] == 1), else — unless
the skip list is given and holds none of the bike's sector [+0x31c1] — by 0x45f290 (counter
[0x4f0dc0]) and 0x461f70. The list scan is per bike (the translated cost: bikes x list length).
The callers read no register afterwards (eax / ecx / edx are not compared in check mode). The
translated body's GAME_HOOKS (cockpit render nodes, native_render.cpp) are called at the same points.

## 0x458e20 dir_angles

0x458e20: the angles of the direction a -> b (pitch from the horizontal length, yaw); straight
up / down: +-pi/2, 0, 0; a == b: 0

## 0x45b4e0 angle_blend

0x45b4e0: blend two angle sets (thiscall: ecx = out; [esp+4] = a, [esp+8] = b, [esp+12] = t; ret 12)
Sets are {int count; float xyz[count][3]}; out count = min(a, b). t <= 0 (by its bits): copy a,
t >= 1.0: copy b (rep movsd; nothing when out is that set); else each angle a + (b' - a) * t with b'
moved by 2 pi to within pi of a, the result wrapped into [0, 2 pi].

## 0x461aa0 bike_contact_frame

0x461aa0: a bike's contact frame (thiscall bike, no args)
Six points around the bike (+0x2ea9.., 12 bytes each) from its position (+0x30), orientation
matrix (+0xbc) and two offsets (+0x1b4 front, +0x338 back): the frame aims from the position at the
point 50 units ahead (0x458e20, 0x40fd40, inverted 0x40fe80) and spreads +-40 sideways; the middle
pair is the average of points 3 and 1, the last one of points 0 and 4 times [0x4d1018]. Each point is
put on the track (sector search 0x40c7e0, then 0x40c960) and lifted by [0x4d0f80] ([0x4d1020] for
points 2 and 5). With [0x52c234] and [+0x30c1]: four more points (+0x30ed..) around +0x30b5 from
the angle 0x490220 (+0x3085), their heights from the floor (0x40c8b0) + [0x4d0f60].
Callers reload eax / ecx / edx afterwards (check_abi).

## 0x462110 draw_cockpit

0x462110 at 0x458c19 and its return 0x458c1e: the render nodes it created

## 0x464190 node_tree_matrices

0x464190: world matrices of a node tree (thiscall: ecx = first node; recursive)
For each node of the sibling list ([+8]): the local matrix = rotation from the angles at +0xc about
the pivot +0x48 (translation R (-p) + p, 0x410010 / 0x40ffc0); its world matrix +0x18 = the local
one, under a parent ([+4]) combined with the parent's world matrix as the x86 orders the products;
then the children ([+0]). Callers read no register afterwards.

## 0x465d60 bike_light_mode

thiscall (bike; arg unused; ret 4): 0x1f without a track ([0x52c10c] == 0), else 0x10 + 0xf * (sector [+0xe0] == 0): the lighting / shadow mode for the bike model.

## 0x466a80 bike_lean_update

0x466a80: bike lean (thiscall: ecx = bike; no args; ret)
Lean speed [+0x2e9d] (scaled by the bike table, its minimum [+0x5092d8]), lean phase [+0x2e99] and
its sine [+0x2e91], the lean angle [+0x2ea1] turning toward the target (0x45ca00 - [+0x2ea5] +
[+0x2e39]) by the shortest way round, wrapped into [0, 2 pi]; [+0x2e8d] = the change, wrapped by
0x458da0 and clamped to +-[+0x5092a8]. dt clamped to 3..5 (by its bits).

## 0x467930 bike_speed_revs

0x467930: bike speed / revs (thiscall: ecx = bike; no args; ret)
Speed from the distance moved this frame ([+0x338..] - [+0x344..]) / dt; for AI bikes ([+8] == 0)
the acceleration [+0x120] (clamped -50..40) and the integrated speed [+0x118] (through
game_hook_ai_speed, the runtime's fractional carry), then the revs [+0x11c] from the gear ratio
(0x48e180) or, for the player ([+8] != 0 and [+4] != 0), from the engine table.
replay: check mode runs this after the translated version; the hook keeps state between calls, so
the native run replays the value the translated run stored instead of calling it a second time.

## 0x469c60 bike_lean_points

0x469c60: a bike's lean and front / back points (thiscall bike, no args)
T = [0x509398 + 292 * model] (the dirt bikes' flag). The look direction blends the heading +0x18c
(turned towards the bike's forward axis) with that axis by w (from +0x31d1, decayed by [0x581014],
and the speed ratio +0x124 / +0xb4); 0x46bdc0 builds its frame. T != 0: with +0x307d > 0 the front
point +0x1b4 swings around the back point +0x338 by sin(+0x307d) in the bike's frame; +0x2e55 = the
frame from the back to the front point. T == 0: the front / back points at per-model distances
(0x5092b0 table) along the look frame, put on the floor of their sectors (0x40c7e0, 0x40c8b0), and
+0x2e55 = the orthonormal frame along them (+0x307d > 0, a crash tumble, runs the translated body).
The lean +0x2e1d from the yaw between the look frame and the bike (0x490220 angles wrapped to
0..2pi, the difference to -pi..pi): T != 0 with [+8] and [+4], atan of the yaw by the speed (at most
50), clamped, smoothed with +0x2e21; else the yaw scaled and clamped, then divided by a speed term
(T != 0), or for the player's bike ([+8], [+4], [0x4e9834] not 1 / 2) by the steering scale
[0x4dc150] (at least 20 without +0x2e29) and shaped, or negated. Callers read no register afterwards.

## 0x46b120 bike_drive

0x46b120: a bike's heading and speed along the track (thiscall bike, arg dt; ret 4)
T = [0x509398 + 292 * model] (AI steering on), the aim point: the sector a few positions ahead
([0x4dc140] * 9 / 16) moved by +0x31d5 along its x axis. Order of the x86: the acceleration of the
target speed +0x1a4 (capped by +0xb4 scaled by +0x31d9) and 0x46cd20 (the bike it follows); on the
track (+0x30c1 == 0) the yaw towards the aim point (0x46bdc0, its inverse, 0x490220 angles wrapped to
0..2pi, the difference to -pi..pi) sets the steering +0x110 (AI: scaled, clamped to +-pi/4) and the
rotation 0x45cb00 (matrix by value); the forward vector +0x198 (= +0xd4 + +0xe0, normalized; the
heading +0x18c flips with it when they point apart); the speed +0xec towards the cap; the direction
to a second aim point; the sector search 0x40c7e0; the heading: AI bikes on the ground steer it by dt
and rebuild the orientation (+0xbc..+0xe8) around it, the others blend it towards the aim (+0x31d1);
the move pos += heading * speed * dt * [0x4d1010] unless [0x4dc0b4]; the sector search again.
Ends with that call's registers. Stack locals as the x86 keeps them where a callee reads them.

## 0x46bd00 mat_angles

thiscall (this = float[3] out; matrix; ret 4): Euler angles of a rotation matrix: c = sqrt(1 - m6^2); c below [0x4d0e50] (gimbal lock): (+-pi/2 by the sign of m6, 0, atan2(m4 / -m1 ...)); else out.y = atan2(m6, c), out.x = atan2(-m7 / c, m8 / c), out.z = atan2(-m3 / c, m0 / c). 0x46d270 is the same with ecx = matrix and the output as the argument.

## 0x46bdc0 mat_look_at

0x46bdc0: orthonormal frame looking from a to b (thiscall: ecx = 4x3 out; args a, b)
d = b - a (floats), normalized (0x40fcb0: sqrt((x*x + y*y) + z*z), unless 0); side = (d.z, 0, -d.x)
normalized by sqrt(d.z^2 + d.x^2); up = (-d.x*d.y, d.z^2 - (-d.x)*d.x, -(d.z*d.y)) normalized;
out rows: side, up, d, 0. Every float store the x87 code makes is rounded here as well.
eax = out, ecx / edx = the bits of d.y / d.z, x87 status = the last comparison with 0.

## 0x46bf80 bike_step

0x46bf80: one bike's movement step (thiscall bike, one stack arg; ret 4)
Order of the x86: 0x465fa0; 0x46b120(arg); the move against the track (0x40ca80 from the old
position, a hit puts the bike on the returned point, finds its sector, halves +0x1a4 / +0xec);
0x46c7a0 (contact test); the airborne flag +0x2e85; 0x4660f0(arg) unless [0x4dc0b4]; the floor
height under the bike (0x40c8b0) decides between staying in the air (+0x2db8 = 0) and landing: then
the vertical speed goes into +0x2e95, the heading (+0x18c / +0x194) is normalized, the bike is put on
the floor (minus a random amount for some bikes), and its orientation (+0xbc..+0xe8) is rebuilt from
the floor under two points ahead / behind (per-model distances at [0x5092b0 + 292 * model]). Then the
front / back points (+0x1b4, +0x338), 0x469c60, the lean +0x2e1d (scaled, clamped to +-pi/4, smoothed),
0x466940, 0x466a80, 0x461aa0, +0x2dc8 restored around 0x466d70, 0x4683e0, 0x467b30(1), 0x467930.
Ends with that last call's registers, as the x86.

## 0x46c970 track_in_window

0x46c970: is a track position in a window (stdcall: pos, from, ahead, behind)
Positions wrap at [0x52c10c] (0x40ec30: add / subtract the count until in range). Returns 1 when
pos lies between wrap(from - behind) and wrap(from + ahead), allowing for the wrap. ecx = pos;
edx = the count where the x86 reloads it, else unchanged.

## 0x46cd20 bike_pick_target

0x46cd20: an AI bike picks the bike it follows (thiscall: ecx = bike; arg = dt, ret 4)
Bikes are 0x3209-byte records (array 0x5812xx, count [0x509bac], index 0 = the player); this one's
track position +0x31c1, target bike +0x31d9, own index +0x2dd0. Scans the bikes for one in a
window ahead / behind on the track (0x46c970), measures distances (0x46c9f0, |float| _ftol),
speed comparisons ((int) * 0.2777 as float, the x87 three-way compare), then eases speed +0xec /
+0x1a4 and the gap +0x31d5. Rare branches call 0x46cb40 / 0x46ca10 (translated). The caller
(0x46b1d0) reloads eax / ecx / edx afterwards; the x87 status word is the last comparison's.

## 0x46d270 mat_angles2

thiscall (this = matrix; out; ret 4): Euler angles of a rotation matrix, as 0x46bd00.

## 0x46d490 mat_set_translation

thiscall (this = matrix; v; ret 4): matrix translation (+0x24..+0x2c) = v.

## 0x46d4f0 vec_copy

thiscall (this = dst; src; ret 4): copy 3 dwords; eax = dst.

## 0x46d540 vec_scale

stdcall (out, s, v; ret 0xc): out = v * s (s a float argument); eax = out.

## 0x46d5e0 vec_dot

stdcall (a, b; ret 8): st0 = (a.z*b.z + a.y*b.y) + b.x*a.x (dot product).

## 0x46d610 vec_cross

0x46d610: o = a x b

## 0x46e8b0 sphere_visible

0x46e8b0: is a sphere visible from a camera (thiscall: ecx = camera; centre, radius; ret 8)
Not beyond the draw distance ([0x51d1bc + 36 * [0x4dc098]] * [0x4d13b0], against the unrounded
distance to the camera position +0x98), then the four frustum planes (0x4b10c0: dot + d, counted at
[0x709378]) at +0xd4 (> r), +0xe4 (< -r), +0x104 and +0xf4 (both > r), r = radius * [0x4d13b8]
(stored over the radius argument, as the x86 does). eax = 1 when visible; ecx = the last plane.

## 0x473a90 pack_rgb

0x473a90: (r, g, b) bytes -> a pixel of the display format (stdcall, ret 0xc)
Each component: (c & 0xff) >> [0x4ee9b8 / bc / c0], then shifted left by [0x4ee9c4 / c8 / cc] as a
16-bit value, or-ed. eax = the pixel (upper half 0); cl = the last shift; edx = the blue part.

## 0x485e50 quad_to_render_list

0x485e50: a textured quad (or triangle) into the render list (stdcall, 10 args, ret 0x28):
  texture word, 4 vertices (else 3), texture-map word, uv[4][2] (or NULL), xy[4][2], 565 colour,
  depth mode, existing polygon record (or NULL: the next of 40 records at 0x5dbc90, 64 bytes),
  poly-setup argument, alpha (6 bits).
Fills the record (texture, mode words, uvs, colour via 0x423110 inline), the vertices in the ring
0x5dc698 (28 bytes each, 0xaa0 of them, index [0x4f0e4c] stepping by 4): z / 1/z constants, light
word, screen x / y; then 0x49c590 (polygon setup, native) puts it into a depth bucket.

## 0x48e180 bike_gear

0x48e180: gear from speed (thiscall: ecx = physics object, [esp+4] = int *gear or NULL; ret 4)
The record's max speed k ([+0x5092c0], float) times per-gear thresholds against the speed [ecx+0xc8]
picks a gear (1..5, or 1..6 when [+0x509398] is 0) and its ratio; returns in st0 the speed / (ratio
* k), as float, clamped to 0..1 (> 1.0f by its bits, < 0 by compare). eax = the pointer, ecx = the
unclamped float's bits, edx = gear.

## 0x490220 object_angle

0x490220: an angle of an object (thiscall, no stack args; st0 = result)
From the sums of three vector pairs of the object: u = ([+0x18]+[+0x24], [+0x1c]+[+0x28],
[+0x2c]+[+0x20]), w = ([+0xc]+[+0x24], [+0x10]+[+0x28]); len = sqrt([0x4d1e48]) * |u|; below
[0x4d1e50] the result is +-pi/2 ([0x4d1e5c] / [0x4d1e60]) by the sign of u.x, else
atan2((w.x * u.y - u.x * w.y) / len, u.z / len). Locals rounded to float where the x86 stores them.

## 0x490b90 palette_gradient

0x490b90: a 32-step colour gradient into a palette range (thiscall: ecx = object; first, last; ret 8)
From the bytes (r, g, b) at +0x5497 to those at +0x5494 in 16.16 steps of ((end - start) << 16) >> 5
(unsigned, as the x86 shifts), 32 display pixels (0x473a90) into the table 0x60d1a0; then entries
first + 1 .. last + 1 of the object's 16-bit table at +0x416 take table[i & 31].

## 0x492b10 font_text_size

0x492b10: font text size (thiscall: ecx = font; text, int *w, int *h, single_line; ret 0x10).
Character widths [font + 0x618 + 4c] (> 0: plus the spacing [font + 0xa20]); '\n' starts a line
(height += [0xa1c] + [0xa18], rounded up to even when [0x624b14]) unless single_line, which stops
there. *w = the widest line, *h = the total height (starting at [0xa18]).

## 0x493490 font_draw_string

0x493490: font draw string (thiscall: ecx = font; ret 0x38, 14 args):
  0 uint16 **dst (line start in the surface, advanced), 1 pitch (pixels), 2..4 passed to the glyph
  call, 5 text, 6 flags (& 0x1f: 4 centre, 0x10 right-align within 7 = width), 8 x, 9 y,
  10 passed to the glyph call, 11 rect out (or NULL), 12 / 13 rgb byte triples (or NULL).
The glyph method [[font] + 0x18] gets pointers to the x / y argument slots and moves them.
Returns (eax) the last glyph call's result (1 before the first, 0 for an empty text).

## 0x493820 font_print

0x493820 (print a string on a surface: SEH frame, CString, heap buffer, surface locks) stays
translated: mostly orchestration around calls, little work of its own per call.

## 0x495bd0 palette_to_pixels

0x495bd0: a palette range to display pixels (thiscall: ecx = object; args first, last; ret 8)
With a palette ([obj + 0x549a], [obj + 0x549e] entries of 3 bytes) and last >= first: for i in
first..last, entry i % count through 0x473a90 into the 16-bit table at obj + 0x418 + 2 i.

## 0x497080 clip_screen

0x497080: clip a triangle against the screen rectangle (Sutherland-Hodgman)
stdcall (poly, verts): poly = a triangle record (+0x2c three vertex indexes, +4 three (u, v)
pairs), verts = transformed vertices (28 bytes). Clip vertices (28 bytes): screen x, y (int), the
light value (int, flags << 13), u/z, v/z, 1/z (floats), z (float, final pass). Lists of up to 8
(closed: the first vertex repeated at the end) in two buffers that swap after each pass, count at
+0xe0. Passes: x <= xmax + 1, y <= ymax + 1, x > xmin, y > ymin. A new vertex on an edge gets the
limit as its clipped coordinate, the other ints as _ftol(v + (n - v) * d / e) and the floats as
(n - v) * t + v, with d = limit - v, e = n - v, t = d / e (its +0x18 is left as it was). Returns
the final list (eax); its u, v are divided back by 1/z.

## 0x497e40 clip_near

0x497e40(a, b, c, verts, face, record, &count), stdcall: the triangle of the face's vertex slots
a, b, c (vertices of 28 bytes: x y z, light +0xc, screen x / y +0x10 / +0x14, rhw +0x18) against the
near plane z = [0x625774] ("behind": z below it or unordered, as fcomp / test ah, 1). Vertices on the
plane are appended to the pool (0x6d20d8, count [0x68b014]; their index = (pointer - verts) / 28,
unsigned) by 0x497d60(behind, front, out). Each resulting triangle becomes a 64-byte record
(material / texture / flags of the face, the face's 16-bit colour expanded to +0x3c, indexes with
the first repeated and 0xffff, uv pairs with the first repeated) unless all its screen points lie
beyond one edge (the x86's outcode sums, below); count + 1 per record. Returns the next record.
One vertex K behind (F1, F2 the next ones): (F1, F2, P1) and (F2, P2, P1), P1 / P2 on K-F1 / K-F2.
Two behind (F in front): (F, P2, P1), P1 on prev(F)-F, P2 on next(F)-F. uv of a split point:
((uF - uK) * (k / (zF - zK))) * (near - zK) + uK. Registers at the end as each x86 path leaves them.
0x497d60: the point of the edge behind -> front on the near plane, projected (rhw [0x4d1f4c] / near,
screen x / y offsets [0x60d3c4] / [0x624f88]); the light +0xc interpolated with an integer slope

0x497e40 called from the clipping path: its arguments and return address on the guest stack below
`esp` as the x86 pushes them, then the native splitter (MOTORACER_NATIVE=check: the hooked call)

## 0x499460 clip_slot_triangle

0x499460(a, b, c, verts, face, record, &count): the triangle of vertex slots a, b, c as a record
(material / texture / colour / flags of the face, index 0xffff after the closing repeat of a);
count + 1; returns record + 0x40 (ecx = &count, edx = the new count).

## 0x49a160 render_begin_frame

0x49a160: begin a frame's render lists (stdcall: keep, no_lock; ret 8)
Unless no_lock: lock the back buffer (0x476700 on [0x624afc]; failing: the x86's error message and
exit), [0x624f5c] = its pixels at the viewport origin, unlock again when [0x4dc170] (0x4767b0);
[0x4f1a20] = locked. Then, when locked or no_lock and not keep: the pool / record / node counters are
reset ([0x53469c] += the pool count) and the span table rebuilt: [0x68b008] entries of 8 bytes at
0x60d3d0 (0, [0x62e448] + 1, 0), pointers at 0x623bd8 ([0x65e1d4] counts them). Last 0x4099a0 on
[0x4dc0c4] when it holds something. Callers read no register afterwards.

## 0x49b2c0 transform_project

0x49b2c0: transform vertices to the camera, project to the screen
stdcall (matrix, src, count, flags), returns the output in eax (and edx).
  matrix: 3x4 floats (x, y, z columns per row; row 3 = translation); x columns are scaled by the
    viewport scale [0x6d20c4], y columns by [0x6d104c]
  src: count vertices, 3 floats each
  out: the next `count` entries of the transformed-vertex pool 0x6d20d8 (28 bytes each; the pool's
    used count [0x68b014] grows by count): x, y, z (floats), flags << 13, screen x, screen y (int,
    only written when z >= the near plane [0x625774]), 1/z (float)
  screen x = centre x [0x60d3c4] + x/z, screen y = centre y [0x624f88] - y/z, rounded per the x87
  rounding mode (fistp; the function only changes the precision control).

## 0x49b7f0 draw_faces

0x49b7f0 / 0x49c110: visible faces of an object -> polygon setup
thiscall (ecx = renderer), stdcall args (verts, faces, count, arg4):
  verts: the object's transformed vertices (28 bytes each, as 0x49b2c0 writes them)
  faces: count records of 64 bytes: +0 type, +0x2c..+0x32 four vertex indexes, +0x36 special
    index, +0x38 flags (bit 0: may be type 0xd, bit 1: depth bias)
Per face (0x49b7f0 skips type 0xd, 0x49c110 type 0xd with flag bit 0): [0x51a404]++; a reference
vertex — 0x49b7f0: arg4 ? the farthest : the nearest; 0x49c110: the farthest (x87 compares, NaN
rules included) — must be beyond the near plane. All four beyond it: screen-space test — outcode
counts against the screen rectangle reject faces entirely off one side, then the 2D cross
product (as float) must be > 1. Otherwise ("near" face, flags bit 0): 3D normal
(v1 - v2) x (v1 - v0) dotted with the reference vertex must be > 1. With [0x4dc06c] set and
[0x4dc070] clear, faces not entirely inside the rectangle get flags bit 1 (clip).
Visible faces: 0x49b7f0: 0x49c590(face, depth, verts, flags, 0), depth = reference z
(+ [0x4d1f94] with the bias bit); 0x49c110: faces whose special entry [0x4f9300 + 824 * index]
is set go to 0x49ca70(face, z, verts, arg4, flags), the others to 0x49c590(face, z, verts, flags, 0).

## 0x49c590 poly_setup

0x49c590: polygon setup — depth bucket, material, render node
thiscall, stdcall args (face, depth, verts, flags, mode). Bucket: mode 1 -> 0, mode 2 -> 0x1f3f,
else _ftol(depth * [0x4d1f9c]) (negative: nothing drawn), at most 0x1f3e; min / max used bucket
[0x68b030] / [0x65e1d0]. face+0x3a = material (face+0): with a material override active
([0x4dc0c4] -> 0, not material flag 0x40, [0x4f1a18] clear) the material flags get the "near"
bit 0x20 (per [0x4dc098] mode, depth < 3072 / 6144) and lose 0x85, and face+0x3a =
[0x624b58 + flags*4]. Then (pool [0x655d14] < 0x1d4b) a 24-byte render node (face, depth, next,
verts, 0, -1) is pushed onto the bucket's list [0x655d38 + bucket*4]. Faces to clip (flags with
[0x4dc06c] set, [0x4dc070] clear) go through the clipper (clip_path).

## 0x49ca70 poly_setup_special

0x49ca70: polygon setup for "special" faces (from 0x49c110) — fast path
stdcall (face, depth, verts, variant, flags). Bucket _ftol(depth * [0x4d1f9c]) (negative: nothing),
at most 0x1f3e; min / max bucket; face+0x3a = material, or with an override active
([0x4dc0c4] -> 0 and [0x4dc098] <= 1) 0, or 0x2a for materials with flags 9; then (pool not full)
a render node (face, depth, next, verts, texture [0x4f9304 + 4 * (id * 206 + variant)], variant).
Faces to clip: clip_path.

## 0x4af570 render_flush_lists

0x4af570 walks the depth-sorted polygon lists and builds Direct3D execute buffers through the
execute-buffer object [0x624afc]. Its two per-vertex loops were ~30% of the game's x86 instructions
in a race (docs/22-hot-functions.md). The versions here do exactly
what the translated x86 does — same bytes in guest memory, same registers and stack slots where
the following code reads them — so they can be checked against it at run time:
  MOTORACER_NATIVE=0      translated code only
  MOTORACER_NATIVE=check  run both on the same input and log any difference
  (default)               native

## 0x4b0f60 segment_plane

0x4b0f60: segment / plane intersection (thiscall plane, stdcall out, p1 (3 floats), p2; ret 0x1c)
out = p1 if p1 and p2 are within [0x4d2090] on every axis or the segment is parallel to the plane
(n . d == [0x4d2094]); else p1 + d * -(n . p1 + n.w) / (n . d), d = p2 - p1 (counted in [0x709378]).
Leaves eax = out, ecx / edx = bits of out.x / out.z, the x87 status of the last compare.

## 0x4b10c0 plane_distance

thiscall (this = plane; point; ret 4): st0 = ((n.y p.y + n.z p.z) + n.x p.x) + d; counts [0x709378].

## 0x4b3048 _ftol

MSVC CRT _ftol: st0 -> edx:eax truncated (the translator inlines it).

## 0x4b32d0 crt_qsort_swap

0x4b32d0: swap n bytes of a and b (cdecl a, b, n) — the sort's swap
eax = 0 after a swap (unchanged when a == b), ecx / edx = the ends.

## 0x4b3400 crt_strncmp

0x4b3400: strncmp(s1, s2, n), cdecl (MSVC CRT: repne scasb for the length within n, repe cmpsb).
eax = ecx = -1 / 0 / 1 (unsigned bytes), edx untouched.

## 0x4b8800 crt_output

0x4b8800: the CRT's _output(FILE *stream, const char *format, va_list args)
The engine behind sprintf / vsprintf (and MFC's CString::Format): ~8% of the translated game code
in a race (HUD strings, a per-blit debug trace). Native for the conversions whose output is the
C standard's: c s d i u o x X and %%, with flags, width / precision (incl. *), h / l (32-bit);
a NULL %s prints "(null)" as MSVC does. Each conversion goes through the host snprintf. Floats,
%p, %n, wide characters, I64 / L / F / N and streams that would need _flsbuf run the translated
code (decided before anything is written). Like the CRT, the format and argument pointers (its
parameters, on the stack) end up past what was used; eax = characters written.

## 0x4c5e9d CFile_Read

0x4c5e9d: CFile::Read(buffer, count), thiscall (this+4 = Win32 handle), returns the count read
The track loader reads its .trk field by field (2-10 bytes, ~190,000 calls): the translated
version goes through the ReadFile import each time. Same result: eax = bytes read (0 for count
0), ecx = this, edx = the frame slot it passed to ReadFile. A failing read (the MFC version
throws) runs the translated code.

## 0x62ebf0 

0x62ebf0 (counter [0x65e1c8]; 0x423060 fills it from the clipped record and copies it to a stack
buffer), its indexes / uv pairs, the record's flags (+0x38), opaque alpha with [0x4dc1bc] or
[0x4dc1c0], and a render node on the bucket's list (no pool limit check here, as in the x86).
0x49ca70's nodes carry the face's texture handle [0x4f9304 + 4 (id * 206 + variant)] and the variant.
Registers at the end as the x86 leaves them (eax = ecx = record count; edx: the last bucket /
texture written, else the last clipped vertex count, else from the split calls).
