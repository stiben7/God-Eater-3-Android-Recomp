cmake_minimum_required(VERSION 3.22)
project(nier_runtime C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)
# Release (tools/build_apk.sh RELEASE=1, la de los jugadores): cada función y dato en su sección para que el enlazador
# tire lo que nadie usa, funciones idénticas plegadas (solo las que no se comparan por dirección) y sin tabla de
# símbolos (la dinámica, la que enlaza el juego, se queda). Sin LTO: con lo que el juego enlaza por nombre, sin probar.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-ffunction-sections -fdata-sections)
  add_link_options(-Wl,--gc-sections -Wl,--icf=safe -s)
endif()

# libnnsdk.so: nuestra reimplementación del SDK de Nintendo. Todo símbolo que
# define se enlaza al juego con prioridad sobre bionic y sobre los stubs.
add_library(nnsdk SHARED
  src/nnsdk/account.cpp
  src/nnsdk/cheats.cpp
  src/nnsdk/audio.cpp
  src/nnsdk/codec.cpp
  src/nnsdk/cxx.cpp
  src/nnsdk/diag.cpp
  src/nnsdk/fs.cpp
  src/nnsdk/hid.cpp
  src/nnsdk/libc.cpp
  src/nnsdk/mem.cpp
  src/nnsdk/movie.cpp
  src/nnsdk/oe.cpp
  src/nnsdk/swkbd.cpp
  src/nnsdk/time.cpp
  src/nnsdk/nvn/bcn.cpp
  src/nnsdk/nvn/astc.cpp
  src/nnsdk/nvn/driver.cpp
  src/nnsdk/nvn/perf_hint.cpp
  src/nnsdk/nvn/game_tweaks.cpp
  src/nnsdk/nvn/buffer.cpp
  src/nnsdk/nvn/device.cpp
  src/nnsdk/nvn/cmdbuf.cpp
  src/nnsdk/nvn/count_check.cpp
  src/nnsdk/nvn/formats.cpp
  src/nnsdk/nvn/executor.cpp
  src/nnsdk/nvn/gpu_memory.cpp
  src/nnsdk/nvn/memory.cpp
  src/nnsdk/nvn/layout.cpp
  src/nnsdk/nvn/lsfg/chain.cpp
  src/nnsdk/nvn/lsfg/dll.cpp
  src/nnsdk/nvn/lsfg/framegen.cpp
  src/nnsdk/nvn/lsfg/motion.cpp
  src/nnsdk/nvn/state.cpp
  src/nnsdk/nvn/procs.cpp
  src/nnsdk/nvn/program.cpp
  src/nnsdk/nvn/spirv_cache.cpp
  src/nnsdk/nvn/queue.cpp
  src/nnsdk/nvn/texture.cpp
  src/nnsdk/nvn/vk.cpp
  src/nnsdk/nvn/window.cpp
  src/nnsdk/nvn/hud.cpp
  src/nnsdk/nvn/fsr.cpp
  src/nnsdk/os.cpp
  src/nnsdk/vi.cpp
  src/nnsdk/aoc.cpp
  src/nnsdk/album.cpp
  src/nnsdk/bcat.cpp
  src/nnsdk/crypto.cpp
  src/nnsdk/cxx_ios.cpp
  src/nnsdk/ec.cpp
  src/nnsdk/err.cpp
  src/nnsdk/friends.cpp
  src/nnsdk/hid_vibration.cpp
  src/nnsdk/image.cpp
  src/nnsdk/ldn.cpp
  src/nnsdk/libc_file.cpp
  src/nnsdk/libc_thread.cpp
  src/nnsdk/movie_ff.cpp
  src/nnsdk/nfp.cpp
  src/nnsdk/ngc.cpp
  src/nnsdk/nifm.cpp
  src/nnsdk/nsd.cpp
  src/nnsdk/os_fiber.cpp
  src/nnsdk/os_memory.cpp
  src/nnsdk/watch.cpp
  src/nnsdk/os_sync.cpp
  src/nnsdk/pctl.cpp
  src/nnsdk/prepo.cpp
  src/nnsdk/settings.cpp
  src/nnsdk/socket.cpp
  src/nnsdk/ssl.cpp
  src/nnsdk/util.cpp
  src/nnsdk/web.cpp
)
target_include_directories(nnsdk PRIVATE src)
# Recompilador de shaders de Eden (GPL-3): libnnsdk.so queda bajo GPL-3.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(recompiler)
target_link_libraries(nnsdk PRIVATE eden_shader)
# Vulkan por punteros (volk, desde nvn/vk.h): sin enlazar libvulkan.so, para que el driver pueda ser el
# del sistema o uno del usuario cargado con libadrenotools (nvn/driver.cpp). Las librerías de hooks de libadrenotools
# (libhook_impl.so, libmain_hook.so, libfile_redirect_hook.so, libgsl_alloc_hook.so) van en la APK junto a libnnsdk.so.
set(EXTERNAL ${CMAKE_CURRENT_LIST_DIR}/external)
if(NOT CMAKE_ANDROID_ARCH_ABI)  # (libadrenotools lo exige; el toolchain antiguo del NDK solo pone ANDROID_ABI)
  set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
endif()
add_subdirectory(${EXTERNAL}/libadrenotools adrenotools)
target_sources(nnsdk PRIVATE ${EXTERNAL}/volk/volk.c)
# ARM's ASTC encoder (Apache-2.0, unmodified): BC textures re-encoded as ASTC on Mali (nvn/astc.cpp). NEON only.
file(GLOB ASTCENC_SOURCES ${EXTERNAL}/astcenc/*.cpp)
add_library(astcenc STATIC ${ASTCENC_SOURCES})
target_compile_definitions(astcenc PRIVATE ASTCENC_NEON=1 ASTCENC_SVE=0 ASTCENC_SSE=0 ASTCENC_AVX=0 ASTCENC_POPCNT=0
                                           ASTCENC_F16C=0)
target_compile_options(astcenc PRIVATE -w -O2)
target_include_directories(astcenc PUBLIC ${EXTERNAL}/astcenc)
target_link_libraries(nnsdk PRIVATE astcenc)
target_include_directories(nnsdk PRIVATE ${EXTERNAL}/volk)
target_compile_definitions(nnsdk PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR)
target_link_libraries(nnsdk PRIVATE adrenotools android mediandk aaudio z dl)
# FFmpeg (LGPL, solo el demuxer MP4; tools/build_ffmpeg.sh): respaldo cuando AMediaExtractor no encuentra pistas en un
# vídeo. Sin esa carpeta se compila igual, sin el respaldo.
set(FFMPEG_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg-android)
if(EXISTS ${FFMPEG_DIR}/lib/libavformat.a)
  target_include_directories(nnsdk PRIVATE ${FFMPEG_DIR}/include)
  target_compile_definitions(nnsdk PRIVATE MHRISE_FFMPEG=1)
  target_link_libraries(nnsdk PRIVATE ${FFMPEG_DIR}/lib/libavformat.a ${FFMPEG_DIR}/lib/libavcodec.a
                                      ${FFMPEG_DIR}/lib/libavutil.a m)
else()
  message(WARNING "third_party/ffmpeg-android not found: building without the FFmpeg video fallback (run tools/build_ffmpeg.sh)")
endif()
# Su libc++ estática no se exporta: el juego debe usar una sola copia de libc++abi (la del loader).
target_link_options(nnsdk PRIVATE -Wl,--exclude-libs,ALL)

# Núcleo del loader: carga main, lo reloca contra libnnsdk/bionic/stubs y lo arranca.
add_library(loader_core OBJECT
  src/loader/run.cpp
  src/loader/nso.cpp
  src/loader/imports.cpp
  src/loader/crash.cpp
  src/loader/host_symbols.c
  src/loader/stub_dispatch.S
  src/loader/tls.cpp
  src/loader/tls_slow.S
  src/loader/probe.cpp
  src/loader/probe_slow.S
)
target_include_directories(loader_core PUBLIC src)
set_target_properties(loader_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# nier_loader: desde la terminal de la Flip, sin ventana.
add_executable(nier_loader src/loader/main.cpp)
target_link_libraries(nier_loader PRIVATE loader_core dl)

# libnier.so: la APK (NativeActivity), con ventana.
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(nier SHARED src/loader/android_main.cpp ${GLUE}/android_native_app_glue.c)
target_include_directories(nier PRIVATE ${GLUE})
target_link_libraries(nier PRIVATE loader_core adrenotools android log dl)
target_link_options(nier PRIVATE -u ANativeActivity_onCreate)
