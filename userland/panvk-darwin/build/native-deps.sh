#!/bin/sh
# Host (x86_64 macOS) toolchain for Mesa's build-time tools: LLVM+clang, SPIRV-Headers/Tools, SPIRV-LLVM-Translator
# and a Python venv with meson. mesa_clc turns the panvk OpenCL-C libpan sources into SPIR-V while building.
#   C  cache directory (default ~/husky/panvk/cache); installs into $C/native
set -eux
PATH=/usr/local/bin:$PATH
C=${C:-$HOME/husky/panvk/cache}
P=$C/native
LLVM_VER=18.1.8
TRANS_TAG=v18.1.15
TOOLS_TAG=vulkan-sdk-1.4.309.0
HDRS_TAG=vulkan-sdk-1.4.309.0
J=$(sysctl -n hw.ncpu)
mkdir -p "$C" "$P"
cd "$C"

fetch() { # url dir strip
	[ -d "$2" ] && return
	mkdir -p "$2.tmp"
	curl -fsSL "$1" | tar -xz${4:-} -C "$2.tmp" --strip-components=1
	mv "$2.tmp" "$2"
}

# The system Python is 3.9; Mesa wants >= 3.10.
PY_URL=https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.13.18%2B20261110-x86_64-apple-darwin-install_only.tar.gz
if [ ! -x "$P/bin/meson" ]; then
	[ -x "$C/python/bin/python3" ] || { mkdir -p "$C/python" && curl -fsSL "$PY_URL" | tar -xz -C "$C/python" --strip-components=1; }
	rm -rf "$P/venv"
	"$C/python/bin/python3" -m venv "$P/venv"
	"$P/venv/bin/pip" install -q meson mako pyyaml packaging
	mkdir -p "$P/bin" && ln -sf "$P/venv/bin/meson" "$P/bin/meson"
fi

if [ ! -f "$P/bin/llvm-config" ]; then
	[ -d llvm-project-$LLVM_VER.src ] || {
		curl -fsSL -o llvm.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz
		tar -xJf llvm.tar.xz llvm-project-$LLVM_VER.src/llvm llvm-project-$LLVM_VER.src/clang llvm-project-$LLVM_VER.src/cmake \
			llvm-project-$LLVM_VER.src/third-party llvm-project-$LLVM_VER.src/clang-tools-extra/CMakeLists.txt
		rm llvm.tar.xz
	}
	cmake -G Ninja -S llvm-project-$LLVM_VER.src/llvm -B llvm-native -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$P" -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=X86 \
		-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
		-DCLANG_INCLUDE_TESTS=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
		-DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
		-DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_RTTI=ON -DLLVM_PARALLEL_LINK_JOBS=2 \
		-DLLVM_ENABLE_BINDINGS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF
	ninja -C llvm-native -j "${LLVM_JOBS:-6}" install
fi

# Before the SPIRV-Tools install: the translator wants its own SPIRV-Headers revision, not the installed one.
if [ ! -f "$P/lib/pkgconfig/LLVMSPIRVLib.pc" ]; then
	fetch https://github.com/KhronosGroup/SPIRV-LLVM-Translator/archive/refs/tags/$TRANS_TAG.tar.gz spirv-llvm-translator
	fetch https://github.com/KhronosGroup/SPIRV-Headers/archive/$(cat spirv-llvm-translator/spirv-headers-tag.conf).tar.gz spirv-headers-trans
	cmake -G Ninja -S spirv-llvm-translator -B spirv-llvm-translator-b -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$P" -DLLVM_DIR="$P/lib/cmake/llvm" -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$C/spirv-headers-trans" \
		-DLLVM_SPIRV_INCLUDE_TESTS=OFF -DLLVM_SPIRV_BUILD_EXTERNAL=YES -DBUILD_SHARED_LIBS=OFF
	ninja -C spirv-llvm-translator-b install
fi

if [ ! -f "$P/lib/pkgconfig/SPIRV-Tools.pc" ]; then
	fetch https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/$HDRS_TAG.tar.gz spirv-headers
	fetch https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/$TOOLS_TAG.tar.gz spirv-tools
	cmake -G Ninja -S spirv-headers -B spirv-headers-b -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_BUILD_TYPE=Release
	ninja -C spirv-headers-b install
	cmake -G Ninja -S spirv-tools -B spirv-tools-b -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_BUILD_TYPE=Release \
		-DSPIRV-Headers_SOURCE_DIR="$C/spirv-headers" -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
		-DSPIRV_WERROR=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
	ninja -C spirv-tools-b install
fi

echo native-deps ok
