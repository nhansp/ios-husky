# SPDX-License-Identifier: MIT
# Unified CMake toolchain for iOS (iphoneos arm64e/arm64) and Linux aarch64 proxy build.
#
# iOS:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/ios-arm64.toolchain.cmake -DMALI_IOS_ARCH=arm64e ...
#
# Linux aarch64 proxy build:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/ios-arm64.toolchain.cmake -DMALI_PROXY_BUILD=ON ...
#   (or -DCMAKE_SYSROOT=/usr/aarch64-linux-gnu)

# Determine whether this is a Linux aarch64 proxy build or an Apple iOS build.
if(MALI_PROXY_BUILD OR
   (DEFINED CMAKE_SYSTEM_NAME AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT APPLE) OR
   (DEFINED CMAKE_SYSROOT AND CMAKE_SYSROOT MATCHES ".*(linux|aarch64).*"))

  # ============================================================================
  # 1. Linux aarch64-linux-gnu PROXY Toolchain
  # ============================================================================
  set(CMAKE_SYSTEM_NAME Linux)
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
  set(_triple aarch64-linux-gnu)

  if(NOT DEFINED CMAKE_SYSROOT OR CMAKE_SYSROOT STREQUAL "")
    set(CMAKE_SYSROOT "/usr/${_triple}" CACHE PATH "Linux aarch64 sysroot" FORCE)
  endif()

  find_program(_clang NAMES clang-18 clang)
  if(_clang)
    set(CMAKE_C_COMPILER "${_clang}" CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_C_COMPILER_TARGET ${_triple} CACHE STRING "C compiler target" FORCE)
    find_program(_clangxx NAMES clang++-18 clang++)
    set(CMAKE_CXX_COMPILER "${_clangxx}" CACHE FILEPATH "C++ compiler" FORCE)
    set(CMAKE_CXX_COMPILER_TARGET ${_triple} CACHE STRING "C++ compiler target" FORCE)
  else()
    set(CMAKE_C_COMPILER ${_triple}-gcc CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER ${_triple}-g++ CACHE FILEPATH "C++ compiler" FORCE)
  endif()

  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

  # Fallback for libstdc++ arch-specific c++config.h if cross libstdc++ headers missing
  if(NOT EXISTS "${CMAKE_SYSROOT}/include/c++")
    file(GLOB _cxxcfg "/usr/include/x86_64-linux-gnu/c++/*")
    list(GET _cxxcfg -1 _cxxcfg_dir)
    if(_cxxcfg_dir)
      set(CMAKE_CXX_FLAGS_INIT "-isystem ${_cxxcfg_dir}")
    endif()
  endif()

else()

  # ============================================================================
  # 2. Apple iOS (iphoneos SDK) Toolchain (arm64e / arm64)
  # ============================================================================
  set(CMAKE_SYSTEM_NAME iOS)
  set(CMAKE_SYSTEM_PROCESSOR arm64)

  # Architecture: default to arm64e (iOS 26 userland standard)
  if(NOT DEFINED MALI_IOS_ARCH OR MALI_IOS_ARCH STREQUAL "")
    if(DEFINED ENV{ARCH} AND NOT "$ENV{ARCH}" STREQUAL "")
      set(MALI_IOS_ARCH "$ENV{ARCH}")
    elseif(DEFINED ENV{MALI_IOS_ARCH} AND NOT "$ENV{MALI_IOS_ARCH}" STREQUAL "")
      set(MALI_IOS_ARCH "$ENV{MALI_IOS_ARCH}")
    else()
      set(MALI_IOS_ARCH "arm64e")
    endif()
  endif()
  set(CMAKE_OSX_ARCHITECTURES "${MALI_IOS_ARCH}" CACHE STRING "OSX Architectures" FORCE)

  # Minimum iOS Deployment Target: 26.0 (aligning with MaliMetal and CI)
  if(NOT DEFINED MALI_IOS_MIN OR MALI_IOS_MIN STREQUAL "")
    if(DEFINED ENV{DEPLOY_TARGET} AND NOT "$ENV{DEPLOY_TARGET}" STREQUAL "")
      set(MALI_IOS_MIN "$ENV{DEPLOY_TARGET}")
    elseif(DEFINED ENV{MALI_IOS_MIN} AND NOT "$ENV{MALI_IOS_MIN}" STREQUAL "")
      set(MALI_IOS_MIN "$ENV{MALI_IOS_MIN}")
    else()
      set(MALI_IOS_MIN "26.0")
    endif()
  endif()
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${MALI_IOS_MIN}" CACHE STRING "iOS Deployment Target" FORCE)

  # SDK Sysroot (iphoneos)
  if(NOT DEFINED MALI_IOS_SDK OR MALI_IOS_SDK STREQUAL "")
    if(DEFINED ENV{SDK} AND NOT "$ENV{SDK}" STREQUAL "")
      set(MALI_IOS_SDK "$ENV{SDK}")
    elseif(DEFINED ENV{MALI_IOS_SDK} AND NOT "$ENV{MALI_IOS_SDK}" STREQUAL "")
      set(MALI_IOS_SDK "$ENV{MALI_IOS_SDK}")
    else()
      set(MALI_IOS_SDK "iphoneos")
    endif()
  endif()
  set(CMAKE_OSX_SYSROOT "${MALI_IOS_SDK}" CACHE STRING "OSX Sysroot" FORCE)

  # Apple clang pointer authentication for arm64e:
  # Apple clang automatically manages PAC rules for arm64e, explicitly passing:
  # -fptrauth-calls -fptrauth-returns -fptrauth-auth-traps -fptrauth-indirect-gotos
  # No -march guessing.
  if(MALI_IOS_ARCH MATCHES "arm64e")
    set(_ptrauth_flags "-fptrauth-calls -fptrauth-returns -fptrauth-auth-traps -fptrauth-indirect-gotos")
    set(CMAKE_C_FLAGS_INIT "${_ptrauth_flags}")
    set(CMAKE_CXX_FLAGS_INIT "${_ptrauth_flags}")
  endif()

  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)

endif()
