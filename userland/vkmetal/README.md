# HuskyVKMetal

A Metal driver plug-in for iOS 26.5 (23F77) on the Pixel 8 Pro (Mali-G715): Metal implemented by the metal2vulkan runtime on the Mesa panvk Vulkan
driver (`libvulkan_panfrost.dylib`, `userland/panvk-darwin`). A port of the front end of UniXNU's AdrenoMetal.

Discovery is Apple's: the accelerator service's `MetalPluginName` property (the kernel publishes `HuskyVKMetal` when booted with `huskymetal=vk`)
names `/System/Library/Extensions/HuskyVKMetal.bundle`; its principal class `HuskyVKMetalDevice` (a subclass of `_MTLDevice`) gets
`-initWithAcceleratorPort:` and returns the frontend's device, which Metal registers (`MTLAddDevice`).

## Layout

| File | |
|---|---|
| `HuskyVKMetalPlugin.mm` | discovery entry, cache directories, driver path, boot-argument switches |
| `HuskyVKMetalFrontend.mm/.h` | the Metal objects over the bridge's C ABI (AdrenoMetal's `NativeBridgeMetalFrontend.mm`, trimmed and extended) |
| `bridge/` | the Rust staticlib crate (`nmb_*` C ABI over metal2vulkan + its runtime), `RustPacBoundary.c`, `patches/` against metal2vulkan |
| `scripts/rust-pac-rewrite.py` | arm64e pointer-authentication rewrite of the Rust code's indirect branches (UniXNU) |
| `stubs/Metal.tbd` | link stub that exports the private Metal classes (from `plugin/MaliMetal`) |
| `build.sh` | VM-side build; `tools/dev/vm-vkmetal.sh OUTDIR` drives it |

## Licensing and linking

- AdrenoMetal sources: MIT (`licenses/LICENSE.AdrenoMetal-MIT`). `scripts/rust-pac-rewrite.py` is from UniXNU, BSD-2-Clause-Patent
  (`licenses/LICENSE.UniXNU-BSD-2-Clause-Patent`).
- metal2vulkan is GPL-2.0-or-later OR LGPL-3.0-or-later; it is used under **LGPL-3.0-or-later**. It is never linked into the plug-in executable:
  `build.sh` links the Rust staticlib (metal2vulkan, its runtime, the bridge and `RustPacBoundary.c`) into its own dylib,
  `HuskyVKMetal.bundle/Frameworks/libhuskyvk_m2v.dylib`, which the plug-in executable loads dynamically. That dylib can be rebuilt from the pinned
  source (`555b13d57e09bff70996a88cff64f1275256d5cf` plus `bridge/patches/*.patch`, which are the modifications) and replaced. The pin, the
  patches and the build recipe are in this directory; the LGPL/GPL texts are in `licenses/` and copied into the bundle.
- `bridge/patches/0002`-`0041` are AdrenoMetal's twelve patches (all apply to the pin); `0050` is ours: iOS enables the runtime's host-page copy path (read_pages, mapped_writable), Apple accepts a source's dma-buf tokens (dma_buf_length), and `open_loader` falls back to a driver library's `vk_icdGetInstanceProcAddr` (`libvulkan_panfrost.dylib` exports no `vkGetInstanceProcAddr`).

## Behaviour notes

- Shader compile: libraries from metallib data are read by the runtime itself (no compile service). `newLibraryWithSource:` and stitched libraries
  need MTLCompiler's `MTLCodeGenServiceBuildRequest` types 13 and 14, whose layout is not verified on 26.5: they are refused unless the kernel
  command line has `huskyvk-codegen=1`. The device's `-compiler` is MaliMetal's `MTLCompiler` with the `simulatorDummyPlugin` path.
- IOSurface: a surface's pages reach panvk as a dma-buf token (`husky_drm_surface_fd` of the shim, looked up by `husky_drm_default_fd`), which the
  runtime imports through `VK_EXT_external_memory_dma_buf` (patch 0050 lets it accept the token on Apple). Without the symbols, or with
  `huskyvk-nodmabuf=1`, a surface is copied.
- `newBufferWithBytesNoCopy:` over ordinary memory is a copy kept coherent at every commit and completion by the runtime (patch 0050 enables the
  runtime's copy path on iOS).
- Switches (kernel boot arguments, mirrored into the environment): `huskyvk-codegen=1`, `huskyvk-nodmabuf=1`, `diag=1`, `log-gpu=1`, `gpu-purge=0`.
  `HUSKYVK_ICD` overrides the Vulkan driver path (default: `Frameworks/libvulkan_panfrost.dylib` in the bundle, then `/usr/local/lib/`).
