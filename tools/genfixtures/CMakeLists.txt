add_executable(genfixtures main.cpp)
target_link_libraries(genfixtures PRIVATE soundpalette_core)
sp_set_warnings(genfixtures)
