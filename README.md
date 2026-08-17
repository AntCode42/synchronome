# Synchronome — Real-Time Multi-Threaded Frame Acquisition Pipeline

*Capstone project for the Real-Time Embedded Systems specialization (University of Colorado Boulder, ECEA-5315/5316), running on a Raspberry Pi 3B+ under a PREEMPT-RT-capable Linux kernel.*

**The system captures frames from a USB webcam via V4L2, detects clock-tick transitions through frame differencing, and writes timestamped PPM images to disk — at a sustained, glitch-minimal rate of both **1 Hz** (minimum objective) and **10 Hz** (stretch objective).**

## What this project demonstrates

- A 4-thread producer/consumer pipeline (`CAPTURE → CONVERT → DIFF → WRITE`) built from a single-threaded V4L2 base, with each stage isolated to solve a specific real-time bottleneck (see [`docs/02_architecture_and_synchronization.md`](docs/02_architecture_and_synchronization.md))
- POSIX message queues for inter-stage sequencing, and counting semaphores for bounded shared-buffer access — two mechanisms that are deliberately **not interchangeable** (queues carry *order*, semaphores carry *capacity*)
- An adaptive frame-differencing tick detector, evolved from a naive fixed-threshold comparator into a sliding-window, debounced, stabilization-aware detector (see [`docs/03_tick_detection_algorithm.md`](docs/03_tick_detection_algorithm.md))
- Measured, not assumed, real-time feasibility: WCET figures for every stage, and an honest accounting of where the system's limits are physical rather than fixable in software (see [`docs/04_known_limitations.md`](docs/04_known_limitations.md))

## Repository layout

```
src/            capture/convert/diff/write threads, sequencer, main
include/        synchronome.h, diff.h — shared declarations
docs/
  01_rma_analysis.md                     WCET measurements, RMA feasibility
  02_architecture_and_synchronization.md pipeline design, queues, semaphores, poison pill shutdown
  03_tick_detection_algorithm.md         chg_diff() evolution, calibration methodology
  04_known_limitations.md                SD card bottleneck, 10Hz sampling limit, threshold trade-offs
```

## Quick architecture summary

```
CAPTURE (V4L2, reactive to select()/DQBUF)
   │ sem_wait(raw_sem) → memcpy into raw_pool[i] → mq_send(i)
   ▼
CONVERT (reactive, blocks on mq_receive)
   │ sem_wait(frame_sem) → YUYV→RGB → mq_send(j)
   ▼
DIFF (reactive, blocks on mq_receive)
   │ chg_diff() tick test → if tick: mq_send(j) to WRITE
   │                      → if not: sem_post(frame_sem) immediately
   ▼
WRITE (reactive, blocks on mq_receive)
   │ dump_ppm() → sem_post(frame_sem) (only once WRITE is done reading)
```

Only `CAPTURE` is driven by hardware readiness (`select()`); every other stage is purely reactive to its input queue — no software sequencer is used to pace `CONVERT`/`DIFF`/`WRITE`, since none of the project's real-time requirements demand it (see the design discussion in `docs/02`).

## Build

```
make
```

Produces `./synchronome`. Requires `-lpthread -lrt` (already wired into the Makefile).

## Status

- **1 Hz objective:** met. ~99% tick-detection reliability over sustained runs, zero duplicate writes, zero blurry writes after the stabilization mechanism was added.
- **10 Hz stretch objective:** met with a measured ~4–5% miss rate, which is explained and bounded mathematically in `docs/04_known_limitations.md` rather than being an unexplained defect.

## AI Usage

AI assistance was used in a documentation-and-review capacity, not to design or write the core algorithm or architecture.

All algorithmic decisions (EMA vs. FIFO window, threshold values, `NOISE_MARGIN_HIGH`/`LOW`, `SILENCE_MS`, the RMA/WCET analysis, the queue/semaphore architecture, and the poison-pill shutdown design) were made and implemented independently; AI use was limited to reviewing, commenting, and documenting code and decisions already made.