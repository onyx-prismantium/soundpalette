# soundpalette-app: Dear ImGui (docking) + GLFW + OpenGL 3 desktop shell (§10).
# Links core + UI libraries; all analysis and glyph geometry comes from core (§1 rule 6).

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw nfd imgui)

find_package(OpenGL REQUIRED)

# Dear ImGui ships no CMakeLists; build it (and its GLFW/GL3 backends) as a static lib.
# Third-party source: deliberately not built with our -Wall -Wextra -Werror (same policy as
# the vendored audio implementation, see core/CMakeLists.txt and NOTES.md M1).
add_library(sp_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(sp_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_features(sp_imgui PUBLIC cxx_std_20)
target_link_libraries(sp_imgui PUBLIC glfw OpenGL::GL)

# Vendored stb_image_write implementation, isolated from our warning flags (same policy as
# core's soundpalette_vendor_audio).
add_library(sp_vendor_stb OBJECT gui/vendor_stb_impl.cpp)
target_include_directories(sp_vendor_stb PRIVATE ${PROJECT_SOURCE_DIR}/extern/stb)

# GUI component library: every view except the executable's main().  External apps
# (SoundPalette Studio) link this to embed the grid/inspector/tuner/constellation/
# harmonize/manual views and the AppState machinery.  Include dirs are PUBLIC so
# consumers get gui/, miniaudio and stb transitively.
add_library(soundpalette_gui STATIC
    gui/app.cpp
    gui/grid.cpp
    gui/inspector.cpp
    gui/tuner.cpp
    gui/harmonize_panel.cpp
    gui/constellation.cpp
    gui/manual.cpp
    $<TARGET_OBJECTS:sp_vendor_stb>
)
target_include_directories(soundpalette_gui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/extern/miniaudio  # playback via ma_engine (implementation lives in core)
    ${PROJECT_SOURCE_DIR}/extern/stb        # stb_image_write for --smoke PNG
)
target_link_libraries(soundpalette_gui PUBLIC soundpalette_core sp_imgui glfw OpenGL::GL nfd
                      nlohmann_json::nlohmann_json)
sp_set_warnings(soundpalette_gui)

add_executable(soundpalette-app main.cpp)
target_link_libraries(soundpalette-app PRIVATE soundpalette_gui)
sp_set_warnings(soundpalette-app)
if(MSVC)
    # std::getenv (SP_UI_SCALE override) trips MSVC's non-standard C4996 CRT deprecation;
    # same conventional opt-out as the unit-test target.
    target_compile_definitions(soundpalette_gui PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(soundpalette-app PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
