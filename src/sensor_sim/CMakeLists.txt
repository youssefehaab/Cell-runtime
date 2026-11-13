add_library(cell_sensor_sim_lib STATIC frame_generator.cpp disconnect_fault.cpp)
target_link_libraries(cell_sensor_sim_lib PUBLIC cell_common)
cell_set_warnings(cell_sensor_sim_lib)

add_executable(sensor_sim main.cpp)
target_link_libraries(sensor_sim PRIVATE cell_sensor_sim_lib)
cell_set_warnings(sensor_sim)

install(TARGETS sensor_sim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
