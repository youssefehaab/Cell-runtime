add_library(cell_arm_sim_lib STATIC motion_profile.cpp arm_model.cpp)
target_link_libraries(cell_arm_sim_lib PUBLIC cell_common)
cell_set_warnings(cell_arm_sim_lib)

add_executable(arm_sim main.cpp)
target_link_libraries(arm_sim PRIVATE cell_arm_sim_lib)
cell_set_warnings(arm_sim)

install(TARGETS arm_sim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
