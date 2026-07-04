FetchContent_MakeAvailable(doctest)

add_executable(sp_unit_tests
    test_placeholder.cpp
    test_decode.cpp
    test_features.cpp
    test_mapping.cpp
    test_manifest.cpp
    test_lint.cpp
    test_describe.cpp
    test_dsp_ops.cpp
    test_propose.cpp
    test_glob.cpp
    test_profile.cpp
    test_deviation.cpp
)
target_link_libraries(sp_unit_tests PRIVATE soundpalette_core doctest::doctest nlohmann_json::nlohmann_json)
sp_set_warnings(sp_unit_tests)

if(MSVC)
    # C5285 fires inside doctest.h itself (v2.4.12 specializes std::tuple internals; warning
    # added in MSVC 14.5x) — third-party noise, same policy as the vendored-code TUs.
    # C4996 flags the tests' standard std::getenv as "unsafe"; _CRT_SECURE_NO_WARNINGS is the
    # conventional opt-out for MSVC's non-standard CRT deprecations.
    target_compile_options(sp_unit_tests PRIVATE /wd5285)
    target_compile_definitions(sp_unit_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(sp_unit_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
