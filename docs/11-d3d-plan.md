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

## Stage 3 results (2026-10-13) — high resolution, owner-confirmed
- One SDL_GPU device owns the swapchain (`host_gpu.cpp`); software frames are uploaded and blitted, D3D frames are presented from the render target. `RenderScale=auto|1..4` (auto = 4 on the dev Mac → 2560×1920).
- **Bug 1 — 3D in the top-left quarter:** vertex x/y were normalised by the render-target size instead of the guest size.
- **Bug 2 — text bars/underlines at scale > 1:** the half-pixel shift that maps D3D's integer pixel centres onto Metal made flat quads cover an extra half original pixel right/bottom, sampling neighbouring glyphs. Flat textured overlays (all vertices share rhw/z — HUD and text have rhw=1, z≈0) now stay on the original pixel grid and sample at the D3D pixel centre via screen-space derivatives; 3D keeps the shift through a per-draw vertex uniform.
- **Bug 3 — 16/9 mode still low-res:** in 16/9 mode the game renders into a separate **640×360 offscreen D3D target** and each frame does `Blt(primary [0,60,640,420] ← target)` instead of flipping (seen with `MOTORACER_D3D_DEBUG=1` Blt summary). The CPU Blt read the picture back at 640×360. Now an unscaled full-target Blt to the primary presents the GPU picture directly (`present_blt` → `present_placed`); with `Widescreen=auto` only the 16:9 band is shown (2560×1440 at scale 4), otherwise it is placed in the 4:3 frame. The target's guest copy is marked stale and read back on demand.
- (Retracted: a blurry paused frame was the 16/9 offscreen-target bug above, not a separate pause-screen problem — pause is sharp, owner-confirmed.)
- Debug aids: `MOTORACER_D3D_DEBUG=1` (flat-triangle classes, overlay/full-upload counts, Blt summaries), `MOTORACER_SNAPSHOT_DIR` now also dumps GPU frames at full resolution.

## Custom resolutions through the game's own SCREEN MODE option (2026-10-13)
- `EnumDisplayModes` callback `0x477160` keeps up to 50 modes (16-bit RGB only, stored at `0x5d8954 + 36·i`: masks, bpp, width, height, area, size class). It caps width/height at 640×480 **unless** `[0x4dc174] && [0x4dc06c] && ![0x4dc070]`.
- First attempt assumed this is satisfied in D3D mode — wrong, the user saw nothing above 640×480. Reasons:
  - `0x4dc174` has **no writer** anywhere in the binary (only this read) — a dead developer switch, always 0 in the shipped game.
  - The enumeration runs in the first DirectDraw probe (`0x475150`), *before* D3D init (`0x41e8f0`) sets `0x4dc06c` ("hardware D3D device present"; also set at `0x41f2bf`).
- Fix (`DD_EnumDisplayModes`): when the callback is invoked with `Renderer=d3d` and `CustomModes=1`, the runtime sets `0x4dc174=1`, `0x4dc06c=1`, `0x4dc070=0` for the duration of the enumeration and restores them afterwards. Nothing else reads `0x4dc174`, so the patch is local to the mode filter.
- Verified by log: `ddraw: game accepted 10 display modes: 320x240 512x384 640x480 800x600 1024x768 1152x864 1280x960 1400x1050 1600x1200 2048x1536`. The table's last field is a size class: area < 0x30000 → 0, == 0x30000 (512×384) → 1, larger → 2.
- We now enumerate 4:3 modes up to 2048×1536 (800×600, 1024×768, 1152×864, 1280×960, 1400×1050, 1600×1200, 2048×1536).
- `RenderScale=auto` is relative to the game's chosen mode (smallest multiple covering the display, ≤ 4, render target ≤ 8192 px). A game mode of 1280×960 gets 2×; 2048×1536 gets 1×.

### Rendering above 480 rows: engine tables overflow (2026-10-13)
- With 1024×768 unlocked, races showed only the HUD gauges: the game submitted **no 3D triangles**
  (`MOTORACER_D3D_DEBUG`: only flat HUD/backdrop quads). Bisected with E2E runs
  (docs/13-e2e-testing.md, `MOTORACER_MODES`): 3D target heights 449 (1024×600 16/9) and 480 work;
  539 (1024×720 16/9), 576 (1024×768 16/9) and 600 (800×600 full) fail — width is irrelevant.
- Cause: two static per-row tables sized for 480 rows, filled for every viewport row:
  - `0x62e468` row → framebuffer offset (`y*pitch`), 480 dwords, filled at `0x499e84`, `0x499f44`,
    `0x49aed7`, `0x49b096`; read by the rasterizers as `[y*4 + 0x62e460..0x62e474]` (rows y-2..y+3).
    Row 480 overwrites `0x62ebe8`, row 481 the pitch `0x62ebec`, then the object array at `0x62ebf0`.
  - `0x623bd8` row → span-list head (span buffer), 480 dwords, filled at `0x49a291` from the span pool
    at `0x60d3d0` (8000 × 8 bytes); read by the span inserters (`lea ecx, [eax + 0x623bd8]`) and the
    fill at `0x49b0e0`. Row 480 overwrites `0x624358`.
  - (`0x6d1070` / `0x65e1d8`, 480 × 8 bytes, are only touched by no-op constructor loops.)
- Fix (translator, `RELOCATED_TABLES` in `translate.py`): every absolute operand inside a table
  (and indexed operands up to 4/8 bytes below its start) is rewritten to a 4096-row copy in the
  unused guest range `EXT_LO..EXT_HI` = `0x00800000..0x01000000` (`runtime.h`). The neighbouring
  globals (`0x62e460` byte, `0x624358`, `0x62ebe8`...) keep their addresses. Verified by E2E races
  at 1024×768 (16/9), 1600×1200 (16/9) and the software renderer at 640×480.

### SCREEN MODE persistence (2026-10-13)
- The options loader reads `"ScreenMode"` (`0x4e7fb8`, at `0x42af41`) as the **size class** only
  (0/1/2) and maps it to an area (`0x12c00`/`0x30000`/`0x4b000`) at `0x4dc0cc`; the race start
  (`0x459370` → `0x474cf0`) looks the mode up by that area. So any mode above 640×480 came back as
  640×480 after a restart.
- Fix: translator hook `GAME_HOOKS` (`translate.py`) calls `game_hook_screen_mode_loaded()` right
  before `0x42b0a3` (after the class → area mapping). `runtime/src/game_hooks.cpp` restores the mode
  remembered in `<game dir>/recomp_state.txt` (`ScreenMode=1024x768`) when the class is 2 and the
  mode is in the table, and a per-frame tick saves the selected area whenever it changes.
- `RenderScale=auto` uses the display mode's own pixel density now and logs its inputs
  (`gpu: render scale ...`). On the owner's 2560×1440 pt @2× display a 900-row band gets 4×.
