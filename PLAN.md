# cell-runtime — Build Plan for Claude Code

> Place this file at the repo root as `PLAN.md`. Start Claude Code with:
> “Read PLAN.md fully. Execute it phase by phase. Do not start a phase until the previous phase builds cleanly and all tests pass. After each phase, commit, then summarise what you built and explain the key design decisions to me.”

-----

## 1. Goal

Build a **Linux runtime for an industrial robot cell** in modern C++ with Python tooling. It simulates the controller-side software of an industrial robot: device processes, low-latency inter-process communication, a high-performance data pipeline, industrial-device integration, diagnostics, and production-style deployment with upgrades and rollback.

Target audience: hiring engineers for a robotics/backend C++ role. The repo must demonstrate:

- Modern C++ (C++20) and Python on Linux
- Low-latency IPC with ZeroMQ
- Concurrency, async I/O, lock-free data structures
- Sensor and industrial-device (Modbus TCP) integration
- Measured performance (p50/p99/p999 latency, throughput)
- Diagnostics and operational tooling
- Provisioning, upgrades, rollback, fault recovery
- Engineering quality: tests, sanitizers, CI, clear docs

Everything must run on a single Linux machine or via Docker Compose. No real hardware.

-----

## 2. Tech stack (fixed — do not substitute)

**C++**

- C++20, CMake >= 3.22, GCC 12+ or Clang 15+
- ZeroMQ: libzmq + cppzmq (header-only C++ bindings)
- Protobuf for all messages
- libmodbus (Modbus TCP client)
- spdlog for logging, CLI11 for command-line args, nlohmann/json for config
- GoogleTest for tests, Google Benchmark for micro-benchmarks
- Dependencies via CMake FetchContent where possible; system packages for libzmq, protobuf, libmodbus

**Python (3.11+)**

- pyzmq, protobuf, rich (terminal UI), typer (CLI), pymodbus (simulated PLC server), pytest

**Ops**

- systemd unit files, Docker, Docker Compose, GitHub Actions

**Code rules**

- No raw `new`/`delete`. RAII everywhere. Smart pointers only where ownership demands it.
- Compile with `-Wall -Wextra -Wpedantic -Werror`
- clang-format (Google style, 100 cols) and clang-tidy config committed
- Every process handles SIGINT/SIGTERM with graceful shutdown
- Every process publishes a heartbeat
- Configuration via JSON file + CLI overrides, never hard-coded values

-----

## 3. Architecture

### Processes

|Process     |Language|Role                                                                                                                                                                                                   |
|------------|--------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
|`arm_sim`   |C++     |Simulated 6-axis arm. Publishes joint states at 1 kHz. Accepts commands (move_to, stop, estop, reset). Simple trapezoidal velocity profile toward target joints.                                       |
|`sensor_sim`|C++     |Simulated depth sensor. Publishes point-cloud frames (configurable: default 640x480 points, 15 fps). Can simulate disconnects.                                                                         |
|`plc_bridge`|C++     |Modbus TCP client to the simulated PLC. Polls conveyor registers (speed, item_present, fault) at 100 Hz, publishes state, forwards commands. Reconnects with exponential backoff.                      |
|`plc_sim`   |Python  |pymodbus TCP server simulating a conveyor PLC with registers and coils.                                                                                                                                |
|`broker`    |C++     |Subscribes to all device streams, pushes into an SPSC ring buffer, processing thread computes latency stats, downsamples, aggregates cell state, republishes for consumers, routes commands to devices.|
|`cellctl`   |Python  |Diagnostics CLI: live dashboard, health, latency stats, logs, send commands, fault injection.                                                                                                          |

### ZeroMQ endpoints (all configurable)

|Endpoint                       |Pattern|Owner                                                              |
|-------------------------------|-------|-------------------------------------------------------------------|
|`ipc:///tmp/cell/arm.telemetry`|PUB    |arm_sim                                                            |
|`ipc:///tmp/cell/arm.cmd`      |REP    |arm_sim                                                            |
|`ipc:///tmp/cell/sensor.frames`|PUB    |sensor_sim                                                         |
|`ipc:///tmp/cell/plc.telemetry`|PUB    |plc_bridge                                                         |
|`ipc:///tmp/cell/plc.cmd`      |REP    |plc_bridge                                                         |
|`tcp://*:5556`                 |XPUB   |broker (aggregated state + stats to consumers)                     |
|`tcp://*:5557`                 |ROUTER |broker (external command entry point, routes to device REP sockets)|

Use ipc for device↔broker (lowest latency on one host) and tcp for consumers. Document this choice.

### Messages (`proto/cell.proto`)

- `Header { uint64 seq; uint64 send_mono_ns; string source; }` on every message
- `JointState { Header h; repeated double position; repeated double velocity; ArmMode mode; }`
- `PointCloudFrame { Header h; uint32 width; uint32 height; bytes points; }` (packed float32 xyz)
- `ConveyorState { Header h; float speed; bool item_present; uint32 fault_code; }`
- `Heartbeat { Header h; ProcessStatus status; uint64 uptime_ms; }`
- `Command { Header h; string target; oneof { MoveTo, Stop, EStop, Reset, SetConveyorSpeed } }`
- `CommandReply { Header h; bool ok; string error; }`
- `CellStats { Header h; repeated StreamStats streams; }` with per-stream p50/p99/p999 latency, msg rate, drops, sequence gaps

Latency = broker receive `CLOCK_MONOTONIC` − `send_mono_ns` (valid because same host). Sequence gaps detect drops.

### Broker internals

- I/O thread: zmq poller over all SUB sockets, deserialises header only, pushes into lock-free **SPSC ring buffer** (power-of-two capacity, cache-line padded head/tail, acquire/release atomics).
- Processing thread: pops, records latency into a log-linear histogram (implement in `common/`), tracks sequence gaps, downsamples JointState 1 kHz → 50 Hz for consumers, aggregates CellStats every 1 s.
- Ring buffer full → count and report drops; never block the I/O thread.

-----

## 4. Repo structure

```
cell-runtime/
├── CMakeLists.txt
├── cmake/                 # dependency + warnings + sanitizer helpers
├── proto/cell.proto
├── config/                # default JSON configs per process
├── src/
│   ├── common/            # ring buffer, histogram, clock, logging, config, signal handling, zmq wrappers
│   ├── arm_sim/
│   ├── sensor_sim/
│   ├── plc_bridge/
│   └── broker/
├── python/
│   ├── plc_sim/
│   └── cellctl/
├── tests/
│   ├── unit/              # GoogleTest
│   └── integration/       # pytest, launches real processes
├── bench/                 # Google Benchmark + end-to-end latency bench
├── deploy/
│   ├── systemd/
│   ├── docker/
│   ├── docker-compose.yml
│   └── scripts/           # install.sh, upgrade.sh, rollback.sh, healthcheck.sh
├── docs/
│   ├── architecture.md
│   ├── design-decisions.md
│   └── benchmarks.md
├── .github/workflows/ci.yml
├── .clang-format
├── .clang-tidy
└── README.md
```

-----

## 5. Phases and acceptance criteria

### Phase 0 — Foundation (main agent only, sequential)

- CMake skeleton, warnings, sanitizer options (`-DSANITIZE=address|thread|undefined`)
- `proto/cell.proto` complete and generating C++ and Python code
- `common/`: config loader, logging setup, signal handling, monotonic clock, zmq socket RAII helpers
- **Accept:** `cmake --build` succeeds; empty test target runs green. Proto and `common/` interfaces are now frozen contracts for other agents.

### Phase 1 — Core data structures

- SPSC ring buffer + log-linear latency histogram in `common/`
- **Accept:** unit tests incl. multi-threaded stress test passing under TSan; Google Benchmark showing ring buffer ops/sec.

### Phase 2 — Device processes

- `arm_sim`, `sensor_sim`, `plc_sim` (Python), `plc_bridge`
- **Accept:** each runs standalone, publishes correct messages + heartbeats, handles commands, shuts down cleanly on SIGTERM. `plc_bridge` survives `plc_sim` restart via backoff reconnect.

### Phase 3 — Broker

- I/O thread, ring buffer, processing thread, downsampling, command routing, CellStats
- **Accept:** with all processes running for 60 s: zero sequence gaps at default rates, stats published every second, commands routed end-to-end with replies.

### Phase 4 — cellctl

- Commands: `cellctl status` (live rich dashboard), `cellctl health [--json]` (exit code 0/1), `cellctl stats`, `cellctl cmd arm move-to ...`, `cellctl cmd plc speed 0.5`, `cellctl fault kill <process>|disconnect-sensor|plc-down`
- **Accept:** all commands work against running system; `health --json` usable by scripts.

### Phase 5 — Ops and deployment

- systemd units: `Restart=on-failure`, `WatchdogSec` with sd_notify from each C++ process, dependency ordering
- Dockerfiles (multi-stage, slim runtime image) + docker-compose for the full cell
- `install.sh`, `upgrade.sh`, `rollback.sh`: releases in `/opt/cell-runtime/releases/<version>`, `current` symlink, upgrade runs `cellctl health` and **automatically rolls back** on failure
- **Accept:** `docker compose up` runs the full cell; a deliberately broken release triggers automatic rollback (demonstrated by integration test or script).

### Phase 6 — Testing, benchmarks, CI

- pytest integration tests: start system, send commands, assert replies, kill a process and assert recovery, assert zero gaps under nominal load
- End-to-end latency bench: report p50/p99/p999 per stream at default and 2x/5x load; record results in `docs/benchmarks.md` with machine specs
- CI matrix: GCC + Clang, Release, ASan+UBSan, TSan; run unit + integration tests; clang-format check
- **Accept:** CI green on all jobs.

### Phase 7 — Documentation

- README: one-paragraph pitch, architecture diagram (Mermaid), quickstart (Docker Compose in 3 commands), demo of `cellctl status`, benchmark table, link to design decisions
- `design-decisions.md`: ipc vs tcp, SPSC ring buffer and memory ordering, why drop instead of block, downsampling, Protobuf choice, backoff strategy, rollback design, what would change for real hardware
- **Accept:** a stranger can clone, run, and understand the system in 10 minutes.

### Optional Phase 8 — Algorithms bonus

- `src/algo/`: greedy pallet-packing heuristic (boxes onto a pallet, maximise fill, respect stacking) + axis-aligned bounding-box collision check
- Python bindings via pybind11, small Python demo that visualises a packed pallet
- **Accept:** unit tests + benchmark.

-----

## 6. Agent plan

Phase 0 is done by the **main agent alone**: it freezes the proto and `common/` interfaces. After that, subagents work in parallel in **separate git worktrees**, each owning its directories only.

|Agent                  |Owns                                                                               |Phases            |
|-----------------------|-----------------------------------------------------------------------------------|------------------|
|**Main / orchestrator**|Root CMake, `proto/`, `common/` interfaces, merges, final review                   |0, coordination   |
|**cpp-core**           |`common/` internals (ring buffer, histogram), `broker/`                            |1, 3              |
|**devices**            |`arm_sim/`, `sensor_sim/`, `plc_bridge/`, `python/plc_sim/`                        |2                 |
|**python-tooling**     |`python/cellctl/`, `tests/integration/`                                            |4, 6 (integration)|
|**devops**             |`deploy/`, Dockerfiles, `.github/workflows/`                                       |5, 6 (CI)         |
|**qa-reviewer**        |Read-only review: sanitizer runs, benchmarks, code review against rules in §2, docs|6 (bench), 7      |

Parallel order after Phase 0:

1. cpp-core (Phase 1) ‖ devices (Phase 2) ‖ devops (Dockerfiles + CI skeleton)
1. cpp-core (Phase 3) once Phase 1 merges ‖ python-tooling (Phase 4) against device processes
1. devops (Phase 5) ‖ python-tooling (integration tests)
1. qa-reviewer (Phase 6 benchmarks, full review) → main agent (Phase 7 docs)

Rules for all agents:

- Never edit files outside owned directories; request interface changes from the main agent.
- Every change must build and pass tests before handing back.
- Commit messages: `phase-N(<area>): <summary>`.

-----

## 7. Definition of done

- `docker compose up` brings up the full cell; `cellctl status` shows live data
- All CI jobs green, including TSan and ASan
- Benchmarks documented with real numbers
- Fault injection: killed process restarts, PLC outage recovers, bad upgrade rolls back
- README and design decisions complete
