# ViBeS

<p align="center"><img src="./screenshots/full.png"></p>

ViBeS (short for Vita Benchmarking Software) is a plugin for benchmarking games and homebrews.

The plugin allows to analyze in realtime frametime and framerate, collecting also minimum and maximum values of these two, and shows details about these in comfortable to inspect charts, similarly to how RTSS (RivaTuner Statistics Server) works for PC benchmarking.

## Changelog

### v.1.3

- Benchmark log per game: `ux0:data/benchmark/<TITLEID>.log`, on by default, START + UP toggles it.

### v.1.0

- Initial release.

## Setup Instructions

- In order to properly use this plugin with commercial games, [ioplus](https://github.com/CelesteBlue-dev/PSVita-RE-tools/blob/master/ioPlus/ioPlus-0.1/release/ioplus.skprx) or [rePatch reLoaded](https://github.com/SonicMastr/rePatch-reLoaded) is required. For homebrew applications, no extra requirements are needed.
- Install this plugin by copying `ViBeS.suprx` to your taiHEN plugins folder (usually `ux0:tai`) and adding two entries to your `config.txt` under `*ALL` or under specific Title ID sections you want to use the plugin on.

# Controls

- You can change verbosity of the GUI by pressing START + LEFT. Four modes are currently available: No GUI, FPS Counter, Frametime Analysis + FPS Counter (Default), Frametime Analysis + FPS Analysis. The currently used mode will be saved and used for any further launch of the running application.
- You can reset minimum/maximum values collected by pressing START + RIGHT in order to benchmark specific sections of games/applications.
- You can switch the log on/off by pressing START + UP (saved per game, like the GUI mode).

# Log

Each game writes its results to `ux0:data/benchmark/<TITLEID>.log` (over 256 KiB the file moves to `<TITLEID>.old.log` and a new one starts). System apps (NPXS…) are not logged.

```
# ViBeS v1.3 | PCSB00245 | start | 2026-11-26 12:05:09
#     t   fps  ft_avg  ft_min  ft_max  (s, ms)
# res 960x544
      1  30.0   33.33   31.80   40.10
...
# sum minute section=1 time=60s frames=1782 fps avg=29.6 min=26.9 max=30.0 | ft avg=33.76 min=31.80 max=49.10 ms
```

- One line per second: time, FPS, average / min / max frametime.
- `# sum` every minute, on START + RIGHT (a new section starts) and when the log is switched off.
- `# pause` marks a gap of 3 s or more (suspend, loading); it is left out of the summary.
- The log is written every 5 s, so the last few seconds before closing the game may be missing.

## Credits

- CatoTheYounger for testing the plugin.
