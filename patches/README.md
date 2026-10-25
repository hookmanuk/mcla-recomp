# Patches to the ReXGlue SDK

Apply to ReXGlue v0.10.0 (`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`), from the SDK checkout:

```
git apply ../../patches/rexglue-v0.10.0-mcla.patch
git apply ../../patches/rexglue-minmax-blend-factor.patch
git -C thirdparty/FFmpeg apply ../../../../patches/rexglue-ffmpeg-ps5-config.patch
```

For VR (OpenXR, Windows), one more goes on top of those:

```
git apply ../../patches/rexglue-vr.patch
```

On Linux, two more go on top, from the same folder:

```
git apply ../../patches/rexglue-linux-procmap-query.patch
git apply ../../patches/rexglue-linux-wait-sleep.patch
```

They make page-protection lookups and multi-handle waits cheap on Linux. The Windows build needs neither. The PS5 build also applies `rexglue-linux-wait-sleep.patch` (`ps5/make_ps5.sh` does it): the PS5 runs the same POSIX threading code, where a wait shorter than 1 ms spun instead of sleeping.

The first was several files at first; they are combined now because several touch the same build files and would not apply independently. The second goes on top of the first (both change `src/graphics/vulkan/command_processor.cpp`). The build scripts apply all three, and add the second to an SDK checkout that was patched before it existed.

## rexglue-v0.10.0-mcla.patch

| Change | Files | Affects |
|---|---|---|
| Vblank timer resync: the guest tick count can step backwards, which made an unsigned difference wrap and fire vblank interrupts without end | `src/graphics/graphics_system.cpp` | all platforms |
| Per-frame counters kept in Release builds, and their CSV path taken from `REX_PERF_LOG_CSV` (the GPU plugin has its own copy of the logger state, so a path set by the host never reached it) | `CMakeLists.txt`, `src/core/CMakeLists.txt`, `src/system/CMakeLists.txt`, `src/core/perf/counter.cpp` | all platforms |
| Debug-only register lookup removed from every GPU register write in Release builds | `src/graphics/command_processor.cpp` | all platforms |
| PS5 platform: everything else. Platform detection, libc and libc++ differences, anonymous shared memory with a shared-object address reservation at a kernel-chosen base, 16 KiB host pages, signals without realtime signals, no X11/Wayland/RenderDoc, static runtime and GPU libraries, no desktop install rules | the remaining files | PS5 only, behind `REX_PLATFORM_PS5` or `if(PS5)` |

## rexglue-minmax-blend-factor.patch

The car reflection fix: cars came out white with a glow around them, worst at night. The Xbox 360's GPU applies the source blend factor in the MIN and MAX blend equations; Direct3D 12 and Vulkan ignore blend factors there. The game draws the light glows in the cars' environment map with `max(source x SrcAlpha, destination)`, so every glow became a solid square, and the paint mirrored those. With the patch the translated pixel shader multiplies by its own alpha in that case.

| Change | Files | Affects |
|---|---|---|
| System flags per render target, set when its colour or alpha blend is MIN or MAX with source factor SrcAlpha and destination factor One | `src/graphics/d3d12/command_processor.cpp`, `src/graphics/vulkan/command_processor.cpp` | D3D12; Vulkan (PC, PS5) |
| The multiply by the saturated alpha before the colour is written | `dxbc_translator.h`, `dxbc_translator_om.cpp`, `spirv_translator.h`, `spirv_translator_rb.cpp` (under `include/rex/graphics/pipeline/shader` and `src/graphics/pipeline/shader`) | D3D12; Vulkan (PC, PS5) |

Host render target path only (the default on both backends).

## rexglue-ffmpeg-ps5-config.patch

FFmpeg selects a configuration header per platform. This adds `config_ps5_x86_64.h`, generated from the Linux x86-64 one with `HAVE_MALLOC_H`, `CONFIG_ICONV`, `HAVE_MEMALIGN`, `HAVE_SCHED_GETAFFINITY` and `HAVE_LINUX_PERF_EVENT_H` turned off, and selects it for `__PROSPERO__`.

## rexglue-vr.patch

VR on the Vulkan backend through OpenXR, turned on by `--vr=true` (or `MCLA_VR=1`); without it nothing changes. The
game's side (the head-posed camera, the VR defaults) is in `src/vr_camera.cpp` and `src/perf_options.cpp`.

| Change | Files |
|---|---|
| OpenXR session, headset frame loop and pacing, the HUD on its own layer, recentring (F8) | `src/ui/vulkan/vr_openxr.cpp`, `include/rex/ui/vulkan/vr_openxr.h`, `src/ui/vulkan/shaders/vr_hud_alpha.*`, `thirdparty/openxr` (Khronos headers) |
| Stereo: each submission replayed for the right eye on twins of the GPU's resources, a clip-space eye shift for perspective draws, pixel-shader textures pushed per draw | `src/ui/vulkan/vr_twins.cpp`, `include/rex/ui/vulkan/vr_twins.h`, `src/graphics/vulkan/deferred_command_buffer.*`, `src/graphics/vulkan/command_processor.cpp` |
| Which draws, render targets and resolved pages differ per eye | `src/graphics/vulkan/command_processor.cpp`, `src/graphics/vulkan/texture_cache.cpp`, `src/graphics/shared_memory.cpp` |
| GPU thread cost: submissions on their own thread, uploads hoisted to a submission's prologue, hot pages uploaded only when changed, dynamic constant buffers in VR | `src/graphics/vulkan/command_processor.cpp`, `src/graphics/shared_memory.cpp`, `src/graphics/vulkan/shared_memory.cpp` |
| No desktop window presents in VR unless `vr_desktop_mirror=true` (a windowed present behind the headset's window now and then stalled the GPU queue) | `src/ui/vulkan/vulkan_presenter.cpp` |

The OpenXR loader (`openxr_loader.dll`, Khronos 1.1.63) goes beside `mcla.exe`; `scripts/make_pc.ps1` fetches it.
