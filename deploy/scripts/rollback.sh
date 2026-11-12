#!/usr/bin/env bash
# Switch the cell back to the previous release (or to a named one) and restart it.
#
#   rollback.sh [version]
#
# Exit 0 iff the cell is healthy on the release rolled back to.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

target="${1:-$(link_version previous)}"
[[ -n "$target" ]] || die "no previous release recorded and no version given"
from="$(link_version current)"
[[ "$target" != "$from" ]] || die "already running $target"

log "rolling back: $from -> $target"
cell_stop
switch_link current "$target"
[[ -z "$from" ]] || switch_link previous "$from"
cell_start
if cell_healthy; then
  log "rollback complete: running $target, healthy"
else
  die "cell is NOT healthy after rolling back to $target - manual intervention required"
fi
