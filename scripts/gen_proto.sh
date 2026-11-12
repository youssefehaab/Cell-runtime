#!/usr/bin/env bash
# Generate the Python protobuf bindings without running the CMake build.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
protoc --python_out="$root/python/cell_proto" -I "$root/proto" "$root/proto/cell.proto"
echo "generated $root/python/cell_proto/cell_pb2.py"
