FetchContent_MakeAvailable(doctest)

add_executable(sp_unit_tests
    test_placeholder.cpp
)
target_link_libraries(sp_unit_tests PRIVATE soundpalette_core doctest::doctest)
sp_set_warnings(sp_unit_tests)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(sp_unit_tests)
