# Reproducibility

This document describes the physical test setup, hardware, and software environment needed to reproduce the verification runs described in `docs/01_rma_analysis.md` and `docs/03_tick_detection_algorithm.md`.

## Physical test setup

The reference clock is **not** a physical analog wall clock — it is an analog clock face displayed on a monitor, filmed by the target camera. This choice was made for repeatability (identical clock state can be regenerated for every run) but introduces a source-refresh-rate variable that would not exist with a physical clock, and is documented here for that reason.

| Parameter                  | Value                          |
|-----------------------------|---------------------------------|
| Clock source                | Analog clock face, displayed on monitor |
| Display resolution          | 2K                              |
| Display refresh rate        | 120 Hz                          |
| Camera-to-screen distance   | 23.5 cm                         |
| Framing                     | Clock face fills approximately the entire camera field of view |

**Why this matters for the timing analysis:** the display refreshes at 120 Hz, well above both the 1 Hz and 10 Hz acquisition/selection rates this project targets, and above the ~29–30 fps native camera acquisition rate (see `docs/01_rma_analysis.md`). The display is not the bottleneck or a source of aliasing at either verification rate — this is stated explicitly rather than left as an unstated assumption.

**Why the framing matters:** the clock face filling nearly the full frame maximizes the pixel area available to `chg_diff()` for tick detection (see `docs/03_tick_detection_algorithm.md`), improving signal-to-noise in the frame-differencing baseline relative to a small clock face surrounded by background.

![Test setup](assets/setup_photo.jpg)
*Camera-to-screen physical setup used for all verification runs.*

## Hardware

- **Target:** Raspberry Pi 3B+ (Cortex-A53, aarch64)
- **Camera:** Logitec C270, 640x480
- **Host (development):** ThinkPad T14 Gen 1, NixOS

## Software environment

- **Kernel:** SMP PREEMPT Debian 1:6.12.75-1+rpt1
- **Target OS:** Debian (aarch64)
- **Toolchain:** native compile on Pi

## Clock use

- **1Hz clock:** https://www.visnos.com/demos/clock
- **10Hz clock:** https://www.timeanddate.com/stopwatch/

## Usage

By default the synchronome work at 1 Hz. For running at 10 Hz uncomment the following line in synchronome.h file

```
#define FREQ_10HZ
```

You need to have a empty `/frame` repository at the source of the file in ordre to work.

## Build

```
make
```

Requires `-lpthread -lrt` (wired into the Makefile).

## Run

```
sudo ./synchronome
```

Frames are written as PPM files with the timestamp and `uname -a` output embedded as `#`-prefaced comment lines in the header, per the project's minimum objectives.

## Verifying a run

- `journalctl -t pthread` — per-thread runtime logs (WCET measurements, tick detection events)
- Frame count check: 181 frames minimum for a 1 Hz / 3-minute verification window (see grading rubric MINIMUM criteria), 1801 for the full 30-minute objective
- Visual check: sequential PPM frames should show monotonic second-hand advancement with no skips, repeats, or blur
