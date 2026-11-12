include(FetchContent)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# --- System packages -------------------------------------------------------
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
find_path(CPPZMQ_INCLUDE_DIR zmq.hpp REQUIRED)
pkg_check_modules(MODBUS REQUIRED IMPORTED_TARGET libmodbus)
find_package(Protobuf REQUIRED)

# libsystemd is optional: without it sd_notify calls compile to no-ops.
pkg_check_modules(SYSTEMD IMPORTED_TARGET libsystemd)

add_library(cell_zmq INTERFACE)
target_link_libraries(cell_zmq INTERFACE PkgConfig::ZMQ)
target_include_directories(cell_zmq SYSTEM INTERFACE ${CPPZMQ_INCLUDE_DIR})

# --- FetchContent (pinned) -------------------------------------------------
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.14.1 GIT_SHALLOW TRUE)
FetchContent_Declare(CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git GIT_TAG v2.4.2 GIT_SHALLOW TRUE)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3 GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(spdlog CLI11 nlohmann_json)
cell_mark_system(spdlog)
cell_mark_system(CLI11)
cell_mark_system(nlohmann_json)

if(CELL_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.15.2 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
endif()

if(CELL_BUILD_BENCH)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git GIT_TAG v1.9.0 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(benchmark)
endif()
