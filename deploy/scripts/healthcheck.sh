#!/usr/bin/env bash
# Exit 0 iff the cell running from $CELL_PREFIX/current is healthy.
# Extra arguments are passed to `cellctl health` (e.g. --json).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
[[ -x "$CELL_PREFIX/current/bin/cellctl" ]] || die "no release installed under $CELL_PREFIX"
exec "$CELL_PREFIX/current/bin/cellctl" health --wait-healthy-ms "$CELL_HEALTH_WAIT_MS" "$@"
