# Standalone C++ unit tests (KNOWN_ISSUES.md, KI-31).
#
# Each test covers pure logic in a header under src/native/ that includes only
# the C++ standard library, so these targets need no SDK, no game data and no
# codegen output. Configure them alone with:
#
#   cmake -S . -B out/build/tests -DSKATE3_TESTS_ONLY=ON
#   cmake --build out/build/tests --config Release
#   ctest --test-dir out/build/tests -C Release --output-on-failure
#
# or add -DSKATE3_BUILD_TESTS=ON to a normal configure to build them next to
# the game. A test that needs SDK or game code does not belong here: extract
# the logic it checks into a header like the ones below first.

set(SKATE3_TESTS
    character_fade_test
    hall_of_meat_test
    screen_fx_test
)

foreach(test_name IN LISTS SKATE3_TESTS)
    add_executable(${test_name} ${test_name}.cpp test_check.h)
    target_include_directories(${test_name} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(${test_name} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${test_name} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${test_name} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "unit" TIMEOUT 60)
endforeach()

# All test executables, so CI and local runs can build them in one step
# without building the game: cmake --build <dir> --target skate3_tests
add_custom_target(skate3_tests DEPENDS ${SKATE3_TESTS})
