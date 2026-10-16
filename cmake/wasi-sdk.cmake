# Toolchain file for WebAssembly builds with the WASI SDK (D10).
#
# The SDK is found through, in order: the CMake variable WASI_SDK_PREFIX, the
# environment variable WASI_SDK_PREFIX, then /opt/wasi-sdk. The target is
# wasm32-wasip1; test executables run under Node.js (tools/run-wasi.mjs).

if(NOT WASI_SDK_PREFIX)
    if(DEFINED ENV{WASI_SDK_PREFIX})
        set(WASI_SDK_PREFIX "$ENV{WASI_SDK_PREFIX}")
    else()
        set(WASI_SDK_PREFIX "/opt/wasi-sdk")
    endif()
endif()
file(TO_CMAKE_PATH "${WASI_SDK_PREFIX}" WASI_SDK_PREFIX)
set(WASI_SDK_PREFIX "${WASI_SDK_PREFIX}" CACHE PATH "WASI SDK installation directory")

set(sxml_wasi_sdk_toolchain "${WASI_SDK_PREFIX}/share/cmake/wasi-sdk-p1.cmake")
if(NOT EXISTS "${sxml_wasi_sdk_toolchain}")
    message(FATAL_ERROR
        "WASI SDK not found at '${WASI_SDK_PREFIX}'. Install it from "
        "https://github.com/WebAssembly/wasi-sdk/releases and set WASI_SDK_PREFIX.")
endif()
include("${sxml_wasi_sdk_toolchain}")
