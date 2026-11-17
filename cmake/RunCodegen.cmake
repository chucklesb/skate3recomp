# Runs `rexglue codegen` for one manifest, skipping the work when nothing
# changed (cmake -P; called by cmake/CodegenTargets.cmake).
#
#   -DREXGLUE=<rexglue binary> -DMANIFEST=<manifest.toml> -DTOOL_STAMP=<file>
#
# rexglue's own output stamp fingerprints the module's inputs (xex, configs,
# manifest; by content) but not the rexglue build itself, so a codegen fix in
# the SDK would be skipped. We therefore pass --ignore-stamp only when the
# rexglue binary differs (SHA-256) from the one that last generated this
# manifest's output; otherwise rexglue's stamp decides, and an unchanged
# project skips its ~2.5 minute analysis.
foreach(_var REXGLUE MANIFEST TOOL_STAMP)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "RunCodegen.cmake: -D${_var}=... is required")
    endif()
endforeach()

file(SHA256 "${REXGLUE}" _tool_hash)
set(_previous "")
if(EXISTS "${TOOL_STAMP}")
    file(READ "${TOOL_STAMP}" _previous)
    string(STRIP "${_previous}" _previous)
endif()

set(_args codegen)
if(NOT _previous STREQUAL _tool_hash)
    list(APPEND _args --ignore-stamp)
    message(STATUS "codegen: rexglue changed (or first run); regenerating ${MANIFEST}")
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
