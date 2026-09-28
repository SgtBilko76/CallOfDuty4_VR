# Cross-compile the 32-bit Windows build from Linux with llvm-mingw.
#
# The shipping build is MSVC (`scripts\mksln.bat`, Visual Studio 17 2022,
# -A Win32). This toolchain exists so the same sources can be compiled and
# iterated on from a Linux workstation; it is a development aid, not a
# replacement for the release build.
#
# Set KISAK_MINGW_ROOT to point at another llvm-mingw installation.
#
# Configure with:
#   cmake -B build-mingw -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=scripts/toolchains/windows-x86-mingw.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

if(NOT KISAK_MINGW_ROOT)
    file(GLOB _mingw_roots "$ENV{HOME}/toolchains/llvm-mingw-*")
    list(SORT _mingw_roots COMPARE NATURAL ORDER DESCENDING)
    if(_mingw_roots)
        list(GET _mingw_roots 0 KISAK_MINGW_ROOT)
    endif()
endif()

if(NOT KISAK_MINGW_ROOT)
    message(FATAL_ERROR
        "No llvm-mingw found under ~/toolchains; set KISAK_MINGW_ROOT.")
endif()

set(CMAKE_C_COMPILER   "${KISAK_MINGW_ROOT}/bin/i686-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${KISAK_MINGW_ROOT}/bin/i686-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER  "${KISAK_MINGW_ROOT}/bin/i686-w64-mingw32-windres")

set(CMAKE_FIND_ROOT_PATH "${KISAK_MINGW_ROOT}/i686-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The decompiled sources are written for MSVC: __declspec, MS struct
# extensions, and 32-bit MS-style __asm blocks.
# Not -fms-compatibility: it defines _MSC_VER, which sends mingw's own CRT
# headers down MSVC-only paths they cannot satisfy.
# -Wno-c++11-narrowing: the enums carry values like 0xFFFFFFFF in an
# __int32 enum, which MSVC accepts and which keeps the same bit pattern.
set(KISAK_MINGW_MS_FLAGS
    "-fms-extensions -fdeclspec -fasm-blocks -Wno-c++11-narrowing")
set(CMAKE_C_FLAGS_INIT   "${KISAK_MINGW_MS_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${KISAK_MINGW_MS_FLAGS}")
