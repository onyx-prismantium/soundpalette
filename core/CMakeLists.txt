# libsoundpalette: headless core. Must never include or link any windowing, graphics, or UI
# library (§1 rule 6) — the §13.6 layering grep over core/ must come back empty.

# libebur128 is only needed here (audio.cpp); force static (project-wide goal is a single
# static binary per platform) and skip its test suite (needs libsndfile, off by default anyway).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libs" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "libebur128 test suite" FORCE)
FetchContent_MakeAvailable(libebur128)

# KissFFT: real FFT for STFT features (§4 integration notes).
set(KISSFFT_DATATYPE "float" CACHE STRING "" FORCE)
set(KISSFFT_STATIC ON CACHE BOOL "" FORCE)
set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(kissfft)

# nlohmann/json: manifest serialization, mapping IO (§4).
FetchContent_MakeAvailable(nlohmann_json)

# Vendored miniaudio + stb_vorbis implementation, isolated in its own object library so the
# vendor code is NOT built with our -Wall -Wextra -Werror (§13.6 cleanliness is about our code).
add_library(soundpalette_vendor_audio OBJECT
    src/vendor_audio_impl.cpp
)
target_include_directories(soundpalette_vendor_audio PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/miniaudio
    ${PROJECT_SOURCE_DIR}/extern/stb
)
target_compile_features(soundpalette_vendor_audio PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(soundpalette_vendor_audio PUBLIC Threads::Threads)

# Vendored SHA-256 (extern/sha256, public domain, §12/M3), also isolated from our warning flags.
add_library(soundpalette_vendor_sha256 OBJECT
    ${PROJECT_SOURCE_DIR}/extern/sha256/sha256.c
)
target_include_directories(soundpalette_vendor_sha256 PUBLIC ${PROJECT_SOURCE_DIR}/extern/sha256)

# Vendored SQLite amalgamation (extern/sqlite, public domain, extension-4 §2): FTS5 for the
# library index; extension loading omitted (single static binary, no plugin surface).
add_library(soundpalette_vendor_sqlite OBJECT
    ${PROJECT_SOURCE_DIR}/extern/sqlite/sqlite3.c
)
target_include_directories(soundpalette_vendor_sqlite PUBLIC ${PROJECT_SOURCE_DIR}/extern/sqlite)
target_compile_definitions(soundpalette_vendor_sqlite PRIVATE
    SQLITE_ENABLE_FTS5=1
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_DEPRECATED=1
)
if(MSVC)
    target_compile_options(soundpalette_vendor_sqlite PRIVATE /w)
else()
    target_compile_options(soundpalette_vendor_sqlite PRIVATE -w)
endif()

add_library(soundpalette_core STATIC
    src/version.cpp
    src/audio.cpp
    src/features.cpp
    src/mapping.cpp
    src/manifest.cpp
    src/glyph.cpp
    src/describe.cpp
    src/dsp_ops.cpp
    src/recipe.cpp
    src/propose.cpp
    src/profile.cpp
    src/presets.cpp
    src/deviation.cpp
    src/psycho/loudness_iso532_1.cpp
    src/psycho/sharpness_din45692.cpp
    src/psycho/modulation_frontend.cpp
    src/psycho/psycho.cpp
    src/pca.cpp
    src/lint.cpp
    src/ucs.cpp
    src/ucs_data.cpp
    src/library.cpp
    src/offline_classify.cpp
    src/subprocess.cpp
    src/annotator.cpp
    src/metadata_wav.cpp
    $<TARGET_OBJECTS:soundpalette_vendor_audio>
    $<TARGET_OBJECTS:soundpalette_vendor_sha256>
    $<TARGET_OBJECTS:soundpalette_vendor_sqlite>
)

target_include_directories(soundpalette_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(soundpalette_core PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/miniaudio
    ${PROJECT_SOURCE_DIR}/extern/sha256
    ${PROJECT_SOURCE_DIR}/extern/sqlite
    ${libebur128_SOURCE_DIR}/ebur128
)

target_compile_features(soundpalette_core PUBLIC cxx_std_20)
sp_set_warnings(soundpalette_core)
target_link_libraries(soundpalette_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(MSVC)
    # std::getenv (SP_ANNOTATOR lookup, extension-4 §7.1) trips MSVC's non-standard C4996 CRT
    # deprecation; same conventional opt-out as the app and unit-test targets.
    target_compile_definitions(soundpalette_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_link_libraries(soundpalette_core PRIVATE ebur128 kissfft nlohmann_json::nlohmann_json)
