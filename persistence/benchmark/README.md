# Persistence benchmark

This opt-in executable compares the JSON and SQLite adapters on the same machine.
It is not installed or built by the normal `all` target.

```sh
cmake --build build --target benchmark_persistence -j1
for messages in 100 1000 10000; do
  for backend in json sqlite; do
    build/bin/benchmark_persistence "$backend" "$messages" 10
  done
done
```

Use the project's container/toolchain and keep compiler options identical between
backends. Run cases sequentially in separate processes. The fixture contains the
requested number of user/model messages plus a 1 MiB pending tool result. Each
iteration checkpoints the full original, restores and verifies it, performs an
independent current-state read, then archives/compacts to an empty-turn summary
and applies the existing five-archive retention policy. Model/network time is
excluded. Ordinary checkpoints retain only one current graph.

JSON lines report p50/p95 milliseconds for save, restore, storage-only compact,
and read-only inspection; process peak RSS (KiB on Linux); total retained storage;
SQLite database/WAL sizes after operations; and Linux `/proc/self/io` physical
write bytes. Missing procfs reports zero, not a measured absence of writes. WAL
may be zero because short-lived connections checkpoint on close. RSS includes
fixture construction and round-trip verification; equality checks are outside
the restore timer. Reader latency here is uncontended; the real-worker congestion
and compact tests separately exercise control/read responsiveness while running.

The executable owns and removes a unique temporary directory. No credentials,
remote providers, external tools, or existing session files are involved. These
measurements describe one machine/build and do not establish that SQLite is
faster than JSON. Release CI and manual musl validation remain deployment gates;
Debug measurements are diagnostic baselines, not release performance claims.

## PR-G/H reference measurements

Measured on 2026-10-18 in the existing GCC build-base container, limited to two
CPUs and 4 GiB RAM, on the same WSL host and filesystem for both backends.
Release uses `-Os`, LTO disabled; ten sequential iterations per case. This is a
reference measurement, not a guarantee for slower disks or concurrent workloads.

All latencies below are p50 / p95 milliseconds. Compact includes archive T1,
replacement T2 and retention, but excludes model generation.

| Backend | Messages | Save | Restore | Compact | Independent reader | Peak RSS (MiB) | Retained bytes (MiB) | Physical writes (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| json | 100 | 7.8 / 11.2 | 7.8 / 13.2 | 15.9 / 21.9 | 8.6 / 10.7 | 14.2 | 5.3 | 21.1 |
| sqlite | 100 | 15.1 / 18.3 | 7.3 / 8.9 | 37.0 / 41.6 | 7.7 / 12.0 | 13.2 | 7.5 | 77.2 |
| json | 1000 | 12.3 / 13.6 | 14.9 / 16.7 | 18.7 / 22.0 | 14.7 / 18.8 | 17.8 | 7.5 | 29.9 |
| sqlite | 1000 | 43.0 / 58.5 | 57.7 / 66.6 | 103.3 / 136.4 | 51.9 / 69.1 | 15.3 | 10.0 | 116.8 |
| json | 10000 | 73.5 / 81.4 | 88.3 / 102.0 | 86.7 / 89.6 | 95.3 / 104.2 | 61.3 | 29.6 | 118.4 |
| sqlite | 10000 | 359.1 / 446.9 | 565.4 / 756.4 | 950.8 / 1119.1 | 606.0 / 795.9 | 63.3 | 35.0 | 459.0 |

SQLite main DB sizes are 7,827,456 / 10,444,800 / 36,700,160 bytes respectively;
the measured final WAL is zero after connection close, not a bound on peak WAL.
Storage includes five retained original snapshots and the current summary. Write
volume covers all ten iterations. Page reuse keeps retained logical snapshots
bounded, but pruning does not promise to shrink the database file.

SQLite is **not faster than JSON** on these fixtures and writes substantially
more bytes. The normalized codec currently rewrites the full current graph and
prepares statements per decoded message. Optimizing those costs is future work;
this delivery does not claim incremental checkpointing or lower write amplification.

The first integration measurements exposed repeated message-table scans: on the
1,000-message fixture, SQLite restore p95 was 243.5 ms and compact p95 was 916.3 ms.
Step queries now state the existing partial-index predicate, and writable opens
add two optional owner indexes for FK cascades. Readers still accept older v1
files without creating indexes; store/codec versions and data remain unchanged.
The regression checks indexed query plans and read-only compatibility, rather
than imposing unstable wall-clock assertions in general CI.

Reference-host review budgets for the 10,000-message case are p95 save < 1 s,
restore and independent read < 1.5 s, compact storage < 2 s, and peak RSS < 128 MiB.
Both backends meet them. These deliberately allow host variation; changing the
fixture/build/hardware requires a new baseline rather than treating them as SLAs.
Separately, `test_core_congestion` runs both backends under blocked output and a
query flood and requires model cancellation within 3 s; it also verifies that
all 40 tool steps survive persistence. Storage stays on its serialized executor,
not the network executor. No transaction spans the model wait, and mandatory
checkpoint ordering is unchanged.

Deployment acceptance still requires the PR's Debug/Release and portable platform
jobs plus the manually triggered musl workflow. A local benchmark is not evidence
that those other environments passed.
