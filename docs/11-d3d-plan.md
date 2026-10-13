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
