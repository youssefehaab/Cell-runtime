#!/bin/sh
# Container entrypoint: check the ipc:// socket directory, then become the requested
# process (exec, so it is PID 1 and gets SIGTERM straight from the container runtime).
#
#   command: ["arm_sim"]                               # uses $CELL_CONFIG_DIR/arm_sim.json
#   command: ["broker", "--set", "log.level=debug"]
#   command: ["plc-sim"]
#   command: ["cellctl", "health", "--json"]
set -eu

# Default configs put the device<->broker sockets under /tmp/cell; all containers of
# the cell must share that directory (a volume). Override the check with CELL_IPC_DIR
# if the configs point elsewhere.
ipc_dir="${CELL_IPC_DIR:-/tmp/cell}"
if ! mkdir -p "$ipc_dir" 2>/dev/null || [ ! -w "$ipc_dir" ]; then
  echo "cell-entrypoint: warning: $ipc_dir is not writable by uid $(id -u);" \
       "ipc:// endpoints there will fail to bind" >&2
fi

if [ "$#" -eq 0 ]; then
  echo "cell-entrypoint: no process given; available in /opt/cell-runtime/bin:" >&2
  ls /opt/cell-runtime/bin >&2
  exit 64
fi

exec "$@"
