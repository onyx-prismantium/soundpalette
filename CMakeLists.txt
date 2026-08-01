cmake_minimum_required(VERSION 3.22)
project(soundpalette LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SP_WERROR "Treat compiler warnings as errors" ON)
option(SP_BUILD_APP "Build soundpalette-app (GUI)" ON)
option(SP_BUILD_TESTS "Build unit tests" ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(Warnings)
include(Dependencies)

add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(tools/genfixtures)

if(SP_BUILD_APP)
    add_subdirectory(app)
endif()

if(SP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
