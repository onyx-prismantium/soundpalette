add_executable(soundpalette main.cpp library_cmd.cpp)
target_link_libraries(soundpalette PRIVATE soundpalette_core nlohmann_json::nlohmann_json)
sp_set_warnings(soundpalette)
