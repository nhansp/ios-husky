# SPDX-License-Identifier: MIT
# linux-arm64-proxy.toolchain.cmake: compatibility wrapper forwarding to the unified toolchain
set(MALI_PROXY_BUILD ON CACHE BOOL "Force Linux proxy mode" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/ios-arm64.toolchain.cmake")
