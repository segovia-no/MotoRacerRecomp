# Globals

| address | meaning |
|---|---|
| 0x4d0e54 | 0.0f |
| 0x4d0e58 | 2 pi (double) |
| 0x4d0e60 | pi (double) |
| 0x4d0e70 | 1.0f |
| 0x4d1f4c | rhw numerator (rhw = [0x4d1f4c] / z) |
| 0x4d1f9c | depth -> bucket scale |
| 0x4dc008 | replay view active |
| 0x4dc06c / 0x4dc070 | clipping on / off |
| 0x4dc098 | detail level (index of the per-detail table 0x51d1b8, 36 bytes each; also track objects on) |
| 0x4dc0b4 | race paused / frozen (no movement) |
| 0x4dc0c0 | the renderer object |
| 0x4dc0c4 | material override object (when [it] != 0) |
| 0x4dc0e4 | track object table (pointers, vtable draw at +0xc) |
| 0x4dc0f8 | current camera index |
| 0x4dc130 | per-vertex lighting on |
| 0x4dc140 | aim distance unit (sectors ahead: x 9 / 16 / 2 / 4) |
| 0x4dc150 | steering scale |
| 0x4dc170 | unlock the back buffer after locking |
| 0x4dc19c | -3DFX (only selects the D3D mode, 0x42bc85) |
| 0x4dc1bc | opaque-white flag read next to 0x4dc1c0, never written |
| 0x4dc1c0 | -Alpha: 4444 textures (also set with a PowerVR, 0x414e4c) |
| 0x4dc1cc | PowerVR card (SRCBLEND = SRCALPHA for translucent polygons, 0x41de45) |
| 0x4e4f10 | render state value (alpha ref) |
| 0x4e4f2c | blend ops on (0x41e1b0) |
| 0x4e9834 | input / control mode (1, 2: no lean shaping) |
| 0x4ee9b8..0x4ee9cc | pixel format: right shifts r g b (+0, +4, +8), left shifts (+0xc, +0x10, +0x14) |
| 0x4f0dc0 | bikes drawn counter |
| 0x4f1a20 | back buffer locked this frame |
| 0x4f9304 | special face texture handles [4 * (id * 206 + variant)] |
| 0x509398 | per-model dirt-bike flag (0x5092b0 + 0xe8) |
| 0x509bac | bike count |
| 0x51d1bc | per-detail draw distance (36-byte entries) |
| 0x52c108 | the track object (see structs.md) |
| 0x52c10c | track sector count |
| 0x52c228..0x52c230 | light vector |
| 0x5161c0 | cameras (0x238 bytes each) |
| 0x584421 | bikes (0x3209 bytes each) |
| 0x5fcb80 | sine table (16384 floats per turn) |
| 0x60d1a0 | 32-entry gradient palette (0x490b90) |
| 0x60d3c4 / 0x624f88 | screen centre x / y |
| 0x60d3d0 | span table (8-byte entries, pointers at 0x623bd8, count [0x65e1d4]) |
| 0x623bd8 | per-row span-list heads, 480 dwords (0x624358 is the next global: taller viewports overflow it; the recomp relocates it to 0x760010) |
| 0x624374 / 0x62e448 | screen x max / min |
| 0x6d1040 / 0x625784 | screen y max / min |
| 0x624afc | execute buffer / back buffer object |
| 0x624f5c | back buffer pixels at the viewport origin |
| 0x625774 | near plane z |
| 0x62ebf0 | triangle records from the clipper (64 bytes, count [0x65e1c8]) |
| 0x655d14 | render node count |
| 0x655d38 | render lists per depth bucket (0x1f40 buckets) |
| 0x68b008 | span table size |
| 0x68b014 | transformed vertex pool count |
| 0x6d20d8 | transformed vertex pool (28 bytes each) |
| 0x709378 | plane test counter (0x4b10c0, 0x4b0f60) |
