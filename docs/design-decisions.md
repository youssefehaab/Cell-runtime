# Design decisions

Each section states the decision, the reason, and what it costs.

## ipc for devices, tcp for consumers

Device processes talk to the broker over ZeroMQ `ipc://` (Unix domain sockets);
consumers such as `cellctl` talk to the broker over `tcp://`.

- **Why ipc inside the cell.** Devices and broker always share a host. A Unix
  socket skips the TCP/IP stack (no checksums, no Nagle, no loopback routing)
  and gives the lowest latency ZeroMQ offers between processes. It also keeps
  the device sockets off the network entirely: nothing outside the host can
  publish fake telemetry or send a command straight to the arm.
- **Why tcp at the edge.** Dashboards and tools may run on another machine or in
  another container. The broker is the single network-facing surface, which is
  where authentication and rate limiting would go.
- **Cost.** Socket files need a shared directory (`/tmp/cell`; a shared volume
  under Compose), and Unix socket paths are limited to about 107 bytes.

`inproc://` was not an option (separate processes are the point: a crashing
device must not take the broker down) and shared memory was not justified by
the numbers: the ipc hop already costs ~150 µs at the median.

## Protobuf for every message

- **Why.** One schema file gives typed bindings for C++ and Python, and field
  numbers give forward and backward compatibility, which matters as soon as
  devices and tools are upgraded at different times.
- **The header trick.** `Header` is field 1 of every message, so the broker can
  decode just that field off the wire (`parse_header_only`) and timestamp a
  3.69 MB point cloud without parsing it. Sensor frame metadata is read the same
  way, stepping over the `points` blob by its length prefix.
- **Cost.** Serialisation is a copy. For the point cloud that is one 3.69 MB
  copy per frame on the sender, visible as ~1.5 ms in the sensor latency.
  FlatBuffers or a raw shared-memory frame would remove it; not worth the
  second serialisation system at 15 fps.

## SPSC ring buffer and memory ordering

The broker's I/O thread hands messages to the processing thread through
`cell::SpscRing` (`src/common/spsc_ring.hpp`).

- **Why single-producer/single-consumer.** There is exactly one writer and one
  reader, so no CAS loop and no lock is needed: both operations are wait-free.
  Measured at 1.5 ns per item against 10 ns for a mutex-protected deque, and
  75× faster under two-thread contention.
- **Indices.** 64-bit counters that only increase, masked on access. Full is
  `tail - head == capacity`, so every slot is usable and there is no ambiguous
  full/empty state.
- **Memory ordering.** Each side loads its own index `relaxed` (it is the only
  writer), loads the other side's index `acquire`, and publishes with `release`.
  The release/acquire pair is what makes the element's construction visible
  before the consumer reads it, and the slot's destruction visible before the
  producer reuses it. Nothing needs `seq_cst`.
- **False sharing.** `head` and `tail` sit on separate 64-byte cache lines, and
  each side keeps a private snapshot of the other's index, so the contended line
  is only read when the ring looks full or empty.
- **Payloads.** Items own a `zmq::message_t` that is moved in, so the bytes are
  never copied between threads.

## Drop instead of block

When the ring is full the I/O thread counts a drop and discards the message.

- **Why.** Telemetry is only useful fresh. Blocking would push the backlog into
  the ZeroMQ queues and then into the devices, turning one slow consumer into a
  stalled control loop — a 1 kHz arm loop that waits on a dashboard. Dropping
  keeps latency bounded and makes the overload visible.
- **How it stays honest.** Drops are counted per stream at the ring, and
  sequence gaps are counted independently from the headers, so loss anywhere
  between publisher and processing thread shows up in `cell.stats`. Publisher
  restarts (sequence going backwards) are counted separately, not as loss.
- **In practice.** At 5× load the ring peaked at 58 of 65,536 slots, so drops
  only happen if the processing thread stalls.

Commands are the opposite case and are never dropped silently: each gets a
reply or an explicit timeout error.

## Downsampling

The broker republishes `arm.joint` and `plc.conveyor` at 50 Hz instead of 1 kHz
and 100 Hz.

- **Why.** A dashboard cannot use 1,000 updates a second, and tcp consumers
  should not be able to load the broker in proportion to device rates.
- **How.** Latest-value sampling on a fixed 20 ms grid: only the newest message
  is kept, forwarded byte-for-byte (original header intact, so a consumer can
  still compute end-to-end latency), and only if something new arrived. Nothing
  is queued, so no backlog can form.
- **Not averaging.** An averaged joint position is a pose the arm was never in.
- **Point clouds** are not republished at all; consumers get `FrameInfo`
  metadata. Shipping 55 MB/s over tcp to a status tool would be the cell's
  largest load for no benefit.

Statistics are computed on the full-rate stream before downsampling.

## Latency measurement

Latency is `broker receive time − Header.send_mono_ns`, both `CLOCK_MONOTONIC`.

- **Why it is valid.** All processes read the same kernel clock, including
  containers on one host. No clock synchronisation is involved.
- **Histogram.** Log-linear buckets (32 per power of two) cover the full 64-bit
  range in 15 KiB with at most 3.1 % relative error; recording is one array
  increment (2.3 ns). Percentiles report the bucket's upper bound, so they are
  never optimistic.
- **Limit.** Across hosts this would need PTP or hardware timestamps.

## Reconnect and backoff

`plc_bridge` reconnects to the PLC with exponential backoff and jitter
(100 ms initial, ×2, capped at 5 s, ±20 %).

- **Why exponential.** A PLC that is rebooting should not be hit with a connect
  attempt every 10 ms; a brief glitch should still recover in ~100 ms.
- **Why jitter.** Several clients that lost the same PLC at the same moment
  would otherwise retry in lockstep.
- **Non-blocking.** The wait is a state in the main loop, not a sleep. While the
  link is down the bridge still answers commands (with an error), publishes
  `ConveyorState` with `link_up=false`, and heartbeats `DEGRADED`.
- **Reset on success**, so the next outage starts again from 100 ms.

## Command routing

- **One request in flight per device.** A REP socket's reply carries nothing
  that identifies the request, so the only safe correlation is to have exactly
  one candidate. Further requests queue with their own deadlines.
- **Timeout recreates the socket.** After a timeout, the unsent request and any
  late reply are discarded with the old socket. Otherwise a command could
  execute minutes later, after the operator was told it failed, or a late reply
  could be handed to the wrong client.
- **Own thread.** Command handling never competes with the telemetry path.

## Health and supervision

- **Heartbeats come from the main loop** of each process, on the socket that
  loop owns. A stalled loop stops heartbeating; a helper thread would keep
  reporting a dead process as alive.
- **Watchdog.** The same loop kicks the systemd watchdog. In the broker the kick
  is gated on all three worker threads having advanced.
- **`cellctl health`** exits 0 only when every process is alive and `OK`. It is
  the single health gate used by Compose, by the upgrade script and by tests.

## Upgrades and rollback

```
/opt/cell-runtime/
  releases/1.0.0/   releases/1.1.0/   ...
  current  -> releases/1.1.0
  previous -> releases/1.0.0
```

- **Immutable releases.** A version directory is written once (copied under a
  temporary name, then renamed) and never modified. Rolling back is therefore
  exact: the old tree is byte-for-byte what ran before.
- **Atomic switch.** `current` is replaced with `rename(2)`, so there is never a
  moment without a valid `current`.
- **Health-gated.** `upgrade.sh` stops the cell, switches, starts, and waits for
  `cellctl health`. If the new release is not healthy in time it switches back,
  restarts, and exits with code 3; code 4 means the rollback is not healthy
  either and a human is needed.
- **The failed release is kept** for inspection.
- **Cost.** Stop-switch-start means a few seconds of downtime per upgrade. A
  robot cell is stopped for upgrades anyway; a rolling upgrade would need
  version-skew guarantees between devices and broker.

## Pallet packing

`src/algo/` packs boxes onto a pallet with a greedy extreme-point heuristic.

- **Why greedy.** 3D bin packing is NP-hard; a cell needs an answer in
  microseconds per box and a deterministic one. Sorting sturdiest-first (stacking
  limit, then footprint, then weight) and taking the lowest free corner gives
  dense, stable layers: 90 % fill on the demo mix, 1.1 ms for 200 boxes.
- **Stacking is modelled as load, not as a rule of thumb.** Each box has a
  weight it can carry. A new box's weight is split among the boxes under it in
  proportion to contact area and propagated down to the deck; the placement is
  rejected if any box in that chain would exceed its limit.
- **Support.** At least 75 % of a footprint must rest on something (configurable).
- **Collision check.** AABBs with a small tolerance, where touching faces do not
  count as overlap — otherwise neighbouring boxes would collide with each other.
- **Verified by an independent oracle.** `validate()` re-derives every
  constraint without sharing code with `pack()`, and a property test runs it
  over random inputs.
- **Cost.** Not optimal: no look-ahead, no tipping boxes on their side, and no
  centre-of-gravity or interlocking-pattern constraints, which a production
  palletiser would add.

## What would change for real hardware

- **Real-time scheduling.** `SCHED_FIFO` and pinned, isolated cores for the arm
  loop and the broker I/O thread, `mlockall`, and a PREEMPT_RT kernel. The 46 ms
  stall seen in the benchmark is exactly what this removes.
- **Busy-polling** on the I/O thread instead of sleeping in `poll`, trading a
  core for the wake-up latency.
- **Safety.** E-stop would be a hardwired safety circuit and a safety PLC, never
  a message through a broker. The software e-stop here would only mirror it.
- **Fieldbus.** EtherCAT or PROFINET for the arm's cyclic data instead of a
  simulated loop; Modbus TCP stays plausible for a conveyor.
- **Time.** PTP-synchronised clocks and hardware timestamps once sensors are on
  other hosts.
- **Point clouds.** Shared memory or zero-copy transport for the sensor path.
- **Security.** Authenticated, encrypted sockets (CURVE) on the tcp edge and
  authorisation for commands.
- **Config and state.** Calibration data, persistent fault logs, and signed
  release artifacts.
