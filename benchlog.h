/*
 * benchlog.h - VTSS worker: the per-game log (Settings > Devices > VTSS > Log
 * to file) and CPU stats (... > CPU stats).
 *
 * File: ux0:data/VTSS/<TITLEID>.log (rotated to <TITLEID>.old.log past
 * BLOG_MAX_BYTES). Per-second lines (fps, frametime avg/min/max, jitter,
 * hitches), `# hitch` lines for big freezes, and a section summary every
 * minute, on START+RIGHT (a new section) and at stop: FPS 1% / 0.1% low,
 * frametime percentiles from a 0.1 ms histogram, jitter, hitches, pauses, CPU
 * load per core, free memory, VTSS's own cost per frame.
 *
 * HOT-PATH RULE: the display hook only calls blog_frame()/blog_reset()/
 * blog_res()/blog_cost(). Each one stores ONE 32-bit word into a ring buffer:
 * no formatting, no I/O, no syscalls, no window statistics (min/max, sums,
 * structs are the worker's job). A low-priority worker thread wakes every
 * BLOG_WAKE_US, turns the words into statistics and lines, and appends them to
 * the file with a single write every BLOG_FLUSH_US. The file stays open between
 * flushes. With Log to file off and CPU stats off, no thread is created.
 *
 * Ring word (32 bit):
 *   bit 31 = 0   a frametime, microseconds (31 bits, saturated)
 *   bit 31 = 1   an event: bits 30..28 = type, bits 27..0 = payload
 *                  RESET  (START+RIGHT)             no payload
 *                  RES    resolution changed        width << 12 | height
 *                  COST   VTSS's own time in a frame  microseconds
 *
 * MEMORY: the ring, the histogram and the line buffer live in ONE memblock,
 * allocated by blog_init() only when Log to file is on (not freed in
 * blog_fini(): the hook may still be running; the process exit reclaims it).
 * Log off: 0 bytes (CPU stats alone need no buffers).
 *
 * CPU stats: the worker samples the per-core idle time once a second
 * (sceKernelGetSystemInfo) and publishes the load; the overlay reads it only
 * when blog_cpu_seq() moved. In the log the loads are extra columns.
 *
 * The TitleID is checked first: only letters, digits and '_', 4..12 chars (it
 * becomes a file name), and the system's own apps (NPXS*, "main") are never
 * logged.
 */
#ifndef _BENCHLOG_H_
#define _BENCHLOG_H_

#include <stdint.h>

#define BLOG_MAX_BYTES  (256 * 1024)      /* then <TITLEID>.old.log, one step   */
#define BLOG_WAKE_US    (1 * 1000000)     /* worker wakes this often            */
#define BLOG_FLUSH_US   (5 * 1000000)     /* ... and writes the file this often */
#define BLOG_SUM_SECS   60                /* a summary every minute             */
#define BLOG_CPU_CORES  4                 /* cores 0-2 (apps) and 3 (system)    */

/* One VTSS-cost sample every BLOG_COST_EVERY frames (a power of two): the
 * hook reads the clock a second time only on that frame. */
#define BLOG_COST_EVERY 32

/* Hitch: a frame longer than BLOG_HITCH_PCT % of the running average frametime
 * (a smoothed average of the last ~16 frames) AND at least BLOG_HITCH_MIN_US.
 * One missed vsync (30 fps: 66.7 ms instead of 33.3; 60 fps: 33.3 instead of
 * 16.7) is a hitch; a 1.5x wobble is not. */
#define BLOG_HITCH_PCT    175
#define BLOG_HITCH_MIN_US 25000

/* 1: the TitleID can be used as a file name; 0: not. */
int blog_title_ok(const char *t);
/* 1: one of the system's own apps (never logged). */
int blog_title_system(const char *t);

/* Starts the worker thread if log or cpu. log: log to a file (only for a good,
 * non-system TitleID). name: the game's title for the log header (NULL: none).
 * cpu: CPU stats. cfg_path: the per-game config the worker saves when
 * blog_save_cfg() asks (NULL: none). Returns 0, or <0 if no thread runs. */
int  blog_init(const char *titleid, const char *name, const char *cfg_path, int log, int cpu);
void blog_fini(void);

/* Hot path (display hook). Each is a ring-buffer push, nothing else. */
void blog_frame(uint32_t ft_us);                  /* one frametime            */
void blog_res(uint16_t width, uint16_t height);   /* the resolution changed   */
void blog_cost(uint32_t us);                      /* VTSS's own time, 1 frame */
void blog_reset(void);                            /* START+RIGHT: new section */

/* Ask the writer to save the per-game GUI mode - keeps the file write out of
 * the display hook. Returns 0 if queued, <0 (no writer) if the caller must save. */
int  blog_save_cfg(int mode_ui);

/* CPU stats for the overlay. blog_cpu_active(): 1 if sampled. blog_cpu_seq():
 * changes once a second when new values are in (one load, for the hot path).
 * blog_cpu_read(): 1 and the values (load %, ARM clock MHz) if a consistent
 * set was read, 0 if none yet or it changed meanwhile. */
int      blog_cpu_active(void);
/* 1: the log runs (the hook may push); 0: it does not - skip the calls. */
int      blog_log_active(void);
uint32_t blog_cpu_seq(void);
int      blog_cpu_read(uint8_t load[BLOG_CPU_CORES], uint16_t *mhz, uint32_t *seq);

#endif