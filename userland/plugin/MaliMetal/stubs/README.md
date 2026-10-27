Link stubs (text-based .tbd: symbol names only, no code) for the iOS 26.5 frameworks the plug-in
links against. IOGPU.framework is private; Metal.tbd replaces the SDK's public one because the
plug-in subclasses private Metal classes (_MTLDevice, _MTLCommandQueue, ...) that the SDK stub omits.
Generated from the exported-symbol lists of the iOS 26.5 (23F77) dyld shared cache.
