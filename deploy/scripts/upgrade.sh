#!/usr/bin/env bash
# Upgrade the cell to a new release, with automatic rollback.
#
#   upgrade.sh <staged-release-dir | installed-version>
#
# Steps: install (if given a staged tree) -> stop -> switch `current` -> start ->
# health gate. If the new release does not become healthy within
# CELL_HEALTH_WAIT_MS, the previous release is restored and restarted.
#
# Exit codes: 0 upgraded and healthy
#             1 usage / precondition error (nothing changed)
#             3 upgrade failed, rolled back to the previous release (healthy)
#             4 upgrade failed AND the rollback is not healthy
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$here/lib.sh"

[[ $# -eq 1 ]] || die "usage: $0 <staged-release-dir | installed-version>"
if [[ -d "$1" ]]; then
  new="$("$here/install.sh" "$1")"
else
  new="$1"
  [[ -d "$(releases_dir)/$new" ]] || die "release '$new' is not installed"
fi
old="$(link_version current)"
[[ -n "$old" ]] || die "nothing is running yet; use install.sh --activate for the first install"
[[ "$new" != "$old" ]] || die "already running $new"

log "upgrading: $old -> $new"
cell_stop
switch_link previous "$old"
switch_link current "$new"
cell_start

if cell_healthy; then
  log "upgrade complete: running $new, healthy"
  exit 0
fi

log "release $new did not become healthy within ${CELL_HEALTH_WAIT_MS} ms - rolling back to $old"
cell_stop
switch_link current "$old"
switch_link previous "$new"   # keep the failed release reachable for inspection
cell_start
if cell_healthy; then
  log "rolled back: running $old, healthy (failed release $new kept in $(releases_dir))"
  exit 3
fi
log "error: rollback to $old is NOT healthy - manual intervention required"
exit 4
