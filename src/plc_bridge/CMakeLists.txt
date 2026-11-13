add_library(cell_plc_bridge_lib STATIC backoff.cpp conveyor_map.cpp modbus_link.cpp)
target_link_libraries(cell_plc_bridge_lib PUBLIC cell_common PRIVATE PkgConfig::MODBUS)
cell_set_warnings(cell_plc_bridge_lib)

add_executable(plc_bridge main.cpp)
target_link_libraries(plc_bridge PRIVATE cell_plc_bridge_lib)
cell_set_warnings(plc_bridge)

install(TARGETS plc_bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
