# Cross-compile for Windows x64 (MSVC ABI) with an unwrapped clang/lld-link
# and the MSVC CRT/STL + Windows SDK splatted by xwin. Used by the
# windows-x64-* presets from `nix develop .#windows` (see flake.nix and
# tools/cross-windows/).
#
# This uses clang's GNU-style driver with --target=x86_64-pc-windows-msvc,
# the same compiler mode upstream uses natively on Windows. It is not
# clang-cl: the SDK passes GNU flags and keys code paths on MSVC being false.
#
# Inputs (environment, normally set by the nix shell):
#   SKATE3_XWIN_ROOT            xwin splat from tools/cross-windows/make-winsdk.sh
#   SKATE3_CROSS_CLANG/CLANGXX  unwrapped clang / clang++
#   SKATE3_CROSS_RESOURCE_DIR   clang resource dir with the builtin headers
#   SKATE3_CROSS_LLVM_BIN       directory with llvm-ar, llvm-rc, llvm-mt, ...
#   SKATE3_CROSS_LLD_LINK       lld-link

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_skate3_triple x86_64-pc-windows-msvc)

foreach(_var SKATE3_XWIN_ROOT SKATE3_CROSS_CLANG SKATE3_CROSS_CLANGXX
        SKATE3_CROSS_RESOURCE_DIR SKATE3_CROSS_LLVM_BIN SKATE3_CROSS_LLD_LINK)
    if(NOT DEFINED ENV{${_var}} OR "$ENV{${_var}}" STREQUAL "")
        message(FATAL_ERROR "${_var} is not set; configure from `nix develop .#windows`")
    endif()
endforeach()

set(_xwin "$ENV{SKATE3_XWIN_ROOT}")
if(NOT EXISTS "${_xwin}/crt/include/vcruntime.h" OR NOT EXISTS "${_xwin}/sdk/include/um/Windows.h")
    message(FATAL_ERROR "xwin splat ${_xwin} is missing; run tools/cross-windows/make-winsdk.sh")
endif()

set(CMAKE_C_COMPILER "$ENV{SKATE3_CROSS_CLANG}")
set(CMAKE_CXX_COMPILER "$ENV{SKATE3_CROSS_CLANGXX}")
set(CMAKE_ASM_COMPILER "$ENV{SKATE3_CROSS_CLANG}")
set(CMAKE_C_COMPILER_TARGET ${_skate3_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_skate3_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_skate3_triple})

set(_llvm "$ENV{SKATE3_CROSS_LLVM_BIN}")
set(CMAKE_AR "${_llvm}/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_llvm}/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_NM "${_llvm}/llvm-nm" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${_llvm}/llvm-objcopy" CACHE FILEPATH "")
set(CMAKE_OBJDUMP "${_llvm}/llvm-objdump" CACHE FILEPATH "")
set(CMAKE_RC_COMPILER "${_llvm}/llvm-rc" CACHE FILEPATH "")
set(CMAKE_MT "${_llvm}/llvm-mt" CACHE FILEPATH "")
set(CMAKE_LINKER "$ENV{SKATE3_CROSS_LLD_LINK}" CACHE FILEPATH "")

# CRT/STL and SDK headers. -imsvc only exists in clang-cl, and plain -isystem
# dirs are searched before the clang resource dir, so MSVC's stddef.h,
# intrin.h and mmintrin.h would shadow clang's (offsetof stops being a
# constant expression, __m64 breaks). List the resource include dir first,
# which is the order clang-cl uses; clang drops the later duplicate.
set(_skate3_xwin_includes
    "$ENV{SKATE3_CROSS_RESOURCE_DIR}/include"
    "${_xwin}/crt/include"
    "${_xwin}/sdk/include/ucrt"
    "${_xwin}/sdk/include/um"
    "${_xwin}/sdk/include/shared"
    "${_xwin}/sdk/include/winrt")
set(_skate3_common_flags "-resource-dir=$ENV{SKATE3_CROSS_RESOURCE_DIR}")
foreach(_dir IN LISTS _skate3_xwin_includes)
    string(APPEND _skate3_common_flags " -isystem \"${_dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_skate3_common_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_skate3_common_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_skate3_common_flags}")

# llvm-rc preprocesses .rc files itself and needs the SDK headers (winver.h,
# windows.h) too.
set(CMAKE_RC_FLAGS_INIT "")
foreach(_dir IN LISTS _skate3_xwin_includes)
    string(APPEND CMAKE_RC_FLAGS_INIT " -I \"${_dir}\"")
endforeach()

set(_skate3_link_flags "-fuse-ld=$ENV{SKATE3_CROSS_LLD_LINK}")
foreach(_dir crt/lib/x86_64 sdk/lib/um/x86_64 sdk/lib/ucrt/x86_64)
    string(APPEND _skate3_link_flags " -Xlinker \"/libpath:${_xwin}/${_dir}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_skate3_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_skate3_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_skate3_link_flags}")

# The splat has no debug CRT (libcmtd), and try_compile defaults to Debug.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# Libraries, headers and packages only from the xwin root; programs (Python,
# codegen helpers) only from the host.
set(CMAKE_FIND_ROOT_PATH "${_xwin}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
