# Known Limitations

This document separates limitations that were **overcome** by the multi-threaded, queue/semaphore architecture from limitations that are **still present** and — critically — explains *why* each remaining one is a physical/hardware constraint rather than an unfixed software defect, with the measurements that support that conclusion.

## Limitation 1 (overcome): single-threaded SD card write stalling the capture loop

**Before:** in the original single-threaded prototype, `dump_ppm()` (WCET 60.342 ms, occasionally spiking far higher — one observed stall reached ~740 ms) ran on the same thread responsible for keeping up with the V4L2 camera stream at ~30 fps (~33 ms budget per frame). A single slow write could stall CAPTURE long enough for the V4L2 driver's buffer ring to fill and start silently dropping frames — measured effective throughput fell from a nominal 30 fps to as low as ~1 fps under this condition.

**After:** decoupling into four independently-scheduled threads, joined by bounded queues and counting semaphores (`docs/02`), means CAPTURE never touches the SD card and is never blocked by it. CAPTURE's own measured per-call cost is now ~0.02 ms — no correlation with WRITE's latency remains.

**Residual risk, still present:** if the *tick detection rate* is pushed too high (threshold set too low, causing false positives — see Limitation 3), WRITE can still be asked to write faster than the SD card can sustain. The architecture removed the *structural* coupling between capture rate and write latency; it did not remove the SD card's physical write latency itself. Under a high false-positive rate, the same symptom (write stalls, backpressure) can still appear — now isolated to the WRITE stage instead of stalling the whole pipeline, but not eliminated as a possibility.

## Limitation 2 (present, physically bounded): ~4–5% miss rate at 10 Hz

At the 1 Hz objective, the detector achieves ~99% reliability (near-zero missed ticks, zero duplicates, zero blurred writes after the stabilization mechanism). At the 10 Hz stretch objective, a consistent ~4–5% of ticks are missed, confirmed across repeated runs (e.g., 1800-frame target run completing in ~1880 real seconds instead of ~1800, directly reflecting the accumulated time cost of missed ticks rather than a separate timing bug).

**Ruled out by direct measurement (not assumption):**
- Camera hardware cap confirmed via `v4l2-ctl --list-formats-ext`: no format/resolution combination on this device exceeds 30 fps.
- Per-stage software cost is negligible relative to budget: CAPTURE (~0.02 ms) + CONVERT (~4 ms) + DIFF (~2.1–2.9 ms) ≈ 6.5 ms against a 33 ms/frame budget at native camera rate — under 20% utilization even before accounting for the 10 Hz target's 100 ms tick period.
- The miss pattern shows no fixed periodicity (checked directly against gap-distribution data extracted from `journalctl` logs), which is consistent with a phase-drift explanation rather than a deterministic software stall.

**The actual cause — a Nyquist-style sampling limit:**

```
camera rate:        ~29–30 fps  →  ~33.3 ms between frames
target tick period:  10 Hz      →  100 ms between ticks
frames per tick:      100 / 33.3 ≈ 3.003
```

3.003 is not an integer. Even under a perfectly regular capture rate, the alignment between camera frame boundaries and the true tick transition **drifts progressively** — sometimes a transition is captured comfortably mid-frame, sometimes it falls near a frame boundary where neither adjacent frame cleanly captures the peak. This is a direct consequence of sampling a ~10 Hz signal with a ~30 fps sensor: the available margin (3 samples per event) is thin enough that irregular but bounded misses are expected, not anomalous.

**This is documented as a hardware sampling-rate limitation of the Pi 3B+ / Logitech C270 pairing at 640×480, not a defect in the detection algorithm** — the same detector achieves near-perfect reliability at 1 Hz, where the sampling margin (~29–30 frames per tick) is ample.

## Limitation 3 (present, understood trade-off): threshold sensitivity vs. false-positive load

Lowering the detection threshold increases sensitivity (fewer missed ticks) but also increases the false-positive rate — and every false positive is a message sent to WRITE, competing for SD card bandwidth exactly as described in Limitation 1's residual risk. Empirically:

- A threshold too high relative to the current baseline (baseline drift observed up to `~93–108` against an original threshold of `115`) caused the missed-tick problem directly.
- A threshold pushed too low (observed below roughly the midpoint of the calibrated range) reintroduced the SD card write-queue backpressure symptom that the pipeline separation was originally built to solve — writes stalling, visible as the same kind of throughput collapse seen in Limitation 1, just triggered by detection volume instead of raw capture rate.

There is a working band between these two failure modes (documented per-mode in `docs/03`: `108` at 1 Hz with a FIFO-stabilized baseline, `~95` at 10 Hz), but it is a genuine trade-off region, not a value that can be set once and forgotten if capture conditions (lighting, camera position, display brightness) change — as was directly observed when baseline noise drifted between test sessions without any code change.

## Limitation 5 (flagged, unresolved): possible `frame_sem` leak on the confirmed-tick path

A code review of `thread_diff.c` (current EMA/state-machine implementation, see `docs/03`) found that the `STABLE_FOUND` branch of `diff()` — the branch that fires once a tick is confirmed and its index is sent to `WRITE` — does **not** call `sem_post(&frame_sem)` itself. Only the `else` branch (frame stayed in `IDLE` or `TRANSITIONING`, i.e. no tick) posts the semaphore.

This is correct **only if** `WRITE` is the one responsible for releasing that slot, after `dump_ppm()` has finished reading `frame_pool[j]` — which matches the documented ownership rule in `docs/02` ("a frame's `frame_sem` slot is released exactly once, at the point where nobody downstream still needs it"). This needs to be **confirmed against `thread_write.c`** rather than assumed: if `WRITE` does not post `frame_sem` after writing, this reproduces the exact slow, silent semaphore-depletion bug already documented in `docs/03`'s stabilization section, just on the confirmed-tick path instead of the triggering-frame path — and would eventually stall `CONVERT` once the pool is fully drained.

**Status:** not yet verified one way or the other. Tracked here until `thread_write.c` is checked.

## Limitation 6 (minor, cosmetic): unused file-scope `state` variable

`thread_diff.c` declares `static tick_state_t state = IDLE;` at file scope (just below `prev_buffer`), but `diff()` also declares its own `static tick_state_t state = IDLE;` locally — the local declaration shadows the file-scope one everywhere it's used, making the file-scope variable dead code. Harmless (no behavioral impact — the local `static` inside `diff()` is the one actually driving the state machine, and it persists correctly across calls like any function-local `static`), but should trigger a `-Wunused-variable` warning and can be removed for clarity.

## Limitation 4 (present, scoped out): screen-capture-specific artifacts

When the observed clock is a digitally rendered widget (rather than a physical analog clock), the tick transition is itself an animation with nonzero duration, not an instantaneous event — confirmed by observing that a "blurry"/superimposed-digit frame corresponded to a frame captured mid-animation, not to camera motion blur or exposure settings (screen refresh rate was tested at 120 Hz with no change to the symptom, ruling out display refresh as the cause). The stabilization mechanism (`docs/03`) mitigates this by deferring the write to the frame immediately after the transition completes, but a sub-region-of-interest restriction on the diff computation (isolating the digit(s) being measured from unrelated, constantly-changing parts of the display, such as a live centiseconds counter) was identified as a further potential improvement and was not implemented, since WCET measurements ruled out computational cost as the dominant limiting factor at the time this trade-off was evaluated.
