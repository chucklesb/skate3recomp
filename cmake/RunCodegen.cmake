# Runs `rexglue codegen` for one manifest, skipping the work when nothing
# changed (cmake -P; called by cmake/CodegenTargets.cmake).
#
#   -DREXGLUE=<rexglue binary> -DMANIFEST=<manifest.toml> -DTOOL_STAMP=<file>
#   -DSDK_ROOT=<rexglue SDK source tree>
#
# rexglue's own output stamp fingerprints the module's inputs (xex, configs,
# manifest; by content) but not rexglue itself, so a codegen fix in the SDK
# would be skipped. We pass --ignore-stamp when the SDK sources that decide
# the generated code changed since this manifest's output was generated:
# the code generator and its CLI, the XEX loader and export tables
# (src/system, src/kernel: import names), the instruction decoder and the
# output templates. Other SDK changes (UI, graphics, audio, input, core)
# rebuild the rexglue binary but can't change its output, so they no longer
# force the ~2.5 minute analysis; rexglue's stamp decides.
#
# Escape hatch: SKATE3_FORCE_CODEGEN=1 in the environment always regenerates
# (for a codegen-affecting change outside the fingerprinted directories).
foreach(_var REXGLUE MANIFEST TOOL_STAMP SDK_ROOT)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "RunCodegen.cmake: -D${_var}=... is required")
    endif()
endforeach()

# Fingerprint: SHA-256 over "relative path + content hash" of every file
# in these SDK directories, sorted.
set(_fingerprint_dirs
    src/codegen src/rexglue include/rex/codegen
    src/system include/rex/system
    src/kernel include/rex/kernel
    thirdparty/disasm resources/templates)
set(_files "")
foreach(_dir IN LISTS _fingerprint_dirs)
    if(NOT IS_DIRECTORY "${SDK_ROOT}/${_dir}")
        message(FATAL_ERROR "RunCodegen.cmake: ${SDK_ROOT}/${_dir} is missing (update the fingerprint list)")
    endif()
    file(GLOB_RECURSE _found LIST_DIRECTORIES false RELATIVE "${SDK_ROOT}" "${SDK_ROOT}/${_dir}/*")
    list(APPEND _files ${_found})
endforeach()
list(SORT _files)
set(_lines "")
foreach(_file IN LISTS _files)
    file(SHA256 "${SDK_ROOT}/${_file}" _file_hash)
    string(APPEND _lines "${_file} ${_file_hash}\n")
endforeach()
string(SHA256 _tool_hash "${_lines}")
set(_previous "")
if(EXISTS "${TOOL_STAMP}")
    file(READ "${TOOL_STAMP}" _previous)
    string(STRIP "${_previous}" _previous)
endif()

set(_args codegen)
if(NOT _previous STREQUAL _tool_hash)
    list(APPEND _args --ignore-stamp)
    message(STATUS "codegen: the SDK's codegen sources changed (or first run); regenerating ${MANIFEST}")
elseif("$ENV{SKATE3_FORCE_CODEGEN}" STREQUAL "1")
    list(APPEND _args --ignore-stamp)
    message(STATUS "codegen: SKATE3_FORCE_CODEGEN=1; regenerating ${MANIFEST}")
endif()

execute_process(
    COMMAND "${REXGLUE}" ${_args} "${MANIFEST}"
    RESULT_VARIABLE _result
)
if(NOT _result EQUAL 0)
    file(REMOVE "${TOOL_STAMP}")
    message(FATAL_ERROR "rexglue codegen failed (${_result}) for ${MANIFEST}")
endif()

if(NOT _previous STREQUAL _tool_hash)
    file(WRITE "${TOOL_STAMP}" "${_tool_hash}\n")
endif()
