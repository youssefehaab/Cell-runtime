# C++ bindings: a separate library so generated code is exempt from -Werror.
protobuf_generate_cpp(CELL_PROTO_SRCS CELL_PROTO_HDRS cell.proto)
add_library(cell_proto STATIC ${CELL_PROTO_SRCS} ${CELL_PROTO_HDRS})
target_include_directories(cell_proto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(cell_proto PUBLIC protobuf::libprotobuf)

# Python bindings: generated into python/cell_proto/ (git-ignored) on every build.
set(CELL_PROTO_PY_DIR "${CMAKE_SOURCE_DIR}/python/cell_proto")
set(CELL_PROTO_PY "${CELL_PROTO_PY_DIR}/cell_pb2.py")
add_custom_command(
  OUTPUT ${CELL_PROTO_PY}
  COMMAND protobuf::protoc --python_out=${CELL_PROTO_PY_DIR}
          -I ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/cell.proto
  DEPENDS cell.proto
  COMMENT "Generating Python protobuf bindings"
  VERBATIM)
add_custom_target(cell_proto_py ALL DEPENDS ${CELL_PROTO_PY})
