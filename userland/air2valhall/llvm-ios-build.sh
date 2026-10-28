#!/bin/sh
# LLVM 18 libraries for iphoneos arm64e: no backends, the components air2valhall links
# (core bitreader bitwriter passes irreader analysis transformutils ipo scalaropts instcombine).
#   OUT  install directory (include/, lib/)
set -eux
: "${OUT:?}"
VER=18.1.8
WORK=${WORK:-$PWD/llvm-work}
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d llvm-project-$VER.src ]; then
	curl -fsSL -o llvm.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-$VER/llvm-project-$VER.src.tar.xz
	tar -xJf llvm.tar.xz llvm-project-$VER.src/llvm llvm-project-$VER.src/cmake llvm-project-$VER.src/third-party
	rm llvm.tar.xz
fi
SRC=$WORK/llvm-project-$VER.src/llvm

# host tablegen for the cross build
cmake -G Ninja -S "$SRC" -B host -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD= -DLLVM_INCLUDE_TESTS=OFF \
	-DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
	-DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF
ninja -C host llvm-tblgen
ninja -C host llvm-min-tblgen || true

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
cmake -G Ninja -S "$SRC" -B ios -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK" \
	-DCMAKE_OSX_ARCHITECTURES=arm64e -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
	-DLLVM_TARGETS_TO_BUILD= -DLLVM_ENABLE_PROJECTS= -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
	-DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
	-DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
	-DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_FFI=OFF \
	-DLLVM_ENABLE_PLUGINS=OFF -DLLVM_DEFAULT_TARGET_TRIPLE=arm64e-apple-ios26.0 -DLLVM_HOST_TRIPLE=arm64e-apple-ios26.0 \
	-DLLVM_TABLEGEN="$WORK/host/bin/llvm-tblgen" -DLLVM_NATIVE_TOOL_DIR="$WORK/host/bin"
ninja -C ios LLVMPasses LLVMIRReader LLVMBitWriter LLVMBitReader LLVMipo LLVMScalarOpts LLVMInstCombine \
	LLVMTransformUtils LLVMAnalysis LLVMCore

mkdir -p "$OUT/lib" "$OUT/include"
cp ios/lib/libLLVM*.a "$OUT/lib/"
cp -R "$SRC/include/llvm" "$SRC/include/llvm-c" "$OUT/include/"
cp -R ios/include/llvm "$OUT/include/"
ls -la "$OUT/lib"
