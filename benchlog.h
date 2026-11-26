/*
 * benchlog.h - VTSS worker: the per-game log (Settings > Devices > VTSS > Log
 * to file) and CPU stats (... > CPU stats).
 *
 * File: ux0:data/VTSS/<TITLEID>.log (rotated to <TITLEID>.old.log past
 * BLOG_MAX_BYTES). One line per second (fps, average/min/max frametime) and a
 * summary line every minute and on START+RIGHT (min/max reset).
 *
 * HOT-PATH RULE: the display hook only calls blog_second()/blog_reset(), which
 * put a small fixed-size record into a ring buffer: no formatting, no I/O, no
 * syscalls. A low-priority writer thread wakes every BLOG_FLUSH_US, formats the
 * records into one buffer and appends it to the file with a single write. The
 * file stays open between flushes (no open/close per line). With Log to file
 * off and CPU stats off, no thread is created.
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
#define BLOG_FLUSH_US   (5 * 1000000)     /* writer wakes this often            */
#define BLOG_SUM_SECS   60                /* a summary line every minute        */
#define BLOG_CPU_CORES  4                 /* cores 0-2 (apps) and 3 (system)    */

/* 1: the TitleID can be used as a file name; 0: not. */
int blog_title_ok(const char *t);
/* 1: one of the system's own apps (never logged). */
int blog_title_system(const char *t);

/* Starts the worker thread if log or cpu. log: log to a file (only for a good,
 * non-system TitleID). cpu: CPU stats. cfg_path: the per-game config the
 * worker saves when blog_save_cfg() asks (NULL: none). Returns 0, or <0 if no
 * thread runs. */
int  blog_init(const char *titleid, const char *cfg_path, int log, int cpu);
void blog_fini(void);

/* Hot path (display hook). Cheap: a ring-buffer push, nothing else. */
void blog_second(uint32_t frames, uint32_t dt_us, uint32_t ft_min_us,
                 uint32_t ft_max_us, uint16_t width, uint16_t height);
void blog_reset(void);

/* Ask the writer to save the per-game GUI mode - keeps the file write out of
 * the display hook. Returns 0 if queued, <0 (no writer) if the caller must save. */
int  blog_save_cfg(int mode_ui);

/* CPU stats for the overlay. blog_cpu_active(): 1 if sampled. blog_cpu_seq():
 * changes once a second when new values are in (one load, for the hot path).
 * blog_cpu_read(): 1 and the values (load %, ARM clock MHz) if a consistent
 * set was read, 0 if none yet or it changed meanwhile. */
int      blog_cpu_active(void);
uint32_t blog_cpu_seq(void);
int      blog_cpu_read(uint8_t load[BLOG_CPU_CORES], uint16_t *mhz, uint32_t *seq);

#endif
