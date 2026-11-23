# GPUs and drivers

The port was written for the Adreno 650 with Qualcomm's driver (Retroid Pocket 5 / Flip 2) and has since been made to
run on other phones. Each section lists what that family needs and why. The README's switch table has the switch for
each workaround.

**Tell drivers apart by `driverID`** (`VkPhysicalDeviceDriverProperties`), not by the device name. Some Turnip builds
for the A8xx report Qualcomm's own name (`Adreno (TM) 830v1`). The port used to match on "Adreno" and treated them as
Qualcomm's driver, which turned bindless arrays off and cost every shader its fixed sampler: no shadows and wrong
lighting on the A8xx. `qualcomm_7xx_stock()` in `program.cpp` now checks `VK_DRIVER_ID_QUALCOMM_PROPRIETARY` first.

## Adreno, any driver

- **Tiler:** each render pass moves its attachments through tile memory (GMEM). Splitting passes is expensive, so the
  executor avoids it (`TilerOptions` in `executor.cpp`): a full clear becomes the next pass's `loadOp`, rebinding the
  same targets doesn't split, a barrier inside a pass only splits it when needed, `STORE_OP_NONE` for what a pass
  doesn't write, the render area is only what the draws cover, and depth/stencil has no `MUTABLE_FORMAT`, so UBWC
  compression stays on. Each of these has an off switch.
- **LRZ** (early depth rejection) is off for EQUAL depth tests, which is MH Rise's whole colour pass; see `fast_depth`.
- **Memory:** "GPU memory" is system RAM. One Vulkan allocation per texture exhausted the driver at about 1,100, so
  images are sub-allocated from big blocks.
- **A 4-byte `vkCmdFillBuffer` before and after every dispatch:** without it the GPU hung (DEVICE_LOST) loading the
  title screen in 6 of 7 starts. The exact cause was never found; full barriers weren't enough
  (`no_compute_fill`).
- **No conservative rasterisation** in Vulkan. MH Rise turns it off at the start of each list anyway.
- A pixel shader that reads a generic the previous stage doesn't write won't link, so only what's written is declared.
- **Geometry shaders** that use warp operations get Eden's emulation, which is slow on Adreno.

## Adreno 7xx / 8xx with Qualcomm's driver (512.8xx)

- **Rejected pipelines:** 512.863 on an Adreno 730/725 returns `VK_ERROR_UNKNOWN` for many of the scene's pixel
  shaders. There were two causes:
  - **Bindless arrays:** 94 % of the failing shaders indexed texture arrays, against 55 % of all shaders. So bindless
    arrays are off by default on this driver only. The 650's 512.746 handles them.
  - **Unused functions with variable pointers:** Eden declares a CAS-loop helper for 8- and 16-bit storage writes in
    every shader that writes a storage buffer, used or not (about 110 pixel shaders). The driver rejected every
    pipeline containing one; the hunter was black on the character select screen. The port removes uncalled functions
    and the capability they bring (`keep_dead_functions` turns that off).
- **Safe translation:** if a pipeline is still rejected, its stages are retranslated more conservatively: without
  NonUniform marking, then through spirv-opt, then without subgroup operations. The first one that works is
  remembered for the warm-up. The rejected SPIR-V is saved to `<data>/pipelines_fallidas/`.
- **Miscompiled control flow:** Qualcomm's compiler mishandles Eden's flag-based structuring of switches (BRX), giving
  NaN in the hunter's face skinning. Shaders with a BRX go through spirv-opt. Around an `OpUnreachable` it also
  mis-optimises, so that becomes an `OpReturn` (`eden-unreachable-qualcomm`).
- **Shuffles with a dynamic index** are emulated on Adreno (vendor 0x5143), as Eden does (`no_shuffle_qualcomm`
  uses the plain `OpGroupNonUniformShuffle` instead).

## Turnip (Mesa) on Adreno 7xx

Turnip compiles everything Qualcomm's driver rejects. Bindless arrays are on.

- **Bindless arrays matter for shadows:** with them off, the shadow sampler is lost (see [mh-rise.md](mh-rise.md)).
  On an A725 with Turnip, shadows show with bindless arrays on and disappear with them off.
- **The dotted black lines on the floor** came from general subgroup shuffles used for derivatives. They were seen on
  an A725 and an A8xx alike, and fixed by `eden-quad-butterfly`.

## Turnip on Adreno 8xx (Gen8 builds)

Turnip was still being brought up on the A8xx while this was written, and builds differ a lot.

- **Name:** some builds report `Adreno (TM) 8xx` like Qualcomm's driver. Detection by `driverID` handles that (see the
  top of this page).
- **Huge black triangles flashing every second:** overlays that the game partly overwrote were thrown away, and the
  parts outside came back from stale memory. Fixed by splitting overlays (`no_split_overlays`).
- **Blocky flicker on the scenery while turning the camera** (seen on a Snapdragon 8s Gen 4 with
  `Turnip_Gen8_V39`): rectangular tile-sized blocks for one frame. That tester had `fast_depth` on, which keeps LRZ
  running in the colour pass. It points to an LRZ or GMEM problem in that driver. The suggested tests were
  `fast_depth` off, then `TU_DEBUG=nolrz`, then `sysmem`; no answer yet.
- **Driver variables** (`env.NAME` lines in `mhr.ini`, Debug → Driver environment): `TU_DEBUG=sysmem,nolrz,noubwc` and
  `FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1` are there for testing. None of them was the fix for anything so far.

## Mali (ARM's driver and PanVK)

- **No BC texture formats:** BC1–BC5 (the game's colour textures, masks and normal maps) are decoded by a compute shader as they
  upload (`shaders/bc_decode.comp`, the same integer math as `bcn.cpp`): only the compressed blocks are uploaded and
  the CPU does no decoding (`no_gpu_bc` goes back to the CPU). BC6H and BC7 are still decompressed on the CPU (`bcn.cpp`,
  written from the format specification). `bc_on_cpu` tests these paths on an Adreno.
  Unsigned BC6H (skies, lighting) is stored as B10G11R11, half the size of RGBA16F.
  With `bc_astc` (Graphics → Textures as ASTC), BC1/BC2/BC3/BC7 are stored as ASTC 4x4 instead, which Mali reads
  natively: RGBA8 is 4 times the size of BC3/BC7 and 8 times BC1, ASTC 4x4 the same as BC3/BC7. The encode uses ARM's
  astcenc (fastest preset, 4 threads) the first time a level loads, and the result goes to the app's own files folder (`astc_cache`,
  keyed by the BC blocks): shared storage goes through FUSE, which costs time on every file opened. Levels that are
  still all zeros (not streamed in yet) become one-colour ASTC blocks, with nothing encoded or cached. The idea came from the mesa-wrapper that transcodes BC to ASTC for Winlator; the code is the port's
  own plus astcenc. The idea came from the bionic
  Vulkan wrapper, which decodes BC on the GPU for Winlator; the shader here is the port's own.
- **Subgroups of 16,** half a Maxwell warp:
  - **With ARM's driver,** subgroup operations are off. Eden emulates votes as if each invocation were its own warp.
    With them on, ARM's driver stalls the GPU: tested again after the warp barriers fix on a Dimensity 720 (r32p1), a
    bloom pass took seconds per frame. They stay off there.

## Old drivers (Vulkan 1.1)

ARM's r32 driver on a Dimensity 720 (Mali-G57) lacks `VK_KHR_draw_indirect_count`, `extended_dynamic_state`,
`depth_clip_enable`, `vertex_attribute_divisor`, `sampler_filter_minmax`, `dynamic_rendering` and vertex-stage
stores. The one that mattered: without draw-indirect-count, MH Rise's multi-draws (almost the whole main view) were
skipped and only the sky showed. Those draws are now all drawn, ignoring the GPU's count (`no_count_fallback`), and without the
`multiDrawIndirect` feature each draw gets its own call. Check the log for "multi-draw no soportado" or "sin dibujar"
numbers in the millions on any new old driver.

That driver also has no `multiDrawIndirect` (each multi-draw becomes one indirect call per draw) and no
`fillModeNonSolid` (point and line polygon modes are drawn filled, since they would make an invalid pipeline).
  - **Quad swaps stay on regardless** (`no_quad_derivatives` turns them off). With subgroups off, Eden's fallback for
    `SHFL.BFLY` returned each pixel's own value, so the game's hand-made derivatives in about 80 scenery pixel shaders
    were all zero on ARM's driver: the same loss that left PanVK without ground. A quad is 4 pixels on any GPU, so
    quad swaps are safe with 16-wide subgroups (cache key version 5).
  - **With PanVK,** they're on (`short_subgroups`). Without them, Eden's single-lane fallback loses quad derivatives
    and the ground was missing.
  - **Warp-synchronous compute** (32 threads sharing memory with no barrier) races on 16-wide subgroups, so the port
    adds barriers in the main flow (`no_warp_barriers`). This fixed a GPU hang while mining or attacking on a
    Dimensity 8300 (Mali-G615).
- **Integer vertex attributes** (bone indices) declared as float get converted by Mali (5 → 5.0), which stretched
  skinned characters into spikes. They're declared as integers (`no_int_attribs`).
- **Shared memory** is 32 KB, against the Switch's 48 KB, so it's clamped to the device's limit.
- **`OpUnreachable` after a switch** becomes `OpReturn` here too, in the skinning shaders (`no_return_mali`).
- **A Mali-G615 hung on every frame** on the BRX control flow Eden builds for switches. Graphics shaders with a BRX now
  go through spirv-opt too (`no_spirv_opt_brx`).
- **Phones that slow down apps** can be tried with the AnTuTu or Ludashi package builds. Some keep clocks up for
  benchmarks.

## PowerVR

- **No BC formats** either (same CPU decompression).
- **The shader compiler crashes building pipelines on several threads at once**, so they're built on one thread
  (`single_thread_pipelines`; `parallel_pipelines` overrides that).

## Low memory

Phones with less than 5 GB of RAM (or `low_memory`) keep the spare staging chunks and buffer mirrors at half. The
per-frame arenas stay the same size: with less, the factory scene fell to 1 fps.
