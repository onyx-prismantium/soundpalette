# libsoundpalette: headless core. Must never include/link GLFW, OpenGL, ImGui, or NFD (§1 rule 6).

# libebur128 is only needed here (audio.cpp); force static (project-wide goal is a single
# static binary per platform) and skip its test suite (needs libsndfile, off by default anyway).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libs" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "libebur128 test suite" FORCE)
FetchContent_MakeAvailable(libebur128)

# Vendored miniaudio + stb_vorbis implementation, isolated in its own object library so the
# vendor code is NOT built with our -Wall -Wextra -Werror (§13.6 cleanliness is about our code).
add_library(soundpalette_vendor_audio OBJECT
    src/vendor_audio_impl.cpp
)
target_include_directories(soundpalette_vendor_audio PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/miniaudio
    ${CMAKE_SOURCE_DIR}/extern/stb
)
target_compile_features(soundpalette_vendor_audio PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(soundpalette_vendor_audio PUBLIC Threads::Threads)

add_library(soundpalette_core STATIC
    src/version.cpp
    src/audio.cpp
    $<TARGET_OBJECTS:soundpalette_vendor_audio>
)

target_include_directories(soundpalette_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(soundpalette_core PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/miniaudio
    ${libebur128_SOURCE_DIR}/ebur128
)

target_compile_features(soundpalette_core PUBLIC cxx_std_20)
sp_set_warnings(soundpalette_core)
target_link_libraries(soundpalette_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(soundpalette_core PRIVATE ebur128)
