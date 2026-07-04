FetchContent_MakeAvailable(doctest)

add_executable(sp_unit_tests
    test_placeholder.cpp
    test_decode.cpp
    test_features.cpp
    test_mapping.cpp
    test_manifest.cpp
    test_lint.cpp
)
target_link_libraries(sp_unit_tests PRIVATE soundpalette_core doctest::doctest nlohmann_json::nlohmann_json)
sp_set_warnings(sp_unit_tests)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(sp_unit_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
