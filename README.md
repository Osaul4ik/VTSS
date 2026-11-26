# VTSS

<p align="center"><img src="./screenshots/full.png"></p>

VTSS is a PS Vita plugin for benchmarking games and homebrews: realtime frametime and framerate with min/max values and charts (like RTSS on PC), and an optional log file per game.

Based on [ViBeS](https://github.com/Rinnegatamante/ViBeS) by Rinnegatamante.

## Install

1. Copy `VTSS.suprx` to `ux0:tai/`.
2. In `ux0:tai/config.txt` add it under `*ALL`:
   ```
   *ALL
   ux0:tai/VTSS.suprx
   ```
3. Reboot.

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
#     t   fps  ft_avg  ft_min  ft_max  (s, ms)
# res 960x544
      1  30.0   33.33   31.80   40.10
...
# sum minute section=1 time=60s frames=1782 fps avg=29.6 min=26.9 max=30.0 | ft avg=33.76 min=31.80 max=49.10 ms
```

- One line per second: time, FPS, average / min / max frametime (with CPU stats: load of cores 0-3 in %).
- `# cpu` shows the CPU clock when it changes.
- `# sum` every minute and on START + RIGHT (with CPU stats: average load per core).
- `# pause` marks a gap of 3 s or more (suspend, loading); it is left out of the summary.
- Over 256 KiB the file moves to `<TITLEID>.old.log` and a new one starts.
- The log is written every 5 s, so the last few seconds before closing the game may be missing.
- System apps (NPXS...) are not logged.

## Credits

- Rinnegatamante for ViBeS.
- CatoTheYounger for testing ViBeS.
