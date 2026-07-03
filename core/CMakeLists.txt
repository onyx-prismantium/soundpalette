# libsoundpalette: headless core. Must never include/link GLFW, OpenGL, ImGui, or NFD (§1 rule 6).
add_library(soundpalette_core STATIC
    src/version.cpp
)

target_include_directories(soundpalette_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(soundpalette_core PUBLIC cxx_std_20)
sp_set_warnings(soundpalette_core)
