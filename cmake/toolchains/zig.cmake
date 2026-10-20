# Toolchain file for cross-compiling with `zig cc` (D256): one file for every
# native target of the distribution archives (tools/dist.sh). Zig carries
# the C library headers and link stubs for musl, macOS and MinGW-w64, so
# a Linux or macOS host builds all of them without a sysroot.
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/zig.cmake \
#         -DSXML_ZIG_TARGET=aarch64-linux-musl ...
#
# SXML_ZIG_TARGET is a Zig target triple: <arch>-linux-musl (a static
# executable), <arch>-macos.<version> (linked against libSystem), or
# <arch>-windows-gnu (MinGW-w64, a static executable). The zig program is
# found on PATH (mise.toml pins it) or named by SXML_ZIG_EXECUTABLE.

if(NOT SXML_ZIG_TARGET)
    if(DEFINED ENV{SXML_ZIG_TARGET})
        set(SXML_ZIG_TARGET "$ENV{SXML_ZIG_TARGET}")
    else()
        message(FATAL_ERROR "zig.cmake: set SXML_ZIG_TARGET, for example x86_64-linux-musl")
    endif()
endif()
set(SXML_ZIG_TARGET "${SXML_ZIG_TARGET}" CACHE STRING "Zig target triple")
# try_compile projects read the toolchain file again; they see the cache
# variable through this list.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SXML_ZIG_TARGET SXML_ZIG_EXECUTABLE)

if(NOT SXML_ZIG_EXECUTABLE)
    find_program(SXML_ZIG_EXECUTABLE zig REQUIRED)
endif()

string(REGEX MATCH "^[^-]+" sxml_zig_architecture "${SXML_ZIG_TARGET}")
if(SXML_ZIG_TARGET MATCHES "-linux")
    set(CMAKE_SYSTEM_NAME Linux)
elseif(SXML_ZIG_TARGET MATCHES "-macos")
    set(CMAKE_SYSTEM_NAME Darwin)
elseif(SXML_ZIG_TARGET MATCHES "-windows")
    set(CMAKE_SYSTEM_NAME Windows)
else()
    message(FATAL_ERROR "zig.cmake: unsupported target '${SXML_ZIG_TARGET}'")
endif()
set(CMAKE_SYSTEM_PROCESSOR "${sxml_zig_architecture}")

# `zig cc`, `zig ar` and `zig ranlib` are subcommands of one program, and
# CMake wants one program for each tool, so each gets a two-line shell
# wrapper in the build tree (the build machine is Linux or macOS). The
# platform modules of CMake rewrite the archiver rules after this file,
# so wrapping the program is the one way that holds for every system.
foreach(sxml_zig_tool IN ITEMS cc ar ranlib)
    set(sxml_zig_wrapper "${CMAKE_BINARY_DIR}/zig-tools/zig-${sxml_zig_tool}")
    file(WRITE "${sxml_zig_wrapper}"
        "#!/bin/sh\nexec \"${SXML_ZIG_EXECUTABLE}\" ${sxml_zig_tool} \"$@\"\n")
    file(CHMOD "${sxml_zig_wrapper}" PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()
set(CMAKE_C_COMPILER "${CMAKE_BINARY_DIR}/zig-tools/zig-cc")
set(CMAKE_C_COMPILER_TARGET "${SXML_ZIG_TARGET}")
set(CMAKE_AR "${CMAKE_BINARY_DIR}/zig-tools/zig-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "${CMAKE_BINARY_DIR}/zig-tools/zig-ranlib" CACHE FILEPATH "Ranlib")

# zig's linker does not take --dependency-file, which CMake 3.27 and later
# pass to GNU-like linkers.
set(CMAKE_LINK_DEPENDS_USE_LINKER FALSE)

# Never look for libraries or headers of the build machine.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
