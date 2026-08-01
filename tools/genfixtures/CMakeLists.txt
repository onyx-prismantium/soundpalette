add_executable(genfixtures main.cpp)
target_link_libraries(genfixtures PRIVATE soundpalette_core)
target_include_directories(genfixtures PRIVATE ${PROJECT_SOURCE_DIR}/extern/miniaudio)
sp_set_warnings(genfixtures)
