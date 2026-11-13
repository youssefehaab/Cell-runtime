# Palletising algorithms: pure C++ with no dependency on the runtime, plus
# optional Python bindings.
add_library(cell_algo STATIC pallet_packer.cpp)
target_include_directories(cell_algo PUBLIC ${CMAKE_SOURCE_DIR}/src)
cell_set_warnings(cell_algo)

option(CELL_BUILD_PYTHON_BINDINGS "Build the cell_algo Python module (pybind11)" ON)
# An instrumented extension module cannot be loaded into an uninstrumented
# Python interpreter, so sanitizer builds skip the bindings.
if(CELL_BUILD_PYTHON_BINDINGS AND NOT SANITIZE)
  find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
  if(Python3_Development.Module_FOUND)
    include(FetchContent)
    FetchContent_Declare(pybind11
      GIT_REPOSITORY https://github.com/pybind/pybind11.git GIT_TAG v2.13.6 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(pybind11)
    pybind11_add_module(cell_algo_py MODULE bindings.cpp)
    target_link_libraries(cell_algo_py PRIVATE cell_algo)
    # Importable as `cell_algo` with PYTHONPATH=<build>/python.
    set_target_properties(cell_algo_py PROPERTIES
      OUTPUT_NAME cell_algo
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python)
  else()
    message(STATUS "Python development headers not found: skipping cell_algo Python bindings")
  endif()
endif()
