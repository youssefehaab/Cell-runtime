include(GoogleTest)

# One binary for all unit tests; any tests/unit/*_test.cpp is picked up.
file(GLOB CELL_UNIT_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)

add_executable(cell_unit_tests ${CELL_UNIT_TEST_SRCS})
target_link_libraries(cell_unit_tests PRIVATE cell_common GTest::gtest GTest::gmock GTest::gtest_main)
foreach(lib cell_arm_sim_lib cell_sensor_sim_lib cell_plc_bridge_lib cell_broker_lib cell_algo)
  if(TARGET ${lib})
    target_link_libraries(cell_unit_tests PRIVATE ${lib})
  endif()
endforeach()
cell_set_warnings(cell_unit_tests)

gtest_discover_tests(cell_unit_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
