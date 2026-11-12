configure_file(version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/common/version.hpp @ONLY)

# Sources are globbed so new internals (ring buffer, histogram, ...) need no edit here.
file(GLOB CELL_COMMON_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_library(cell_common STATIC ${CELL_COMMON_SRCS})
target_include_directories(cell_common PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(cell_common PUBLIC
  cell_proto cell_zmq spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads
  PRIVATE CLI11::CLI11)
if(SYSTEMD_FOUND)
  target_link_libraries(cell_common PRIVATE PkgConfig::SYSTEMD)
  target_compile_definitions(cell_common PRIVATE CELL_HAVE_SYSTEMD=1)
endif()
cell_set_warnings(cell_common)
