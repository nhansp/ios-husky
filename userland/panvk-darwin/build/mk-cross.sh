#!/bin/sh
# mk-cross.sh DIR SHIM_PREFIX TOOLS_BIN: writes DIR/cross.ini (arm64e iOS 26 target) and DIR/native.ini (Mesa's
# build-time tools from TOOLS_BIN) for meson.
set -eu
D=${1:?}; SHIM=${2:?}; TOOLS=${3:?}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
FLAGS="'-target', 'arm64e-apple-ios26.0', '-isysroot', '$SDK'"
mkdir -p "$D"
cat > "$D/cross.ini" <<INI
[binaries]
c = '$(xcrun --sdk iphoneos --find clang)'
cpp = '$(xcrun --sdk iphoneos --find clang++)'
objc = '$(xcrun --sdk iphoneos --find clang)'
ar = '$(xcrun --sdk iphoneos --find ar)'
strip = '$(xcrun --sdk iphoneos --find strip)'
pkg-config = '/usr/local/bin/pkg-config'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64e'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$SHIM/lib/pkgconfig'

[built-in options]
c_args = [$FLAGS, '-fstack-protector-strong', '-DHUSKY_DRM_SHIM']
cpp_args = [$FLAGS, '-fstack-protector-strong', '-DHUSKY_DRM_SHIM']
objc_args = [$FLAGS]
c_link_args = [$FLAGS]
cpp_link_args = [$FLAGS]
objc_link_args = [$FLAGS]
INI
cat > "$D/native.ini" <<INI
[binaries]
c = '/usr/bin/clang'
cpp = '/usr/bin/clang++'
objc = '/usr/bin/clang'
mesa_clc = '$TOOLS/mesa_clc'
vtn_bindgen2 = '$TOOLS/vtn_bindgen2'
panfrost_compile = '$TOOLS/panfrost_compile'
INI
