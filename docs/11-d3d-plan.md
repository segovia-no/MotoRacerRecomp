# Phase 6 (planned) — Direct3D path on Metal, higher resolutions

## What the game uses (static analysis, 2026-10-13)
- **Direct3D version 1** (DirectX 3/5 "execute buffer" API). Strings: `D3DUnLockExecuteBuffer`, `Error in D3DUnLockExecuteBuffer lpBufEnd OverFlow`, `DsiD3d\ComplInit..Create Viewport aborted`; source module `D:\Dsed\mr320\Dsi_d3d.cpp`.
- GUIDs referenced from code (the full dxguid table is linked, but only these are used):

| Interface | Referenced at (function) |
|---|---|
| `IID_IDirect3D` | `0x41e92a` (`0x41e8f0`, D3D init) |
| `IID_IDirect3DTexture` | `0x41afc0`, `0x41b583`, `0x41b73b`, `0x41b8c6`, `0x41ec28` |
| `IID_IDirectDraw2` | `0x41e80b`, `0x4598ad`, `0x45af8a`, `0x47520a` |
| `IID_IDirectDrawSurface2/3` | several (surface setup) |

  `IDirect3D2`, `IDirect3DDevice2`, `IDirect3DTexture2` are **not** referenced → no DrawPrimitive API; the device is obtained by `QueryInterface` on the back buffer with the GUID chosen during `EnumDevices`.
- Init sequence in `0x41e8f0` (vtable offsets → v1 methods): `DD->QI(IID_IDirect3D)`, `IDirect3D::EnumDevices` (+0x10), surface `GetSurfaceDesc` (+0x58), `Lock`/`Unlock` (+0x64/+0x80), `Surface->QI(IID_IDirect3DTexture)` → `IDirect3DTexture::GetHandle` (+0x10).
- Game strings `MaterialEau/Goudron/Sable/Herbe/Terre/Huile/BordRoute/Neutre` are track surface types (water, tarmac, sand, grass, earth, oil, roadside), not D3D materials.
- Command-line knobs relevant to D3D: `-D3D`, `-ZBuffer`, `-InfiniteZBuffer`, `-Alpha` (ARGB4444 textures), `-Pal16`/`-NoPal256` (palettized texture formats), `-TextureSquareOnly`, `-CorrectRatioTextures`, `-TextureVideoMem<n>`, `-Min/MaxTextureWidth/Height<n>`, `-NoTextureSizeMax256`, `-OpaqueBlackLevel<n>`, `-NoDither`. The GOG ini enables `D3D=true ZBuffer=true` in its launcher settings.
- D3D settings dialog (resource 237) offers 320×240, 512×384, 640×480 and a 16:9 option, plus Fog/Filter/Dither/Specular/Blend/ZEnable/AntiAlias toggles.

## Plan
1. **Interfaces** (COM framework from phase 4): `IDirect3D` (EnumDevices, CreateViewport, CreateMaterial, CreateLight, FindDevice), `IDirect3DDevice` v1 (CreateExecuteBuffer, Execute, AddViewport, GetCaps, BeginScene/EndScene, EnumTextureFormats, CreateMatrix/SetMatrix if used), `IDirect3DExecuteBuffer` (Lock/Unlock/SetExecuteData/GetExecuteData), `IDirect3DViewport` (SetViewport, Clear, SetBackground), `IDirect3DMaterial` (SetMaterial/GetHandle), `IDirect3DTexture` (GetHandle, Load). Advertise one HAL device with RGB color model, Z-buffer, perspective-correct filtered textures, alpha blending, fog, 16-bit render/Z depth; texture formats 565, 1555, 4444 (+ 8-bit palettized if asked).
2. **Execute-buffer interpreter**: walk `D3DINSTRUCTION`s on `Execute` — `PROCESSVERTICES` (expect `D3DTLVERTEX` copy), `TRIANGLE`, `STATERENDER` (texture handle, Z, blend, fog, shading, filtering), `STATELIGHT`/`STATETRANSFORM`/`MATRIX*` if used, `BRANCHFORWARD`, `EXIT`. Batch triangles by state.
3. **GPU backend: SDL_GPU (Metal)** with MSL shaders: textured + Gouraud + specular, alpha test/blend, vertex fog, depth buffer.
4. **Resolution scaling**: `D3DTLVERTEX` x/y are screen pixels for the game's 640×480 mode → render into an N× render target with scaled coordinates (geometry and textures stay the game's; only rasterization resolution rises). Optionally widescreen later.
5. **2D/3D mixing**: the game may still `Lock` the back buffer (HUD, menus) or `Blt` sprites. Blts become textured quads on the GPU; for direct `Lock` writes, download → diff on `Unlock` → upload only changed pixels as an overlay layer so the 3D stays high-resolution.
6. Runtime switch: `[Recomp] Renderer=software|d3d` and `RenderScale=N`; in D3D mode the game gets `-D3D` (and `-ZBuffer`).

## Method
Implement interfaces with the per-slot trap vtables (every unimplemented method names itself), run with `-D3D`, and fill in what the game actually calls — the same loop that brought up DirectDraw/DirectSound.

## Stage 1 results (2026-10-13) — `runtime/src/dx/d3d.cpp`
Enable with `[Recomp] Renderer=d3d` (or `MOTORACER_RENDERER=d3d` for development); the game then gets `-D3D -ZBuffer`.
Nothing is rasterised yet; the software path is unaffected when D3D is off.

**Bring-up sequence observed:** `DD->QI(IID_IDirect3D)` → `EnumDevices` (we offer one HAL device, DX5-size 204-byte `D3DDEVICEDESC`) → 640×480 16-bit Z-buffer surface → `BackBuffer->QI(IID_IDirect3DHALDevice)` = device → `EnumTextureFormats` (we offer RGB565, ARGB1555, ARGB4444, PAL8) → `CreateViewport` + `SetViewport(0,0,640,480)` → one 47 104-byte execute buffer. Textures: 8-bit palettized system-memory textures `Load`ed into video-memory copies, plus 16-bit ones (256×128, 256×256, 128×128, 32×32).

**PowerVR probe:** the game creates a 2×2 surface with `DDPF_FOURCC 'PVRC'` to detect PowerVR cards. Accepting it would select the PowerVR path; we now reject FOURCC surfaces with `DDERR_INVALIDPIXELFORMAT` like ordinary cards.

**Execute buffer layout (every execute):**
```
[0 .. N*32)            N D3DTLVERTEX (pre-transformed, lit; x/y in 640x480 pixels)
instructions:
  PROCESSVERTICES  COPY (flags 2), start 0, dest 0, count N
  BRANCHFORWARD    mask 0, value 0 -> always taken; offset measured from the branch
                   instruction start, lands on a fixed block at 0x4ca8
  ... STATERENDER x1, STATERENDER x13, TRIANGLE xK ... EXIT
```
No matrices, lights or transform states are used — the game does all T&L itself.

**Render states seen (intro + menu):** TEXTUREHANDLE (0 or a handle), TEXTUREADDRESS=CLAMP, TEXTUREPERSPECTIVE=1, ZENABLE=1, ZWRITEENABLE=1, ZFUNC=LESSEQUAL, FILLMODE=SOLID, SHADEMODE=GOURAUD, TEXTUREMAG/MIN=NEAREST|LINEAR, TEXTUREMAPBLEND=MODULATE, ALPHABLEND/ALPHATEST/DITHER/SPECULAR/STIPPLEDALPHA=0. Races may add more (alpha blending for shadows/smoke, fog) — the stats report every 10 s lists them.

**Stage 2 therefore needs:** TLVERTEX triangles with perspective-correct texturing (modulate by vertex colour), Gouraud shading, 16-bit depth test LESSEQUAL with write, nearest/linear filtering, clamp addressing; palettized (PAL8) and 16-bit textures uploaded from guest surfaces (re-upload on Lock/Unlock or Load); viewport Clear with the background material colour.

## Stage 2 results (2026-10-13) — `runtime/src/dx/d3d_gpu.cpp`
**Owner-confirmed: races render correctly through the GPU with transparency, HUD and name entry ("everything works flawlessly").** Native 640×480.

- SDL_GPU (Metal), MSL shaders compiled at startup. Clip-space `pos = (ndc·w, z·w, w)` with `w = 1/rhw` (perspective-correct). Pipelines cached per depth/blend/cull key, samplers per filter/address.
- Frame model: guest back buffer authoritative; GPU colour target uploaded from it before 3D after CPU writes, read back (RGBA8→RGB565) before any CPU access (`Lock`, `Blt`, `BltFast`, `GetDC`, `Flip`).
- Textures cached per surface with `Surface::version` / `Palette::version` invalidation; PAL8/565/1555/4444 → RGBA8.

### Bugs found while bringing it up
1. **`IDirectDraw::DuplicateSurface`** (slot 7) was missing — the game duplicates bike textures (`-NoDuplicateSurfaces` disables it). Duplicates share pixel memory (`Surface::storage` shared_ptr).
2. **Transparency mode:** without `-Alpha` the game blends with `SRCBLEND=DESTBLEND=INVSRCALPHA` (code at `0x41de4d`, chosen by the `-Alpha` global `0x4dc1cc`), giving black boxes. With `-Alpha` it uses standard `SRCALPHA/INVSRCALPHA` + ARGB textures. D3D mode now always passes `-Alpha` (the game's readme recommends it for the name-entry letters too).
3. **MODULATE alpha rule:** vertices carry colour `0x00RRGGBB` (alpha 0); D3D takes alpha from the texture whenever the texture *format* has alpha. Using the texture alpha only when < 1 made opaque HUD texels invisible.
4. **Metal abort on 0×0 texture descriptors** (seen once, during a menu `Flip`): invalid surfaces now draw untextured and are logged. SIGABRT/SIGILL/SIGFPE now go through the crash reporter.

### Render states in a race
SRCBLEND 5 (345k) / 6 (33k) / 2 (22); DESTBLEND 6 / 3; ALPHABLENDENABLE on for most sprite/HUD batches; ALPHATEST, FOG, SPECULAR unused.
