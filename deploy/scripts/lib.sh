# Shared helpers for install.sh / upgrade.sh / rollback.sh / healthcheck.sh.
# Sourced, not executed.
#
# Layout under $CELL_PREFIX (default /opt/cell-runtime):
#   releases/<version>/   immutable release trees (bin/, etc/cell-runtime/, python/, VERSION)
#   current  -> releases/<version>   the release the services run from
#   previous -> releases/<version>   what `current` pointed to before the last switch
#   run/                  pidfiles and logs of the local supervisor
#
# Environment:
#   CELL_PREFIX       install root
#   CELL_SUPERVISOR   systemd | local | auto (default auto: systemd when PID 1 is
#                     systemd, otherwise the local `cellctl up` supervisor)
#   CELL_PYTHON       interpreter for cellctl / plc_sim when a release has no venv
#   CELL_HEALTH_WAIT_MS  how long a freshly (re)started cell may take to become healthy

set -euo pipefail

CELL_PREFIX="${CELL_PREFIX:-/opt/cell-runtime}"
CELL_SUPERVISOR="${CELL_SUPERVISOR:-auto}"
CELL_HEALTH_WAIT_MS="${CELL_HEALTH_WAIT_MS:-20000}"
CELL_UNITS=(cell-plc-sim cell-arm-sim cell-sensor-sim cell-plc-bridge cell-broker)

log() { printf '[%s] %s\n' "$(basename "$0")" "$*" >&2; }
die() { log "error: $*"; exit 1; }

releases_dir() { echo "$CELL_PREFIX/releases"; }

# Version a symlink (current/previous) points to; empty if it does not exist.
link_version() {
  local link="$CELL_PREFIX/$1"
  [[ -L "$link" ]] || return 0
  basename "$(readlink "$link")"
}

# Atomically repoint $CELL_PREFIX/<name> at releases/<version>: build the new
# link next to the old one and rename(2) over it, so there is never a moment
# with no `current` and a crash mid-switch leaves the old link intact.
switch_link() {
  local name="$1" version="$2"
  [[ -d "$(releases_dir)/$version" ]] || die "release '$version' is not installed"
  ln -sfn "releases/$version" "$CELL_PREFIX/.$name.tmp"
  mv -T "$CELL_PREFIX/.$name.tmp" "$CELL_PREFIX/$name"
}

supervisor_mode() {
  case "$CELL_SUPERVISOR" in
    systemd|local) echo "$CELL_SUPERVISOR" ;;
    auto)
      if [[ -d /run/systemd/system ]] && command -v systemctl >/dev/null; then
        echo systemd
      else
        echo local
      fi ;;
    *) die "CELL_SUPERVISOR must be systemd, local or auto" ;;
  esac
}

cell_stop() {
  case "$(supervisor_mode)" in
    systemd) systemctl stop cell.target "${CELL_UNITS[@]/%/.service}" ;;
    local)
      local pidfile="$CELL_PREFIX/run/supervisor.pid" pid
      [[ -f "$pidfile" ]] || return 0
      pid="$(cat "$pidfile")"
      if kill -TERM "$pid" 2>/dev/null; then
        for _ in $(seq 100); do
          kill -0 "$pid" 2>/dev/null || break
          sleep 0.1
        done
        kill -KILL "$pid" 2>/dev/null || true
      fi
      rm -f "$pidfile" ;;
  esac
}

cell_start() {
  case "$(supervisor_mode)" in
    systemd)
      systemctl daemon-reload
      systemctl start cell.target ;;
    local)
      local cur="$CELL_PREFIX/current" run="$CELL_PREFIX/run"
      mkdir -p "$run"
      CELL_CONFIG_DIR="$cur/etc/cell-runtime" nohup "$cur/bin/cellctl" up \
        --bin-dir "$cur/bin" --config-dir "$cur/etc/cell-runtime" --run-dir "$run" \
        >>"$run/supervisor.log" 2>&1 &
      echo $! >"$run/supervisor.pid" ;;
  esac
}

# Exit status 0 iff the running cell reports healthy within CELL_HEALTH_WAIT_MS.
cell_healthy() {
  "$CELL_PREFIX/current/bin/cellctl" health --wait-healthy-ms "$CELL_HEALTH_WAIT_MS" >&2
}
