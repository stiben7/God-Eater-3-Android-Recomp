# How the port works

The game's own ARM64 code runs on the phone's CPU. Nothing is emulated instruction by instruction: the port loads the
console executable into an Android process and supplies everything it imports (the console SDK, the NVN graphics API)
as native Android code.

```
APK
├── SetupActivity, SettingsMenu…  (Java)  folders, settings, touch controls, driver picker
└── NativeActivity
    ├── loader (libnier.so)       loads main + subsdk*, relocates, links imports, patches a few instructions
    └── libnnsdk.so               the console SDK the game imports, written from scratch
        └── nvn/                  NVN on top of Vulkan; Maxwell shaders → SPIR-V with Eden's recompiler
```

## The loader (`runtime/src/loader`)

- **NSO loading** (`nso.cpp`): the ExeFS modules are LZ4-compressed segments. They are mapped, relocated and their
  imports linked. Each import goes to, in order: `libnnsdk.so`, then bionic / compiler-rt / libc++abi, then the SDK's
  own data, and otherwise a named stub that logs the call (`imports.cpp`, `stub_dispatch.S`).
- **Instructions that can't run on Android** are patched: `svc`, the `tpidrro_el0` reads and the counters.
- **Thread-local storage** (`tls.cpp`): the game reads its thread-locals at fixed offsets from `TPIDR_EL0`, with
  Horizon's musl layout (its block starts at TP+16). On Android, TP+16..63 are bionic's slots (the stack canary at
  TP+40), so those accesses are rewritten to branch to stubs in a **patch area** just below `main`, which return the
  game's own TP. The slow path (a thread with no block yet) saves every register, since it can be reached from
  anywhere.
- **The patch area** is 64 KB of executable memory right before `main`, so a single `b` instruction can reach it. The
  TLS stubs use it from the start; the debug probes (`probe.cpp`) use a corner at the end. Anything else that needs to
  patch game code should put its stubs there before `tls::patch` runs, because the area is made read-only and
  executable afterwards. That's why a stub can't keep writable state inside the area: keep it in a global of the
  loader and put its address in a literal.
- **Crashes** (`crash.cpp`): registers, the address as `main+offset`, the last stubs and a backtrace go to the log.
  `SIGUSR2` takes a snapshot of every thread and lets the game carry on; the watchdog in `verbose_log` uses it.
- `android_main.cpp`: waits for the window, sends stdout to `<data>/log.txt`, starts the game on its own thread and
  turns Android input into Switch buttons (controller mapping, touch).

## The SDK (`runtime/src/nnsdk`)

Each `nn::` service the game imports is reimplemented with what the game actually does with it, read from the game's
calls and the values it checks. Game objects are opaque: the SDK keeps its real state in its own tables, looked up by
the address of the game's object. Highlights:

- `os*.cpp`: threads, mutexes, events, fibers (MH Rise uses them as coroutines), the application heap. The heap is a
  memfd, so the same pages can also be mapped where NVN's virtual memory pools need them.
- `fs.cpp`: mount points (`rom:`, `save:`, `aoc:`…) map to folders. `rutas.txt` (written by the setup screen) says
  where the user's ExeFS, RomFS and each DLC are.
- `hid.cpp`, `hid_vibration.cpp`: a single handheld controller, or a Pro Controller in docked mode.
- `audio.cpp` with `audio_cola.h`: AAudio, with a continuous queue, because Wwise closes and reopens its output many
  times during play.
- `movie.cpp`: the game's video library on top of MediaCodec. The FFmpeg demuxer (`movie_ff.cpp`) is the fallback for
  phones whose extractor finds no track.
- `ldn.cpp`, `socket.cpp`, `ldn_net.h`: Local Play over the internet, through hub servers or a direct connection. Two
  copies on one phone work if each has its own data folder (its own user, `user_id.txt`, and its own UDP port).
- The online services (account, friends, eShop, BCAT, NSD, SSL…) answer as a console with no network, using the same
  errors the SDK gives offline, so the game takes its offline paths.

## Settings (`ini.h`, `nvn/flags.h`, `Ini.java`)

Every setting is a line `name = value` in `<data>/mhr.ini`. In native code, `NVN_FLAG("name")` is a live switch: one
static per call site (so the name has to be a literal), read once and then refreshed by a thread that checks the file
about once a second. An empty file with the switch's name in the data folder still counts when the ini doesn't
mention it, which is handy over adb. Values (`wide_view`-style numbers, `env.NAME` driver variables) go through
`nnsdk::ini::read()`. One-shot requests (`trazar`, `profile_gpu`) are files that the runtime deletes once it has acted
on them, not settings.

The Java side writes the ini (`Ini.java`). It re-reads the file before every change, because the in-game panel writes
it too (the FPS counter).

## NVN on Vulkan (`runtime/src/nnsdk/nvn`)

The game gets each NVN function by name (`procs.cpp`). There are no NVN headers: every enum value and object layout
was deduced from how the game uses it, and each one has a comment with the evidence.

- **Addresses:** a memory pool's GPU address is its CPU address. Any address in a command can be read directly.
- **Command buffers** (`cmdbuf.cpp`, `commands.h`) record into the port's own command lists. The **executor**
  (`executor.cpp`) replays them into Vulkan on submit. The queue's state persists between lists, as in NVN.
- **Textures** (`texture.cpp`, `layout.cpp`): the game writes its textures already swizzled for the Tegra X1
  (block-linear, 64-byte by 8-row GOBs), so the executor deswizzles them when it uploads. Sparse textures for streaming
  live in virtual pools that the game maps 4 MB at a time.
- **Images** live in `VK_IMAGE_LAYOUT_GENERAL` and are sub-allocated from big blocks (`gpu_memory.cpp`): one Vulkan
  allocation per texture exhausted the Adreno driver at about 1,100 textures.

### Memory: staging, mirrors, overlays

The game's buffers are its own memory, and the GPU can't read that memory directly (the drivers don't offer
`VK_EXT_external_memory_host`). The port moves data three ways:

- **Staging:** small buffers a draw reads (uniform buffers, vertices, indices) are copied to a staging arena per
  submit. Vertex and index data that doesn't change is cached between submits.
- **Mirrors:** big buffers that shaders only read (MH Rise has 24 MB and 18 MB SSBOs that barely change) get a GPU copy.
  It's checked page by page once per submit, using write watching (`watch.cpp`): pages are made read-only, and the
  first write to each one is caught and noted. Only the pages that changed are uploaded.
- **Overlays:** whatever the GPU writes (SSBO stores, compute results, GPU copies) lives in a GPU buffer called an
  overlay. Later reads of that range use the overlay instead of the game's memory. When the game writes a range that
  only partly overlaps an older overlay, the old one is split rather than dropped (`no_split_overlays`). This fixed the
  A8xx black triangles.
- **Write-back** (`use_return`): copying overlays back to the game's memory so its CPU sees what the GPU wrote, as on
  the console where both share memory. Copying everything back costs too much (30 → 18 fps), so it's off. The one case
  that has to work is always handled: a **GPU copy out of GPU-written data** is how the game reads results back on
  the CPU, so those destinations are returned whole once their submit finishes (`no_readback_copies`). That's what
  makes endemic life visible (see [mh-rise.md](mh-rise.md)).

### Shaders

1. `program.cpp` gets the stage's Maxwell code and its control section, and `runtime/recompiler/nvn_shader.cpp`
   translates it to SPIR-V with **Eden's shader recompiler**. `tools/setup.sh` fetches Eden at a pinned commit and applies
   `tools/patches/eden-*.patch`:
   - `eden-bindless-arrays`: textures indexed from a constant buffer with a dynamic index, counted up to the 64 KB a
     constant buffer can hold. It also keeps the `& mask | sampler` that the game applies to a handle; without that
     the fixed sampler is lost (the shadow sampler, for one).
   - `eden-draw-parameters`: base vertex, base instance and draw ID from Vulkan's draw parameters.
   - `eden-gs-stream0`: geometry shaders that only use stream 0.
   - `eden-invariant-position`: the same position math in every shader, so a depth pre-pass and an EQUAL colour pass
     agree.
   - `eden-isgeneric`: a fix to which attributes count as generics.
   - `eden-quad-butterfly`: in pixel shaders, butterfly shuffles by 1..3 become quad swaps, which include the helper
     pixels on triangle edges.
   - `eden-unreachable-qualcomm`: an `OpReturn` in place of `OpUnreachable` where Qualcomm's driver mis-optimises.
2. Some modules then go through **spirv-opt** (`spirv_opt.cpp`): compute shaders always, and graphics shaders that
   contain a switch (BRX). That's because Qualcomm's compiler mishandles the flag-based control flow Eden builds for
   switches. Functions the shader never calls are removed (with the capability they bring), unless
   `keep_dead_functions` is set.
   On Qualcomm's and ARM's drivers every compute shader goes through it; on Mesa's (Turnip, PanVK) only those with a
   switch.
3. Translations are cached in `<data>/spirv.bin` (`spirv_cache.cpp`). The key covers the code, every translation option
   and `kKeyVersion` in `program.cpp`. **Bump `kKeyVersion` whenever a change alters the SPIR-V produced**, or old
   cached translations keep being used. A graphics stage missing from the cache is translated on a background thread
   (`translate_or_queue`); the draws that need it are skipped until it's ready, instead of the game freezing on it.
   Compute shaders are still translated where they're used. `no_async_shaders` turns this off.
4. Pipelines: built in the background, with a warm-up at start (`pipelines_estado.bin` lists what was used last time)
   and the driver's pipeline cache. If a driver rejects a pipeline, the stages are retranslated more conservatively
   (`no_safe_translation` turns that off), and the rejected SPIR-V goes to `<data>/pipelines_fallidas/`.

### Presenting (`window.cpp`, `fsr.cpp`, `lsfg/`, `hud.cpp`)

The game's 1280x720 (or 1920x1080 docked) window texture is scaled to the screen by FSR 1 (EASU, then RCAS), or a
plain blit with `no_fsr`. `picture_rect()` places it according to the aspect setting, with black around it when
letterboxed. LSFG 2x frame generation (with the player's own `Lossless.dll`) can add a generated frame between real
ones. The FPS counter and the settings panel are drawn by the CPU into a small buffer that is copied on top.

`window.cpp` also handles the frame rate (the game's clock, its MaxFps, the 20/25/40/45/60 limits) and dynamic
resolution (see [mh-rise.md](mh-rise.md)).

## The Android app (`android/java`)

- `SetupActivity`: the start screen. It asks for the game folder (only `exefs`, `romfs` and the DLC) and a data folder
  (saves, logs, cache, settings). Two copies of the app with different data folders can play Local Play together.
  `DumpScanner` checks the files: the build ID of `main`, the update's RomFS, and DLC IDs from the folder names.
- `SettingsMenu`: the in-game menu, with Display, Graphics, Performance, Touch, Combos, Controller, Multiplayer and
  Debug. It shows only the options for the GPU the game last ran on (`<data>/gpu.txt`, written by `vk.cpp`;
  `GpuInfo.java`); before the first start it shows them all. Display → Restore default settings deletes `mhr.ini` and the old switch files. The touch layout, combos and
  button mapping are kept in SharedPreferences and survive that.
- `TouchPad`, `Controls`: the on-screen controller, layout editing, show/hide per button and camera swipe.
- `DriverActivity`: adding a custom Vulkan driver (AdrenoTools zips, loaded with libadrenotools) and `Lossless.dll`.
- Package-name builds (`PACKAGE=com.antutu.ABenchMark tools/build_apk.sh`): some phones keep their clocks up for apps
  they recognise as benchmarks.

## Releasing

The port's own compute shaders (`runtime/src/nnsdk/nvn/shaders/*.comp`: FSR, BC decoding) are compiled into
`shaders/spirv.h` by `tools/shaders.sh` with the NDK's glslc; the workflows run it before building. A shader missing
from an old `spirv.h` just turns its feature off. `tools/build_apk.sh` (with `RELEASE=1` for `-O3`, stripped symbols and a non-debuggable APK). Pushing a `v*` tag runs
the `release` workflow, which builds the normal APK plus the AnTuTu and Ludashi package builds. `tools/setup.sh`
retries the Boost download, because GitHub sometimes answers 502 for a moment.
