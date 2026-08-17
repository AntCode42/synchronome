# Pipeline Architecture and Synchronization

## From one thread to four: why

The original V4L2 capture code did everything — `DQBUF → convert → diff → write` — inside a single loop, on a single thread. That was correct as a starting point, but it meant the SD card write (WCET 60 ms, occasionally spiking far higher under contention) blocked the *same* thread responsible for keeping up with the camera's ~30 fps stream. When a write stalled, V4L2 buffers filled and frames were silently dropped.

The fix was not "make the write faster" — SD card latency is a hardware property, not a software bug — but to **decouple** stages so that a slow WRITE never blocks CAPTURE. This is the standard producer/consumer pipeline pattern, and it is why the project has four threads instead of one.

## The four threads and their triggers

| Thread   | Triggered by                              | Sequencer-driven? |
|----------|--------------------------------------------|--------------------|
| CAPTURE  | `select()` / `VIDIOC_DQBUF` — camera hardware readiness | No |
| CONVERT  | `mq_receive()` on its input queue (blocking) | No |
| DIFF     | `mq_receive()` on its input queue (blocking) | No |
| WRITE    | `mq_receive()` on its input queue (blocking) | No (see below) |

**Only CAPTURE's rate is dictated externally** (by the camera, via `select()`). Every other stage is purely reactive: it blocks on its input queue and does work only when data arrives. A software sequencer (`Sequencer()`, driven by a `SIGALRM` interval timer) was built and tested as a course exercise, but is **not** used to pace any stage in the final pipeline — see "Why the sequencer was removed" below.

## Two shared pools, two semaphores, three queues

```
raw_pool[20][HRES*VRES*2]     (YUYV, filled by CAPTURE, read by CONVERT)
frame_pool[20][HRES*VRES*3]   (RGB,  filled by CONVERT, read by DIFF/WRITE)

raw_sem    — counting semaphore, init 20, guards raw_pool capacity
frame_sem  — counting semaphore, init 20, guards frame_pool capacity

q_capture_to_convert  — mqd_t, carries raw_pool indices
q_convert_to_diff     — mqd_t, carries frame_pool indices
q_diff_to_write       — mqd_t, carries frame_pool indices (tick frames only)
```

### Why both a queue *and* a semaphore — they are not redundant

This distinction took real debugging to nail down, so it is worth stating explicitly:

- **A queue carries order** — which index to process, in what sequence. `mq_send`/`mq_receive` are strictly FIFO.
- **A semaphore carries capacity** — how many pool slots are currently occupied, with zero knowledge of *which* slot or *what order*. `sem_wait()` blocks the producer once the pool is full; `sem_post()` is called once a consumer is done reading a slot, never before.

Neither can substitute for the other. A queue with unlimited buffering doesn't stop `raw_pool`/`frame_pool` from being overwritten out from under a slow consumer — the pools are physically bounded to 20 slots regardless of how deep the queue is. Conversely, a semaphore has no notion of sequence — it cannot tell CONVERT *which* raw frame to convert next, only that a slot is free.

### The rule: reserve before write, release once nobody needs it anymore

```
CAPTURE:  sem_wait(raw_sem)  → write raw_pool[i]     → mq_send(q1, i)
CONVERT:  mq_receive(q1, i)  → read raw_pool[i]       → sem_post(raw_sem)
                              → sem_wait(frame_sem)   → write frame_pool[j]
                              → mq_send(q2, j)
DIFF:     mq_receive(q2, j)  → read frame_pool[j]
              no tick   → sem_post(frame_sem)                (released here)
              tick      → mq_send(q3, j)                      (released later, by WRITE)
WRITE:    mq_receive(q3, j)  → dump_ppm(frame_pool[j]) → sem_post(frame_sem)
```

The key correctness property discovered through direct bugs during development: **a frame's `frame_sem` slot is released exactly once, at the point where nobody downstream still needs it — and that point depends on which path the frame took.** A frame that never becomes a tick is released by DIFF, immediately. A frame that does become a tick must stay reserved until WRITE has finished reading it — releasing it any earlier (e.g., unconditionally right after DIFF, or worse, twice) reopens the exact overwrite race the semaphore exists to prevent.

## Shutdown: poison pill propagation, not a shared timer

`abortProg` is a single global flag. Two independent conditions can set it:

1. `CAPTURE`'s `select()` times out (2 s with no frame — treated as a hardware fault, not a normal condition).
2. `WRITE` reaches its target frame count (`FRAMES_TO_WRITE`, 181 for the 1 Hz/30 min-equivalent test).

The propagation problem: `CONVERT`, `DIFF`, and `WRITE` are all blocked in `mq_receive()`, which does not wake up just because a global variable changed. Only `CAPTURE` can act on `abortProg` immediately, because it is the only stage that is never blocked waiting for a message — it polls the flag directly inside its own `select()` loop.

The solution is a **poison pill**: a reserved sentinel value (`-1`, chosen because it can never be a valid pool index) sent through the queues in cascade:

```
CAPTURE detects abortProg == TRUE
   → sends -1 to q_capture_to_convert, exits
CONVERT's mq_receive() unblocks, sees -1
   → relays -1 to q_convert_to_diff, exits
DIFF's mq_receive() unblocks, sees -1
   → relays -1 to q_diff_to_write, exits
WRITE's mq_receive() unblocks, sees -1
   → exits (end of chain, nothing to relay)
```

Each stage tests the received index before doing any real work; if it's the sentinel, it relays and exits instead of processing.

## Why the sequencer was removed

A `Sequencer()` function (SIGALRM-driven interval timer, `sem_post()` to release WRITE at a fixed sub-rate) was implemented and validated as a standalone exercise. Two things were learned when trying to wire it into the real pipeline:

1. **`sem_wait(&semS1)` placed once, before an unconditional call to `writeloop()`, only executes once** — `writeloop()`'s own `while(!abortProg)` loop never returns to re-check the semaphore, so the sequencer had no actual effect on WRITE's pacing in that configuration.
2. **Making it have a real effect would require `mq_receive()` in non-blocking mode**, so that each sequencer wake-up could check "is there a message waiting?" without blocking indefinitely if the queue was empty (which happens most of the time at 1 Hz, since WRITE is naturally woken far more often than one tick per second would require).

Since the project's actual real-time requirement is a fixed **sample rate ceiling** (1 Hz / 10 Hz) achieved through the physical rate of the source event (the clock itself), not an artificially imposed processing cadence, the reactive queue-driven design already satisfies it: WRITE only ever receives a message when DIFF has confirmed a tick, and DIFF's own detection logic (debounce + stabilization, see `docs/03`) already enforces the minimum spacing between writes. Adding a sequencer on top would have been redundant complexity without a corresponding requirement to justify it, so it was deliberately left out of the final pipeline rather than half-implemented.
