#!/usr/bin/env bash
# clang-format every first-party C++ file. `--check` fails instead of rewriting (used by CI).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
mapfile -t files < <(find src tests bench -type f \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null | sort)
if [[ "${1:-}" == "--check" ]]; then
  clang-format --dry-run --Werror "${files[@]}"
else
  clang-format -i "${files[@]}"
fi
