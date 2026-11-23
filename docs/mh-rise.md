# Monster Hunter Rise, as the port sees it

Everything here is for **version 16.0.1** (`main` build ID `55d50ca1805e9c5bba508d601add852f255a32ba`). Addresses are
`main+offset`. Anything that patches or reads the game's code first checks the instructions it expects at those
addresses, and leaves the game alone if they don't match.

## How a frame is drawn

A trace of a village frame (Debug → Capture this frame) shows this order:

1. **Shadow maps:** a 512x512 D16 depth texture array (format 0x32, 32 layers). The four cascades are its first
   layers, each drawn as its own pass.
2. **Occlusion test:** a tiny 128x64 depth target with 4x MSAA. The game draws a box around each small object (endemic
   life, wirebugs, lights) with an indirect draw. Those draws have no vertex streams: the vertex shader reads the boxes
   from an SSBO, and a pixel shader **writes the object's "seen" flag to an SSBO**. A GPU copy then moves the flags to
   another buffer, and **the CPU reads that buffer on the next frame** to decide which objects to draw.
3. **Depth pre-pass:** about 225 draws with depth GEQUAL. **Reversed Z**: near is 1, far is 0.
4. **Colour pass:** the same meshes again, with depth **EQUAL** against the pre-pass and no depth write, into two
   targets (`R11G11B10F` scene colour and a second `0x29` target). It's a forward renderer: lighting, shadows, probes
   and decals are all sampled here.
5. **Transparent and effects passes:** particles, smoke and water, into a buffer reduced by `ReducedTransparentBuffer`.
6. **Post-processing:** a mip chain, bloom, auto exposure (compute), tone mapping. Then the UI at 1280x720, and the
   window texture.

What this means for the port:

- **Pre-pass plus EQUAL** only works if both programs compute exactly the same depth, hence
  `eden-invariant-position`. On Adreno, EQUAL also disables LRZ, the GPU's early depth rejection. `fast_depth` switches
  meshes the pre-pass drew without a pixel shader (solid ones) to GEQUAL. Cut-out meshes, the ones whose pre-pass
  discards see-through pixels (grass, leaves), keep EQUAL. GEQUAL on everything turns grass into solid cards: that was
  the old `depth_gequal` switch.
- **Hand-made derivatives:** the main scenery pixel shaders (`d788ba00b969e21d`, `059d4ff99c321e24` and about 80 more)
  compute their own slopes with Maxwell `SHFL.BFLY` by 1 and 2. A general subgroup shuffle in Vulkan may leave out
  the helper pixels on triangle edges, which drew dotted black lines along the floor's triangle edges.
  `eden-quad-butterfly` turns those into quad swaps.
- **Occlusion readback:** if the CPU never sees the GPU's flags, it reads zeros and never draws those objects, but
  gameplay still knows they're there (you can gather an invisible wirebug). The port returns readback copies whole
  (`no_readback_copies` turns that off). Whole matters: the game rewrites that buffer on the CPU after reading it, so
  merging chunk by chunk dropped most of the GPU's results, and everything flickered.
- **Depth bias:** the shadow pass uses `nvnCommandBufferSetPolygonOffsetClamp`. Ignoring it makes the ground shadow
  itself, which shows as stripes and triangles.

## Shaders worth knowing

- **Separate samplers with a fixed sampler in the code:** the game builds texture handles as
  `(word & 0x7fff) | 0x10b00000`, an index read from a constant buffer with a sampler ORed in. The shadow map uses
  sampler **268** (`| 0x10c00000`): comparison, `LINEAR_MIPMAP_NEAREST`. Without the OR, the texture is read with
  sampler 0, which has no comparison, so everything counts as lit and there are no shadows. Only `eden-bindless-arrays`
  keeps the OR, and only with bindless arrays on. In the census, 158 of 202 pixel shaders do this, including all 111
  that read shadows.
- **Bindless arrays in constant buffers:** reflection probes and decals index arrays at `c6[0xc83c]` (223 entries,
  64 bytes apart), `c9`/`c10`/`c12[0x5c/0x60/0x64 + 128·i]`, and so on.
- **Compute shaders have no NVN header** (the type is in the control section, `+0x714 == 5`). They read constant
  buffers 5..13 as global memory, at addresses NVN leaves at `c0[0x260 + 16·j]`, and their handles from `c2`
  (textures at `0x168 + 8·i`, images at `0x120 + 8·i`).
- **Warp-synchronous compute:** some compute shaders run in groups of exactly 32 threads (one Maxwell warp) and pass
  data through shared memory with no barrier, because a warp runs in lockstep. `a5411674ebfff7ec` reads loop counts
  back from shared memory this way. On a GPU whose subgroups are smaller (Mali: 16), the two halves race, the loops run
  on garbage and the GPU hangs. That happened while mining or attacking on a Dimensity 8300. `Options::warp_barriers`
  adds barriers in the main flow (`no_warp_barriers` turns that off).
- **The hunter's face** skinning (`793657b4f1e65631`) is a switch (BRX) that writes shared memory. Qualcomm's compiler
  produced NaN there until those shaders went through spirv-opt.
- **Auto exposure** keeps its state and an 8-sample history in a 36-byte SSBO that only compute shaders bind.

## Engine objects the port touches

RE Engine's reflection table names each property's getter and setter (`get_X` / `set_X`). In the registration code,
the name comes from an `adrp`/`add` to the string, and the setter thunk from `adrp x2`/`ldr x2` out of a relocated
slot. A thunk usually ends by branching to the real setter, or stores directly. That's how everything below was found.

| What | Where | Notes |
|---|---|---|
| `via.Application` | `*(main+0x12950348)` | Clock: `+0x430` base 60, `+0x434` MaxFps, `+0x41c`/`+0x424` dt. The limiter loads MaxFps at `main+0x6b04828`. Variable step: the game keeps its speed from 7.5 to 60 fps. |
| `via.render.Renderer` | `*(main+0x12950538)` | Fields below. |
| Shadow system | `*(main+0x12950580)` | `set_ShadowQuality` `main+0x78a0c58` → setter `main+0x7564f1c`, size at `+348`. |
| Mesh LOD | `*(main+0x12950568)` | `set_MeshQuality` `main+0x78a0d68`: float at `+60` or `+64` (by LOD mode). |
| RenderOutput update | `main+0x7936a24` | Multiplies the Renderer's `ImageQualityRate` in each frame. Loading sets the rate (`main+0x5389b50`). |

Renderer fields (Switch values from a log):

| Offset | Property | Switch default | Port setting |
|---|---|---|---|
| `0x1a78` | ImageQualityRate | 1.0 (0.1 while loading) | Dynamic resolution (steps 1.0 → 0.733) |
| `0x1a48` | TextureLoadLevelBias | 0 | `texture_bias_1/2` |
| `0x1a55` | DynamicShadowEnable (setter `main+0x7930a18`) | 1 | `no_dynamic_shadows` |
| `0x1a59`, `0x1a56` | SwingEnable, SwingOnShadowEnable (foliage sway) | 1, 0 | `no_foliage_sway` |
| `0x1a68` | ReducedTransparentBuffer | 2 | `effects_lower/lowest` (experimental) |
| `0x1a8c` | GeometryAOQuality | 1 | `no_ao` (experimental) |
| `0x1a5b` | FXAAEnable | **0** | none: already off |
| `0x1a53` | ShadowCacheEnable | **1** | none: already on |
| `0x1a1e` | SmallObjectCullingEnable | **1** | none: already on |
| `0x1a64` | EffectSSREnable | **0** | none: already off |
| `0x1a30` | SmallObjectCullingRatio | logged | |
| `0x1a44` | DefaultShadowLodBias | 0 | |
| `0x1a70`, `0x2180` | OcclusionCullingCut, OcclusionCullingResolution | | |

The Switch build was already tuned for weak hardware, so several PC options have nothing left to turn off. Check the
`game settings: … is X in the game` lines in the log before adding a setting.

**`via.render.RenderConfig`**, the object behind the PC's options menu (motion blur `+0xb2`, depth of field `+0xb3`,
god rays `+0xb4`, bloom `+0xb5`, lens flare `+0xb6`, chromatic aberration `+0xb7`, film grain `+0xb8`, AO `+0x4c`,
SSR `+0x54`, lens distortion `+0x58`, VRS `+0x5c`), exists in the Switch code. The Switch game never calls its
getters, though. Hooking them caught nothing in the village or on hunts, so changing it does nothing.

GPU culling: `UseGpuOcclusionCulling` is byte `+0x1d` of the object behind GOT entry `main+0x12674d08` (the slot
holds the address of a global, and the global points at the object; getter `main+0x72874`, setter thunk around
`main+0x72e84`).
`EnableGpuInstanceCulling` has getter `main+0x765cba8` and setter `main+0x765cbb8`, on the object behind
`main+0x12674d18`. Turning them off didn't bring the endemic life back; the readback above did.

## Resolution and the camera

- The handheld scene is 960x540 inside a 1280x720 output. The port's base resolution raises the rate up to 1280x720
  (`resolution_576/648/720`). Dynamic resolution lowers `ImageQualityRate` in steps of 64-pixel widths, when the
  average of the last 8 frames goes over 91 % of the 33.3 ms budget, and only if the CPU side fits (a CPU-bound frame
  doesn't get faster at a lower resolution).
- The game asks for the display resolution (`GetDefaultDisplayResolution`) but sizes its scene from its own fixed
  960x540 × rate. Reporting a wider screen changes nothing.
- The camera's aspect ratio is `width / height` stored at `+0x44` of the main camera, at
  `main+0x74c1ff8` (`fdiv s0, s11, s9` then `str s0, [x8, #0x44]`), the same spot the PC "any aspect ratio" mods
  patch. Patching it gives a wider view squeezed into the 16:9 image, which then has to be stretched along with the
  HUD. It was tried as "Wide view" and removed: the result wasn't good enough.

## Other game behaviour the SDK layer handles

- **Texture streaming reads GPU results:** the game decides which large mips to load from what its shaders write.
  Without that data coming back, it never asked for the large mips, and textures stayed at zero (magenta clothes).
  `use_return` brings everything back and fixes it, but costs too much (30 → 18 fps), so it's off by default.
- **Virtual memory pools:** texture memory is 4 MB chunks of physical pools mapped one after another into a virtual
  pool. The heap is a memfd, so the same pages appear at both addresses.
- **Sparse textures** come with block height `03 04` (8 GOBs, pitch to 16: 64 KB tiles).
- **Wwise** closes and reopens its audio output many times. **Videos** are MP4 (H.264 + AAC) inside the paks, decoded
  to NV12 into two textures.
- **The save** is a folder (400 MB + 8 MB journal in `control.nacp`). DLC indices are the title ID minus the app's,
  minus 0x1000.
- **Local Play** hubs use link protocol `0x5201`, so MH Rise only ever sees MH Rise hubs.
