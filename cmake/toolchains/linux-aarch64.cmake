# Cross-compile for linux-aarch64 with an unwrapped clang/lld and an Ubuntu
# arm64 sysroot. Used by the linux-aarch64-* presets from `nix develop .#cross`
# (see flake.nix and tools/cross-aarch64/).
#
# Inputs (environment, normally set by the nix shell):
#   SKATE3_AARCH64_SYSROOT       sysroot from tools/cross-aarch64/make-sysroot.sh
#   SKATE3_CROSS_CLANG/CLANGXX   unwrapped clang / clang++
#   SKATE3_CROSS_RESOURCE_DIR    clang resource dir with the builtin headers
#   SKATE3_CROSS_LLVM_BIN        directory with llvm-ar, llvm-ranlib, ...
#   SKATE3_CROSS_LLD             ld.lld
#   SKATE3_CROSS_PKG_CONFIG      pkg-config that has no baked-in search paths
#   SKATE3_CROSS_WAYLAND_SCANNER host wayland-scanner

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_skate3_triple aarch64-linux-gnu)

foreach(_var SKATE3_AARCH64_SYSROOT SKATE3_CROSS_CLANG SKATE3_CROSS_CLANGXX
        SKATE3_CROSS_RESOURCE_DIR SKATE3_CROSS_LLVM_BIN SKATE3_CROSS_LLD)
    if(NOT DEFINED ENV{${_var}} OR "$ENV{${_var}}" STREQUAL "")
        message(FATAL_ERROR "${_var} is not set; configure from `nix develop .#cross`")
    endif()
endforeach()

set(CMAKE_SYSROOT "$ENV{SKATE3_AARCH64_SYSROOT}")
if(NOT EXISTS "${CMAKE_SYSROOT}/usr/include/stdio.h")
    message(FATAL_ERROR "Sysroot ${CMAKE_SYSROOT} is missing; run tools/cross-aarch64/make-sysroot.sh")
endif()

set(CMAKE_C_COMPILER "$ENV{SKATE3_CROSS_CLANG}")
set(CMAKE_CXX_COMPILER "$ENV{SKATE3_CROSS_CLANGXX}")
set(CMAKE_ASM_COMPILER "$ENV{SKATE3_CROSS_CLANG}")
set(CMAKE_C_COMPILER_TARGET ${_skate3_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_skate3_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_skate3_triple})

set(CMAKE_AR "$ENV{SKATE3_CROSS_LLVM_BIN}/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "$ENV{SKATE3_CROSS_LLVM_BIN}/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_NM "$ENV{SKATE3_CROSS_LLVM_BIN}/llvm-nm" CACHE FILEPATH "")
set(CMAKE_STRIP "$ENV{SKATE3_CROSS_LLVM_BIN}/llvm-strip" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "$ENV{SKATE3_CROSS_LLVM_BIN}/llvm-objcopy" CACHE FILEPATH "")
set(CMAKE_OBJDUMP "$ENV{SKATE3_CROSS_LLVM_BIN}/llvm-objdump" CACHE FILEPATH "")
set(CMAKE_READELF "$ENV{SKATE3_CROSS_LLVM_BIN}/llvm-readelf" CACHE FILEPATH "")
set(CMAKE_LINKER "$ENV{SKATE3_CROSS_LLD}" CACHE FILEPATH "")

set(_skate3_common_flags "-resource-dir=$ENV{SKATE3_CROSS_RESOURCE_DIR}")
set(CMAKE_C_FLAGS_INIT "${_skate3_common_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_skate3_common_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_skate3_common_flags}")

set(_skate3_link_flags "-fuse-ld=lld --ld-path=$ENV{SKATE3_CROSS_LLD}")
# nixpkgs patches the clang driver to leave -dynamic-linker to its cc-wrapper,
# so the unwrapped driver links executables without PT_INTERP.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${_skate3_link_flags} -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_skate3_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_skate3_link_flags}")

# Libraries, headers and packages only from the sysroot; programs (code
# generators like wayland-scanner, Python) only from the host.
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
# nixpkgs patches CMake to drop the FHS prefixes from the default search
# path; put them back so find_path/find_library (FindX11, FindALSA, ...)
# look in <sysroot>/usr.
list(APPEND CMAKE_SYSTEM_PREFIX_PATH /usr /)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE ${_skate3_triple})

# pkg-config: search only the sysroot's .pc files and prefix their paths.
if(DEFINED ENV{SKATE3_CROSS_PKG_CONFIG})
    set(PKG_CONFIG_EXECUTABLE "$ENV{SKATE3_CROSS_PKG_CONFIG}" CACHE FILEPATH "")
endif()
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/lib/${_skate3_triple}/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")

if(DEFINED ENV{SKATE3_CROSS_WAYLAND_SCANNER})
    set(WAYLAND_SCANNER "$ENV{SKATE3_CROSS_WAYLAND_SCANNER}" CACHE FILEPATH "")
endif()
