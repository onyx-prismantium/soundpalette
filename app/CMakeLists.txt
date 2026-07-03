# M0 placeholder: no GLFW/OpenGL/ImGui/NFD linked yet.
# Real GUI shell (§10) lands at M6, at which point this file gains the
# FetchContent_MakeAvailable(glfw imgui nfd) calls and the app/gui/ sources.
add_executable(soundpalette-app main.cpp)
target_link_libraries(soundpalette-app PRIVATE soundpalette_core)
sp_set_warnings(soundpalette-app)
