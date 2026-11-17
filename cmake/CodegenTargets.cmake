add_custom_target(generate-skate3
    COMMAND "${CMAKE_COMMAND}"
            "-DREXGLUE=$<TARGET_FILE:rex::rexglue>"
            "-DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/manifests/skate3.toml"
            "-DTOOL_STAMP=${CMAKE_CURRENT_SOURCE_DIR}/generated/codegen-tool.skate3.sha256"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunCodegen.cmake"
    COMMAND "${CMAKE_COMMAND}"
            "-DSKATE3_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplySkate3CodegenPatches.cmake"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating recompiled code for default.xex"
    VERBATIM
)

add_custom_target(generate-eawebkit
    COMMAND "${CMAKE_COMMAND}"
            "-DREXGLUE=$<TARGET_FILE:rex::rexglue>"
            "-DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/manifests/eawebkit.toml"
            "-DTOOL_STAMP=${CMAKE_CURRENT_SOURCE_DIR}/generated/codegen-tool.eawebkit.sha256"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunCodegen.cmake"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating recompiled code for EAWebkit.xex"
    VERBATIM
)

if(TARGET skate3-title-update-codegen-inputs)
    add_dependencies(generate-skate3 skate3-title-update-codegen-inputs)
    add_dependencies(generate-eawebkit skate3-title-update-codegen-inputs)
endif()

add_dependencies(generate-skate3 generate-eawebkit)

add_custom_target(generate-all
    DEPENDS generate-skate3 generate-eawebkit
)

add_custom_target(skate3_codegen DEPENDS generate-skate3)
add_custom_target(eawebkit_codegen DEPENDS generate-eawebkit)
add_custom_target(all_codegen DEPENDS generate-all)
