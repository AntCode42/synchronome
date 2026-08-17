# Tick Detection: `diff()` / `chg_diff()` — Design Evolution

This detector went through several iterations, each one driven by a concrete, measured failure — not by guessing. This document records both the final algorithm and the debugging path, since the *why* behind each change is as much a part of the deliverable as the code itself.

> **Note on versioning:** an earlier iteration of this detector used a fixed-size FIFO sliding window (30 samples, explicit shift-and-append) to compute the background baseline. That version is superseded. The current implementation (described below) replaces the FIFO window with an **exponential moving average (EMA)** for the baseline, and moves tick confirmation into an explicit state machine in `diff()` rather than a single threshold test in `chg_diff()`. The FIFO-window bugs (Bug 1 and Bug 2 below) are kept for the historical record, since the reasoning that led to abandoning a fixed-count average is part of why the EMA approach was chosen.

## The core idea (current implementation)

`diff()` computes the sum of absolute per-pixel differences between the current frame and the previous one (`change`). Two complementary reference levels track "no tick" background noise:

- **`chg_diff()`** maintains a `baseline` via EMA, expresses each new `change` as a percentage of that baseline (`prc`), and only updates the baseline when `prc` is *below* the detection sensitivity threshold (`DETN_SENS`) — so a real tick never pollutes the reference.
- **`diff()`** independently maintains its own EMA-tracked `noise_floor` and drives a three-state machine (`IDLE → TRANSITIONING → STABLE_FOUND`) that decides when a change is large enough, and has settled enough, to be confirmed and forwarded to `WRITE`.

Both use the same startup pattern: a **bootstrap phase** (10 samples, plain arithmetic mean) seeds the initial reference before any detection is attempted, avoiding the "crushed average" failure described in Bug 1 below.

```c
// chg_diff(): baseline tracked via EMA, updated only on non-tick samples
prc = (change * 100) / baseline
if (prc > DETN_SENS)
    baseline = EMA_ALPHA * change + (1 - EMA_ALPHA) * baseline

// diff(): independent EMA-tracked noise_floor drives a state machine
IDLE:          change >= noise_floor * NOISE_MARGIN_HIGH  &&  elapsed_ms >= SILENCE_MS
                   → TRANSITIONING
               else → noise_floor = EMA_ALPHA*change + (1-EMA_ALPHA)*noise_floor
TRANSITIONING: change < noise_floor * NOISE_MARGIN_LOW
                   → STABLE_FOUND
               transition_count >= MAX_TRANSITION_FRAMES (timeout)
                   → STABLE_FOUND  (forced, logged as a warning)
STABLE_FOUND:  send diff_index to WRITE, reset to IDLE
```

## Why EMA instead of a fixed-count sliding window

An earlier version used a hardcoded 30-sample window (see Bug 1 / Bug 2 below for the failures that motivated moving away from it). The EMA approach was adopted because:

- **No fixed sample count to get wrong.** A FIFO window's correctness depends on knowing exactly how many real samples are currently held (see Bug 1) — an EMA has no such bookkeeping: every new sample updates the average in O(1) with no count to track or zero-guard.
- **No reset discontinuity.** The FIFO window's Bug 2 came from wiping the array on every tick and rebuilding it from a handful of samples. An EMA never resets — it just keeps decaying old influence via `EMA_ALPHA`, so there is no "unreliable average right after a tick" window at all.
- **Cheaper.** One multiply-add per frame instead of an array shift over 30 elements per frame.

The trade-off: an EMA's effective "memory length" is a function of `EMA_ALPHA` rather than an explicit sample count, so its responsiveness/stability trade-off is tuned by that single constant instead of a window size — calibration methodology (below) applies equally to this constant.

## Bug 1 — dividing by a fixed window size instead of the real sample count *(historical — FIFO-window version)*

The original sliding-window average divided by a hardcoded `30`, regardless of how many real samples were actually in the window:

```c
avg = (sum / 30);   // WRONG once fewer than 30 real samples exist
```

Immediately after any reset (window full, or a tick just detected), the window would contain only 1–2 real values, but the average was still computed as if 30 were present — artificially crushing `avg` toward zero and inflating `prc` into the thousands. This produced a diagnostic, reproducible pattern in `PRC_DEBUG` logs: `prc=0` alternating almost perfectly with `prc≈3000` on every other frame.

This class of bug is structurally avoided in the current EMA implementation, since there is no divisor tied to a sample count — see "Why EMA instead of a fixed-count sliding window" above. The **bootstrap phase** (10-sample plain average before EMA tracking begins) is the current implementation's equivalent safeguard against operating on too few real samples.

## Bug 2 — reset-based window destroys the moving average's reliability *(historical — FIFO-window version)*

The original window, once full or once a tick fired, was **entirely wiped** and rebuilt from scratch. This meant that immediately after every tick, the very next several frames were evaluated against a baseline computed from only 1–3 real samples — a statistically unreliable average, sitting right next to the threshold.

This bug is what motivated abandoning the FIFO-window approach in favor of EMA, which never resets (see above). In the current implementation, `baseline` (in `chg_diff()`) and `noise_floor` (in `diff()`) persist and adapt continuously; a confirmed tick does not wipe either reference.

## Threshold calibration — empirical, not guessed

The threshold started at a generic `115` (i.e. "+15% over baseline"), copied from an earlier single-threaded prototype. Live `PRC_DEBUG`/`EMA_DEBUG` logging (`prc`, `baseline`, `change` on every frame) combined with `journalctl` extraction was used to build a real histogram of baseline noise versus true tick amplitude, rather than adjusting the number by feel.

Two concrete recalibrations came out of this process:
- At one point the baseline had drifted up to `~93–108` (very close to `115`), and true ticks were only reaching `~110–123` — a near-miss zone with almost no margin. Lowering the threshold to `108` (baseline max observed: `102`, giving a 6-point margin) recovered the majority of previously-missed ticks without introducing false positives from noise.
- For the 10 Hz stretch case, the same empirical process was repeated: threshold lowered further (down toward `~95`), trading detection sensitivity against the false-positive risk documented in `docs/04`.

The current `diff()` state machine adds two further calibrated constants on top of the threshold: `NOISE_MARGIN_HIGH` (how far above `noise_floor` a change must rise to leave `IDLE`) and `NOISE_MARGIN_LOW` (how far back down it must settle to confirm `STABLE_FOUND`). These were calibrated the same way — from logged `PRC_DEBUG` data, not guessed.

## Debounce — `SILENCE_MS`, and why it is time-based, not frame-count-based

A single physical/visual tick transition can span more than one captured frame at ~29–30 fps. Without a minimum spacing rule, two (or more) consecutive elevated frames from the *same* transition were logged as two separate ticks — confirmed directly by inspecting saved PPM pairs that showed the identical hand/digit position despite being logged as distinct `CHANGE` events.

```c
clock_gettime(CLOCK_MONOTONIC, &now);
elapsed_ms = (now.tv_sec - last_tick_time.tv_sec) * 1000.0
           + (now.tv_nsec - last_tick_time.tv_nsec) / 1e6;

if (change >= noise_floor * NOISE_MARGIN_HIGH && elapsed_ms >= SILENCE_MS) {
    // enter TRANSITIONING — candidate tick, not yet confirmed
    last_tick_time = now;
}
```

`SILENCE_MS` is deliberately measured against **wall-clock time elapsed**, not a fixed number of frames, because the pipeline's effective frame rate is not perfectly constant — a frame-count-based debounce would represent a different real duration depending on momentary FPS drift. The value itself is set per target rate: long enough to reliably absorb a multi-frame transition, short enough to stay well under the minimum spacing between two genuine ticks (≈1000 ms at 1 Hz, ≈100 ms at 10 Hz).

## Stabilization — waiting for the transition to finish before writing

Even after debouncing solved duplicate *detections*, saved frames were sometimes still visually blurred or, for a digitally rendered clock widget, showed a superimposed/mid-transition digit. The cause: the very first frame that crosses the threshold is, by construction, captured *during* the transition — not after it has settled.

In the current implementation this is handled by the `TRANSITIONING` state: the triggering frame moves the state machine from `IDLE` to `TRANSITIONING` but is **not** forwarded to `WRITE`. Only once a subsequent frame's `change` drops back below `noise_floor * NOISE_MARGIN_LOW` (or the `MAX_TRANSITION_FRAMES` timeout forces it) does the state machine reach `STABLE_FOUND` and forward *that* frame's index to `WRITE`.

```
frame in IDLE crosses NOISE_MARGIN_HIGH        → TRANSITIONING, NOT sent to WRITE yet
next frame(s) with change < NOISE_MARGIN_LOW   → STABLE_FOUND, THIS frame sent to WRITE
        (or: MAX_TRANSITION_FRAMES reached without settling → forced STABLE_FOUND, logged as a warning)
```

This requires releasing the *triggering* frame's `frame_sem` slot immediately (it is never sent anywhere), since only the later, stabilized frame is what gets forwarded to `WRITE`. Getting this ordering wrong once caused a slow, silent depletion of `frame_sem` (never released for triggering frames), leading to exactly the kind of stall this project's semaphore design was meant to prevent — a good illustration that a correct high-level mechanism can still be defeated by a missed release on one specific code path.

**Open point (flagged, not yet resolved as of this writing):** a code review of `thread_diff.c` found that the `STABLE_FOUND` branch of `diff()` does not itself call `sem_post(&frame_sem)` — only the `else` branch (still `IDLE`/`TRANSITIONING`) does. If `WRITE` is not responsible for releasing that slot after `dump_ppm()`, this reproduces the same class of semaphore-depletion bug described above, just on the confirmed-tick path instead of the triggering-frame path. See `docs/04_known_limitations.md` for tracking.

## What was tried and deliberately not pursued

- **Restricting `diff()` to a sub-region of the frame** (isolating just the digits being read, to reduce both computational cost and noise from unrelated parts of the image, such as a constantly-changing centiseconds digit on a stopwatch widget) was proposed and reasoned through, but not implemented — WCET measurements (`docs/01`) showed CAPTURE+CONVERT+DIFF together consume under 20% of the available per-frame budget even at 10 Hz, so computational cost was ruled out as the actual bottleneck before this optimization would have been worth the added complexity.
