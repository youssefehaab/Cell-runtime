# All logic lives in the library so the unit tests can link it; main.cpp is only
# the run_app() entry point.
add_library(cell_broker_lib STATIC
  broker.cpp
  broker_config.cpp
  command_queue.cpp
  command_thread.cpp
  frame_scan.cpp
  health_table.cpp
  io_thread.cpp
  liveness.cpp
  processing_thread.cpp
  stream_tracker.cpp)
target_link_libraries(cell_broker_lib PUBLIC cell_common)
cell_set_warnings(cell_broker_lib)

add_executable(broker main.cpp)
target_link_libraries(broker PRIVATE cell_broker_lib)
cell_set_warnings(broker)

install(TARGETS broker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
