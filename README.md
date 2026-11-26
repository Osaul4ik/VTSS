# VTSS

<p align="center"><img src="./screenshots/full.png"></p>

VTSS is a PS Vita plugin for benchmarking games and homebrews: realtime frametime and framerate with min/max values and charts (like RTSS on PC), and an optional log file per game.

Based on [ViBeS](https://github.com/Rinnegatamante/ViBeS) by Rinnegatamante.

## Install

1. Copy `VTSS.suprx` and `VTSS_settings.suprx` to `ux0:tai/`.
2. In `ux0:tai/config.txt` add:
   ```
   *ALL
   ux0:tai/VTSS.suprx
   *NPXS10015
   ux0:tai/VTSS_settings.suprx
   ```
   If `*ALL` or `*NPXS10015` is already there, put the line under it instead.
3. Reboot.

`VTSS.suprx` is the overlay; `VTSS_settings.suprx` is only the page in the Settings app, so games don't carry its code.

For commercial games [ioplus](https://github.com/CelesteBlue-dev/PSVita-RE-tools/blob/master/ioPlus/ioPlus-0.1/release/ioplus.skprx) or [rePatch reLoaded](https://github.com/SonicMastr/rePatch-reLoaded) is required. Homebrews need nothing extra.

## Settings

**Settings > Devices > VTSS**

- **VTSS Enable** - off: VTSS is not loaded anywhere (it unloads itself right at start, almost no memory or CPU used).
- **Status** - Loaded / Not loaded. Press it for details.
- **Log to file** - writes each game's results to `ux0:data/VTSS/<TITLEID>.log`.
- **CPU stats** - load of each CPU core (0-2 for games, 3 for the system) and the CPU clock, once a second, on the overlay and in the log.

Changes apply to games started after them.

## Controls

- START + LEFT: GUI mode - No GUI, FPS counter, Frametime analysis + FPS counter (default), Frametime analysis + FPS analysis. Saved per game.
- START + RIGHT: reset min/max values (to benchmark a specific section; in the log a new section starts).

## Log

```
# VTSS v1.0 | PCSB00245 | start | 2026-11-26 12:05:09
# game The Simpsons Hit & Run
# clk cpu=444 bus=222 gpu=222 xbar=166 MHz
# mem free user=220.0 cdram=12.0 phycont=3.5 MiB
#     t   fps  ft_avg  ft_min  ft_max  jitter  hitch  (s, ms)
# res 960x544
      1  30.1   33.25   32.77   33.92    0.34      0
# hitch 80.12 ms at t=5.8
      6  28.7   34.87   32.75   80.12    3.62      1
...
# sum minute section=1 time=60s frames=1782
#   fps    avg=29.8 min=26.4 max=30.2 | low 1%=18.9 0.1%=12.5
#   ft     avg=33.51 min=32.73 max=80.12 | p50=33.3 p95=33.8 p99=33.9 p99.9=80.1 ms
#   jitter avg=0.79 ms | hitches=5 pauses=0
#   mem    free user=217.0 (min 214.0) cdram=12.0 phycont=3.5 MiB
#   vtss   cost avg=89 max=108 us/frame (1 in 32 frames, n=47)
```

- The header has the game's title, the CPU / BUS / GPU / XBAR clocks (an overclock shows here) and the free memory. A `# clk ... at t=` line is added each time a clock changes (noticed within a second).
- One line per second: time, FPS, average / min / max frametime, jitter and the number of hitches (with CPU stats: load of cores 0-3 in %). A second's average hides stutter, so look at ft_max, jitter and hitch too.
- Jitter: the average difference between one frame and the one before.
- Hitch: a frame longer than 175% of the running average frametime (the last ~16 frames) and at least 25 ms, e.g. one missed vsync. `# hitch` lines give the time of each (at most 4 per second; the hitch column counts them all).
- `# sum` every minute, on START + RIGHT (a new section starts) and when the game closes. It covers the section so far, from a histogram of the frametimes in 0.1 ms steps:
  - `low 1%` / `0.1%`: the average FPS of the slowest 1% / 0.1% of the frames.
  - `p50` ... `p99.9`: frametime percentiles, 0.1 ms resolution (`>200.0`: beyond the histogram).
  - jitter, hitches, pauses.
  - `cpu` (with CPU stats): average load per core.
  - `mem`: free memory of the game (now, and the lowest in the section).
  - `vtss`: VTSS's own time per frame (the overlay and log hook, not the display call), measured on 1 frame in 32.
- `# res` shows the resolution when it changes.
- `# pause` marks a frame of 3 s or more (suspend, loading); it is left out of every statistic.
- Past 256 KiB the file moves to `<TITLEID>.old.log` and a new one starts (checked after each write).
- The log is written every 5 s, so the last few seconds before closing the game may be missing.
- Memory: the log's buffers are one 28 KiB block, allocated only while Log to file is on. Off: nothing.
- System apps (NPXS...) are not logged.

## Credits

- Rinnegatamante for ViBeS.
- CatoTheYounger for testing ViBeS.