# RMA Feasibility Analysis

## Purpose

Before any code was optimized, the project's real-time feasibility was checked analytically using Rate Monotonic Analysis (RMA), so that architectural decisions (how many threads, whether a sequencer was needed, whether the SD card write had to be isolated) were based on measured worst-case execution time (WCET), not intuition.

## Module 1 baseline measurements (single-threaded reference)

These are the original per-stage WCET figures measured during the Module 1 timing-analysis assignment, on the same target hardware (Pi 3B+, 640×480 capture):

| Stage    | WCET       |
|----------|------------|
| CAPTURE  | 0.130 ms   |
| CONVERT  | 11.966 ms  |
| DIFF     | 2.515 ms   |
| WRITE    | 60.342 ms  |

At the project's operational cadence (1 Hz), utilization:

```
U = Σ (Ci / Ti) ≈ 0.079
```

This is comfortably under the Liu & Layland least-upper-bound for any reasonable task count (`n·(2^(1/n) − 1)`, which approaches ln(2) ≈ 69.3% asymptotically) — so at 1 Hz, feasibility was never in question mathematically. The interesting engineering problem was elsewhere: **WRITE's WCET (60 ms) is two orders of magnitude larger than CAPTURE's (0.13 ms)**, and that asymmetry, not the aggregate utilization, is what drove the architecture (see `docs/02`).

## Multi-threaded pipeline measurements (this project)

Once the pipeline was split into four independent threads (`CAPTURE`/`CONVERT`/`DIFF`/`WRITE`), each stage's per-call cost was re-measured with `CLOCK_MONOTONIC` timestamps around its core work, logged via `syslog` (`CAPTURE elapsed_ms`, `CONVERT elapsed_ms`, `DIFF elapsed_ms`, `WRITE elapsed_ms`) and cross-checked with `journalctl`:

| Stage    | Observed (this pipeline) | Budget available (10 Hz, 33 ms/frame) |
|----------|---------------------------|------------------------------------------|
| CAPTURE  | ~0.02 ms                  | 33 ms                                     |
| CONVERT  | ~4.0 ms                   | 33 ms                                     |
| DIFF     | ~2.1–2.9 ms                | 33 ms                                     |
| **Total**| **~6.5 ms**                | **33 ms**                                  |

The combined CAPTURE+CONVERT+DIFF cost is well under 20% of the per-frame budget at the camera's native ~29–30 fps. This measurement was the deciding evidence in ruling out "software is too slow" as the explanation for missed ticks at 10 Hz — see `docs/04_known_limitations.md` for what the real cause turned out to be.

## Why WRITE was the architectural pressure point, not the aggregate

Regarding the WRITE service: it is classified as best-effort rather than RM-scheduled. The reason is that the WRITE thread must write to the SD card, and prior testing revealed a recurring, seemingly random stall in this write path. After extended investigation and repeated testing, we were able to rule out thermal throttling or other external factors as the cause. The stall originates directly from the SD card's write-speed limitation: the SD card's sustained write bandwidth is significantly lower than the rate at which data arrives at WRITE. WRITE itself can process (prepare) frames quickly, but cannot commit them to storage at the same rate, which creates a backlog at the SD card interface. This backlog is reproducible across the majority of test runs. It becomes particularly problematic at the 10 Hz acquisition rate, where the write throughput deficit accumulates faster than at 1 Hz.

Lowering the tick-detection threshold to 95 yields a detection reliability of approximately 94.6%. However, lowering the threshold further to improve detection reliability increases exposure to random pauses caused by the SD card backlog — meaning the threshold cannot be lowered further without trading detection accuracy for timing reliability. This is a direct engineering trade-off between the two failure modes, not an independent tuning parameter.

Two mitigation approaches are available to address the underlying SD card bottleneck: (1) use a higher write-throughput SD card than the budget card currently used, or (2) write to a different storage medium entirely — such as eMMC or an external SSD/flash drive connected directly to the Pi — to bypass the SD card write-speed ceiling altogether.

This is why the pipeline was decoupled into stages joined by bounded queues and semaphores (`docs/02`): CAPTURE never waits on the SD card, because it isn't the thread doing the writing.

## RMA concepts applied elsewhere in the course, referenced here for completeness

- **Priority inversion** (three necessary conditions: low-priority holds a resource needed by high-priority; medium-priority preempts low without needing the resource; all three share a core) was avoided by design — no shared mutex is held across a priority boundary in this pipeline; the counting semaphores here bound *capacity*, not mutual exclusion of a single critical section.
- **`SCHED_FIFO`** with per-thread `pthread_setschedparam` was used for all four pipeline threads, with explicit CPU core affinity, to get deterministic dispatch — consistent with the RM assumption that scheduling is priority-driven and preemptive.
