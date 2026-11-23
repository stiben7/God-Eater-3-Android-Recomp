# Fixes

Bugs the port has hit, what caused each one and what fixed it. Each entry gives the switch that turns the fix off,
where one exists, so the old behaviour can be compared.

## Graphics

**Dotted black lines along the floor's triangle edges** (every GPU, worst on Adreno).
The main scenery pixel shaders compute their own derivatives with `SHFL.BFLY` by 1 and 2. Eden translated those to
`OpGroupNonUniformShuffleXor`, which Vulkan doesn't guarantee to run for helper invocations, so pixels on triangle
edges read garbage. Butterflies by 1..3 in pixel shaders are now quad swaps, which always include helper invocations
(`tools/patches/eden-quad-butterfly.patch`, cache key version 3). Early on this was wrongly ruled out after counting
the wrong SPIR-V opcode: `OpGroupNonUniformShuffleXor` is 346, `QuadBroadcast` 365, `QuadSwap` 366.

**Endemic life, wirebugs and some lights invisible but still gatherable** (also on Eden).
The game's occlusion test writes its results on the GPU, copies them to a buffer and reads that on the CPU the next
frame. The port kept GPU writes on the GPU, so the CPU read zeros and culled everything small. GPU copies out of
GPU-written data are now returned to the game's memory (`no_readback_copies`, Graphics → Occlusion results). At
first they came back merged chunk by chunk against what the CPU had written, but the game rewrites that buffer after
reading it, so most results were dropped and the objects flickered. Now the buffer comes back whole. Turning off the
game's GPU occlusion and instance culling didn't help; that was the wrong end of the chain.

**No shadows and flat lighting on the Adreno 8xx with Turnip.**
The shadow map was drawn correctly but sampled with sampler 0 (no comparison) instead of the game's sampler 268. The
fixed sampler is ORed into the handle in the shader code, and only `eden-bindless-arrays` keeps it, with bindless
arrays on. Those were off because the Turnip build called itself "Adreno (TM) 830v1" and the port took it for
Qualcomm's driver. The driver is now detected by `driverID`.

**Huge black triangles flashing about once a second** (Adreno 830).
The game carves the same memory into differently sized GPU ranges from frame to frame. A new overlay that only partly
overlapped an older one replaced it, and the parts outside came back from stale game memory, so vertex data blew up
for a frame. Overlapping overlays are now split (`no_split_overlays`).

**Stripes and flickering triangles of shadow on the ground; decals fighting the terrain.**
The game's depth bias (`nvnCommandBufferSetPolygonOffsetClamp`) was ignored. It now reaches Vulkan as dynamic depth
bias, with the clamp when the device supports `depthBiasClamp` (`no_depth_bias`).

**The world missing, only the sky drawn** (Dimensity 720, Mali-G57 with ARM's r32 driver, Vulkan 1.1; fine with
PanVK on the same phone). MH Rise draws almost all of its main view with multi-draws whose count a compute shader
writes on the GPU. That needs `VK_KHR_draw_indirect_count`, which this driver doesn't have, so the port skipped them
(4.6 million draws in one session; "multi-draw no soportado" in the log). The shadow maps still had the village
because the shadow passes draw differently. Now those draws are all drawn, ignoring the count
(`no_count_fallback`). The count is the game's GPU culling, and a culled draw's arguments are still a valid mesh, so
this draws a few hidden things (about 7 % in the village). Using last frame's count instead flickered, because it lags
the arguments by a frame. Found by comparing a PanVK and a stock-driver log from the same phone,
after dynamic shadows, subgroups, GPU occlusion culling and the read-back buffers had been ruled out.

**Grass drawn as solid polygon cards.** Caused by the old `depth_gequal` debug switch, which made the colour pass test
GEQUAL instead of EQUAL. Cut-out meshes rely on EQUAL to skip the pixels their pre-pass discarded. Replaced by
`fast_depth`, which only changes meshes drawn in the pre-pass without a pixel shader.

**Things drawn black, and the hunter black on character select** (Qualcomm's driver 512.863, Adreno 730/725).
That driver rejects pipelines: those that index texture arrays (bindless arrays are off by default there), and those
containing Eden's unused variable-pointer helper (uncalled functions are now removed). See
[gpus-and-drivers.md](gpus-and-drivers.md).

**NaN in the hunter's face skinning** (Qualcomm's driver). Eden's structured control flow for a switch (BRX) was
miscompiled. Compute shaders, and graphics shaders with a BRX, go through spirv-opt (`no_spirv_opt_brx`).

**Skinned characters stretched into spikes** (Mali). Integer vertex attributes declared float were converted to float
by the driver. They're declared as integers now (`no_int_attribs`).

**Missing ground on PanVK.** Without subgroup operations, Eden's single-lane fallback loses quad derivatives. Subgroup
operations are on with PanVK (`short_subgroups`).

**Magenta clothes.** Texture streaming chooses mips from GPU-written data. With `use_return` on, everything the GPU
writes comes back and the mips load. It's off by default because of its cost.

## Hangs and crashes

**GPU hang while mining or attacking** (Dimensity 8300, Mali-G615). Breadcrumbs pointed at compute shader
`a5411674ebfff7ec`: 32 threads, shared memory writes in loops, the loop limits read back with no barrier. A Maxwell
warp runs in lockstep; Mali's subgroups of 16 don't, so half the group read counts the other half hadn't written yet.
Barriers now go in the main flow of single-warp compute shaders on GPUs with smaller subgroups (`no_warp_barriers`,
cache key version 4).

**DEVICE_LOST loading the title screen** (Adreno). A 4-byte `vkCmdFillBuffer` around each dispatch stops it. The real
cause was never found (`no_compute_fill`).

**DEVICE_LOST when large SSBOs were read.** 24 MB and 18 MB buffers were copied to staging for every draw that bound
them (about 270 MB a frame), and past 16 MB a staging chunk was cut short, so shaders read outside it. These buffers
now live in GPU mirrors, updated page by page.

**Driver failing at about 1,100 textures** (Adreno). One allocation per texture exhausted the driver. Images are now
sub-allocated from big blocks.

**Pipeline creation crashing on PowerVR.** Its compiler can't build on several threads at once, so it builds on one.

**Videos stuck on the first frames.** The video library times playback with `GetProgramTotalActiveTime`, which has to
advance. On a Snapdragon 7+ Gen 2, `AMediaExtractor` found no tracks at all; an FFmpeg demuxer is the fallback.

**Audio cutting out.** Wwise closes and reopens its output many times; the old queue threw away audio that was already
mixed each time. It's now one continuous queue (`audio_cola.h`).

## Performance

- **First-use freezes on Turnip:** every compute shader went through spirv-opt, which took 4.3 s for 4 new shaders the
  first time an attack used them (plus 1.5 s of their pipelines). That was a fix for Qualcomm's own compiler; on Mesa
  drivers only compute shaders with a switch (BRX) go through it now (`spirv_opt_compute` restores the old rule).
- **BC textures as ASTC on Mali** (`bc_astc`, off by default until tested): 2-8 times less texture memory traffic
  than RGBA8. The first load of each texture is slower (CPU encode); the cache in the data folder makes the next ones a
  file read.
- **BC textures on Mali and PowerVR:** BC1–BC5 are decoded on the GPU (`no_gpu_bc`), so loading and streaming no longer
  decode them on the CPU.
- **Shader stutter:** translations are cached in `spirv.bin`, pipelines are warmed up from `pipelines_estado.bin`, and
  the driver's pipeline cache is saved. MH Rise creates about 1,200 translations from about 250 distinct shaders per
  session; each one used to cost 2.2 ms with the frame stopped.
  The warm-up starts with the pipelines the last session used (`pipelines_recent.bin`), and any pipeline already
  recorded is made right away in NvnSubmit (up to 8 ms a frame) instead of skipping its draws while a background
  thread builds it: no more objects blinking in after loading a save. From IamWilmar's upstream v1.5.0.
- **Mirrors and write watching** instead of hashing or copying megabytes per submit.
- **Tiler options** (see [gpus-and-drivers.md](gpus-and-drivers.md)) to keep render passes whole on Adreno.
- **Dynamic resolution** through the game's own `ImageQualityRate`, only when the GPU is the limit.
- **Frame rate:** the game's own limiter reads MaxFps, so 60 fps sets `MaxFps` in `via.Application`. The game measures
  each frame, so lower limits keep normal speed.
- **Where CPU time goes:** in a clean log (no Detailed log, no breadcrumbs) on an A725, the port spends 25–32 ms per
  frame against 17–24 ms for the GPU. So the CPU side is the limit for 60 fps, and about 31–35 MB of buffer copies
  per frame is part of it. `perf_stats` gives the breakdown (textures, pipelines, descriptors, vertices) without the
  slow log.

## Input, setup and multiplayer

- **Two copies on one phone couldn't join each other.** They shared a console user and a UDP port. Now each data
  folder is its own user (`user_id.txt`); a busy port makes the second copy bind its own and map it (`ldn_net`).
- **Touch layout sometimes not saved.** It now saves when editing finishes and again with `commit()` when the app
  pauses.
- **Game files and data:** the game folder holds only `exefs`, `romfs` and the DLC. Saves, logs, the cache and
  settings go to the data folder the user picks.

## Tried, didn't help, removed

- **Skipping empty full barriers by default on Mali** (`tight_barriers`, a full barrier with nothing recorded since the
  previous one): the Dimensity 720 recorded 122 of them a frame, but the build that skipped them also lost patches of
  the ground. Suspected, not proven; back to opt-in.

Kept here so nobody tries them again without a new reason:

- **Pinning the subgroup size** (`VK_EXT_subgroup_size_control`), subgroups in Turnip geometry shaders, and a toggle
  for subgroups in compute: no difference.
- **A depth compression (UBWC) toggle in the menu** for the A8xx flicker: no difference. The flicker was the overlays.
  `no_ubwc` stays as a debug switch.
- **A second invariant-position pass:** `eden-invariant-position` already does it.
- **Ultrawide by reporting a wider display:** the game sizes its scene itself, so the picture only stretched.
- **"Wide view"** (patching the camera aspect at `main+0x74c1ff8`): the HUD stretched along with the picture. Removed.
- **PC post-effect switches through RenderConfig** (motion blur, depth of field, bloom, god rays, lens flare,
  chromatic aberration, film grain): the Switch game never reads RenderConfig.
- **FXAA, shadow cache, small object culling and effect SSR toggles:** the Switch game already has them the cheap way.
- **GPU occlusion and instance culling off:** didn't bring endemic life back; the readback did. Occlusion culling off
  didn't bring the world back on the Dimensity 720 either.

## Still open


- **Speckles or magenta and red blocks on the first loading screen** after choosing a save. Probably texture streaming.
- **Blocky one-frame flicker on an Adreno 8xx** with `Turnip_Gen8_V39` while turning the camera. Probably LRZ or GMEM in
  that driver; untested with `fast_depth` off.
- **Black screen on a Dimensity 9500:** the game opened `patch_001.pak` and never went on to `patch_002`. Most likely a
  broken or mixed update dump. The setup screen could check every patch file's size.
- **Effects resolution and AO switches** are experimental: whether their values change anything visible hasn't been
  confirmed.
- **CPU cost per draw** in the executor; see Performance above.
