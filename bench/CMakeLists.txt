# Micro-benchmarks for the core data structures. Run from a Release build:
#   build/bin/cell_bench
file(GLOB CELL_BENCH_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_bench.cpp)

add_executable(cell_bench ${CELL_BENCH_SRCS})
target_link_libraries(cell_bench PRIVATE cell_common benchmark::benchmark benchmark::benchmark_main)
cell_mark_system(benchmark)
cell_set_warnings(cell_bench)
if(TARGET cell_algo)
  target_link_libraries(cell_bench PRIVATE cell_algo)
else()
  list(FILTER CELL_BENCH_SRCS EXCLUDE REGEX "algo_bench\\.cpp$")
  set_target_properties(cell_bench PROPERTIES SOURCES "${CELL_BENCH_SRCS}")
endif()
