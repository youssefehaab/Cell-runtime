# Benchmarks

All numbers on this page were measured, not estimated. They come from a small
shared cloud VM with no CPU isolation, no real-time scheduling and no core
pinning, so the tails reflect hypervisor scheduling as much as the software.
Treat them as a floor for what the design does on ordinary hardware, not as what
a tuned controller would achieve.

## End-to-end latency (device publish → broker receive)

Latency is the broker's receive time minus `Header.send_mono_ns`, both
`CLOCK_MONOTONIC` on the same host. It covers serialisation, the ZeroMQ `ipc://`
hop and the broker's poll loop. The whole cell runs (five processes), at the
default rates and with every rate multiplied by 2 and by 5.

Reproduce with:

```
PYTHONPATH=python python bench/e2e_latency.py --seconds 30
```

### Machine

- CPU: Intel(R) Xeon(R) Processor @ 2.10GHz, 4 logical cores
- Memory: 15.7 GiB
- Kernel: 6.18.44-fc-v378 (x86_64)
- Load average at start: 0.29

### Results (30 one-second windows per load level; latencies in µs)

| Load | Stream | Rate (Hz) | Messages | Gaps | Drops | p50 | p99 | p999 | worst p99 | worst p999 | max |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1x | `arm.joint` | 994.9 | 29,846 | 0 | 0 | 172 | 565 | 2770 | 2687 | 46330 | 46330 |
| 1x | `sensor.frame` | 15.0 | 450 | 0 | 0 | 1868 | 2379 | 2379 | 47566 | 47566 | 47566 |
| 1x | `plc.conveyor` | 99.9 | 2,996 | 0 | 0 | 156 | 455 | 1666 | 46444 | 46444 | 46444 |
| 2x | `arm.joint` | 1992.9 | 59,787 | 0 | 0 | 143 | 520 | 1950 | 1409 | 4456 | 5037 |
| 2x | `sensor.frame` | 30.0 | 900 | 0 | 0 | 1638 | 2756 | 2756 | 8003 | 8003 | 8003 |
| 2x | `plc.conveyor` | 200.0 | 6,000 | 0 | 0 | 127 | 401 | 988 | 1704 | 3648 | 3648 |
| 5x | `arm.joint` | 4975.9 | 149,275 | 0 | 0 | 68 | 508 | 1720 | 5505 | 11010 | 11750 |
| 5x | `sensor.frame` | 75.0 | 2,250 | 0 | 0 | 1343 | 2778 | 2778 | 12772 | 12772 | 12772 |
| 5x | `plc.conveyor` | 499.6 | 14,987 | 0 | 0 | 81 | 504 | 1544 | 2064 | 7969 | 7969 |

p50/p99/p999 are the median across windows; "worst" is the worst single window.

- 1x: ring high-water 12 of 65,536 slots
- 2x: ring high-water 10 of 65,536 slots
- 5x: ring high-water 58 of 65,536 slots

### Reading the results

- **No message was lost at any load level**: zero sequence gaps and zero ring
  drops, including 5 kHz joint states alongside 75 fps of 3.69 MB point clouds
  (about 277 MB/s through the broker). The ring never held more than 58 of its
  65,536 slots, so the processing thread keeps up with large headroom.
- **Median latency falls as load rises** (172 µs → 68 µs for `arm.joint`). At
  1 kHz the broker's I/O thread sleeps in `poll` between messages and pays a
  wake-up each time; at 5 kHz it is more often already awake.
- **`sensor.frame` is slower by construction**: its timestamp is taken before a
  3.69 MB frame is serialised and copied into the socket, so ~1.3–1.9 ms of the
  figure is memory bandwidth, not queueing.
- **The worst windows are scheduling stalls, not pipeline behaviour.** In the 1×
  run one window shows ~46 ms on all three streams at once — every process was
  descheduled together. A design fault would show up on one stream or grow with
  load; this does neither (the 5× run's worst p99 is lower).

## Data-structure micro-benchmarks

`build/bin/cell_bench` (Google Benchmark, GCC 13 Release, same machine):

| Benchmark | Time per op | Throughput |
|---|---:|---:|
| `SpscRing` push+pop, one thread | 1.48 ns | 682 M items/s |
| mutex + `std::deque` push+pop, one thread | 10.0 ns | 100 M items/s |
| `SpscRing`, producer and consumer threads | 1.91 ns | 524 M items/s |
| mutex + `std::deque`, two threads | 145 ns | 6.9 M items/s |
| `LatencyHistogram::record` | 2.26 ns | 442 M items/s |
| `LatencyHistogram::percentile(50)` | 92 ns | — |
| `LatencyHistogram::percentile(99.9)` | 128 ns | — |

Palletising (`src/algo/`), random box mixes on a 1.2 × 0.8 × 1.8 m pallet:

| Benchmark | Time | Result |
|---|---:|---|
| AABB intersection test | 2.3 ns | — |
| Pack 10 boxes | 1.8 µs | 10 placed |
| Pack 50 boxes | 63 µs | 37 placed, 51.6 % fill |
| Pack 200 boxes | 1.1 ms | 57 placed, 57.0 % fill |

The two-thread ring figure is a throughput number: both sides yield when the
ring is full or empty, so items move in batches. It is not a per-item hand-off
latency.

## 60-second soak at default rates

64,899 joint states, 975 frames and 6,499 conveyor states in 60 s: zero gaps,
zero drops, 61 stats windows received, ring high-water 4 slots, broker at about
10 % of one core.
