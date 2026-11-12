cmake_minimum_required(VERSION 3.22)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CELL_VERSION LIMIT_COUNT 1)
project(cell_runtime VERSION ${CELL_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CELL_BUILD_TESTS "Build unit tests" ON)
option(CELL_BUILD_BENCH "Build benchmarks" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(Sanitizers)
include(Warnings)
include(Dependencies)

# All binaries land in build/bin so scripts and tests can find them in one place.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(proto)
add_subdirectory(src/common)

# Modules are picked up as they appear; each owns its own CMakeLists.txt.
# Convention: testable logic lives in a static library named cell_<module>_lib.
foreach(module arm_sim sensor_sim plc_bridge broker algo)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${module}/CMakeLists.txt")
    add_subdirectory(src/${module})
  endif()
endforeach()

if(CELL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/unit)
endif()

if(CELL_BUILD_BENCH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt")
  add_subdirectory(bench)
endif()

install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/cell-runtime)
install(FILES VERSION DESTINATION .)
