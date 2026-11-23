# Third-party notices

This project is licensed under GPL-3.0-or-later. It uses or includes the following third-party components.

| Component | Use | License |
|---|---|---|
| [Eden](https://git.eden-emu.dev/eden-emu/eden) | Shader recompiler (`src/shader_recompiler`, `src/common`), fetched by `tools/setup.sh` and patched with `tools/patches/` | GPL-3.0-or-later |
| [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) (via Eden) | Frame-generation integration in `runtime/src/nnsdk/nvn/lsfg/` (loads the user's own `Lossless.dll`; no Lossless Scaling code or shaders are included) | GPL-3.0-or-later |
| [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | Upscaling shaders (`runtime/src/nnsdk/nvn/shaders/`, compiled into `spirv.h`) | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | SPIR-V optimizer, built from the copy shipped in the Android NDK | Apache-2.0 (see `runtime/recompiler/LICENSE.SPIRV-Tools`) |
| [sirit](https://github.com/eden-emulator/sirit) | SPIR-V emitter used by the shader recompiler | BSD-3-Clause |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | SPIR-V grammar headers | MIT-style (Khronos) |
| [{fmt}](https://github.com/fmtlib/fmt) | Formatting library used by Eden's code | MIT |
| [Boost](https://www.boost.org/) (headers) | Used by Eden's shader recompiler | BSL-1.0 |
| Android NDK `native_app_glue` | NativeActivity glue | Apache-2.0 |
| [libadrenotools](https://github.com/bylaws/libadrenotools) and liblinkernsbypass | Loading the user's own Adreno driver (`runtime/external/libadrenotools`, unmodified) | BSD-2-Clause, Copyright (c) 2021 Billy Laws (text in its `LICENSE`) |
| [BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid) | The approach of `astc::from_bc1` (BC1 blocks turned into ASTC blocks directly: its `runtime/nvn/bc1_astc.c`); the code here is written from the ASTC specification | MIT, Copyright (c) 2026 Peiikai |
| [Arm ASTC Encoder (astcenc)](https://github.com/ARM-software/astc-encoder) 5.7.0 | Re-encoding BC textures as ASTC on GPUs without BC (`runtime/external/astcenc`, library sources only, unmodified) | Apache-2.0, Copyright 2020-2026 Arm Limited (text in its `LICENSE.txt`) |
| [volk](https://github.com/zeux/volk) | Vulkan function loader, so the driver can be the system's or the user's (`runtime/external/volk`, unmodified) | MIT, Copyright (c) 2018-2026 Arseny Kapoulkine (text in its `LICENSE.md`) |
| [ree-save-editor](https://github.com/kvasszn/ree-save-editor) | The save formats (DSSS, Citrus) that `SaveData.java` reads and writes, and the 128 Citrus curves in `android/assets/citrus_curves.bin` (the game's own constants, as that project lists them); the code here is written from the format notes and checked against real saves | GPL-3.0 |

Lossless Scaling and its `Lossless.dll` are not distributed here; the DLL must come from the user's own purchase.

AMD FidelityFX FSR 1 license (MIT):

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
