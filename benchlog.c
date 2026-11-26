/*
 * benchlog.c - VTSS worker: per-game log and CPU stats. See benchlog.h.
 *
 * Producer: the display hook (one thread). Consumer: the worker thread. The
 * ring is single-producer / single-consumer, so no lock: the producer owns
 * g_head, the consumer owns g_tail; release/acquire on the indexes orders the
 * record before the index. A full ring drops the record and counts it (reported in the log).
 *
 * CPU stats: the worker samples sceKernelGetSystemInfo (per-core idle time)
 * once a second; the load is 100% minus the idle share of the elapsed time.
 * The result is published under a sequence counter (seqlock): the overlay
 * re-reads it only when the counter moved, i.e. once a second.
 *
 * Formatting is a tiny integer-only formatter (no libc printf, no floats).
 */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <stddef.h>
#include <stdint.h>

#include "benchlog.h"
#include "vtss.h"

#ifndef VTSS_BUILD
#define VTSS_BUILD "dev"
#endif

#define BLOG_THREAD_PRIO   0xA0           /* below the game's threads           */
#define BLOG_THREAD_STACK  0x2000
#define BLOG_CPU_US        1000000        /* CPU sample period                  */
#define COLUMNS     "#     t   fps  ft_avg  ft_min  ft_max  (s, ms)\n"
#define COLUMNS_CPU "#     t   fps  ft_avg  ft_min  ft_max  c0  c1  c2  c3  (s, ms, % load)\n"
#define BLOG_PAUSE_US      3000000u       /* a "second" this long = suspend/load */

/* ---- ring buffer -------------------------------------------------------- */

enum { REC_SEC = 0, REC_RESET = 1 };

typedef struct {
	uint32_t dt_us;                       /* length of this window              */
	uint32_t ft_min_us, ft_max_us;
	uint16_t frames;
	uint16_t w, h;
	uint8_t  kind;
} BlogRec;

#define RING_N    128                     /* 128 s of records: a power of two   */
#define RING_MASK (RING_N - 1)

static BlogRec           g_ring[RING_N];
static uint32_t g_head;                   /* written by the producer only       */
static uint32_t g_tail;                   /* written by the consumer only       */
static uint32_t g_dropped;                /* producer only                      */

#define LD_ACQ(v)    __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define ST_REL(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELEASE)
#define LD_RLX(v)    __atomic_load_n(&(v), __ATOMIC_RELAXED)
#define ST_RLX(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELAXED)

static int g_stop;                        /* atomics: see LD_ / ST_ below       */
static int g_cfg_dirty;
static int g_cfg_mode;

static SceUID g_thid = -1;
static SceUID g_sema = -1;
static int    g_log;                      /* this process logs to a file        */
static int    g_cpu;                      /* this process samples the CPU       */
static char   g_title[16];
static char   g_path[64], g_path_old[64];
static const char *g_cfg_path;

/* CPU stats, published by the worker (seqlock: odd = being written). */
static uint32_t g_cpu_seq;
static uint8_t  g_cpu_load[BLOG_CPU_CORES];
static uint16_t g_cpu_mhz;

static void push(const BlogRec *r)
{
	uint32_t h = LD_RLX(g_head), next = (h + 1) & RING_MASK;

	if (next == LD_ACQ(g_tail)) {         /* full: the record is dropped        */
		ST_RLX(g_dropped, LD_RLX(g_dropped) + 1);
		return;
	}
	g_ring[h] = *r;
	ST_REL(g_head, next);                 /* record visible before the index    */
}

void blog_second(uint32_t frames, uint32_t dt_us, uint32_t ft_min_us,
                 uint32_t ft_max_us, uint16_t width, uint16_t height)
{
	if (!g_log)
		return;
	BlogRec r;
	r.kind = REC_SEC;
	r.frames = frames > 0xFFFF ? 0xFFFF : (uint16_t)frames;
	r.dt_us = dt_us;
	r.ft_min_us = ft_min_us;
	r.ft_max_us = ft_max_us;
	r.w = width;
	r.h = height;
	push(&r);
}

void blog_reset(void)
{
	if (!g_log)
		return;
	BlogRec r = {0};
	r.kind = REC_RESET;
	push(&r);
}

int blog_save_cfg(int mode_ui)
{
	if (g_thid < 0)
		return -1;
	ST_RLX(g_cfg_mode, mode_ui);
	ST_REL(g_cfg_dirty, 1);
	sceKernelSignalSema(g_sema, 1);        /* a button press, not every frame: wake the worker */
	return 0;
}

uint32_t blog_cpu_seq(void)
{
	return LD_RLX(g_cpu_seq);
}

int blog_cpu_read(uint8_t load[BLOG_CPU_CORES], uint16_t *mhz, uint32_t *seq)
{
	uint32_t s = LD_ACQ(g_cpu_seq);

	if (!s || (s & 1))
		return 0;                         /* nothing yet / being written        */
	for (int c = 0; c < BLOG_CPU_CORES; c++)
		load[c] = LD_RLX(g_cpu_load[c]);
	*mhz = LD_RLX(g_cpu_mhz);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (LD_RLX(g_cpu_seq) != s)
		return 0;                         /* changed meanwhile: next frame      */
	*seq = s;
	return 1;
}

/* ---- TitleID ------------------------------------------------------------ */

int blog_title_ok(const char *t)
{
	int n = 0;

	for (; t[n]; n++)
		if (n >= 12 || !((t[n] >= 'A' && t[n] <= 'Z') || (t[n] >= 'a' && t[n] <= 'z') ||
		                 (t[n] >= '0' && t[n] <= '9') || t[n] == '_'))
			return 0;
	return n >= 4;
}

int blog_title_system(const char *t)
{
	return (t[0] == 'N' && t[1] == 'P' && t[2] == 'X' && t[3] == 'S') ||
	       (t[0] == 'm' && t[1] == 'a' && t[2] == 'i' && t[3] == 'n' && !t[4]);
}

/* ---- output buffer + formatter (worker thread only) ---------------------- */

static char     g_buf[2048];
static int      g_len;
static SceUID   g_fd = -1;
static uint32_t g_bytes;
static int      g_file_bad;               /* a write failed: stop trying        */

static void out_str(const char *s)
{
	while (*s && g_len < (int)sizeof(g_buf))
		g_buf[g_len++] = *s++;
}

static void out_ch(char c)
{
	if (g_len < (int)sizeof(g_buf))
		g_buf[g_len++] = c;
}

static void out_uint(uint32_t v, int width, char pad)
{
	char tmp[10];
	int n = 0;

	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (width-- > n)
		out_ch(pad);
	while (n)
		out_ch(tmp[--n]);
}

/* microseconds as "ms.cc", right-aligned to width (integer part) */
static void out_ms(uint32_t us, int width)
{
	out_uint(us / 1000, width, ' ');
	out_ch('.');
	out_uint((us % 1000) / 10, 2, '0');
}

/* tenths as "n.d" */
static void out_tenths(uint32_t v10, int width)
{
	out_uint(v10 / 10, width, ' ');
	out_ch('.');
	out_ch((char)('0' + v10 % 10));
}

static uint32_t fps10(uint32_t frames, uint64_t dt_us)
{
	return dt_us ? (uint32_t)(((uint64_t)frames * 10000000u + dt_us / 2) / dt_us) : 0;
}

/* ---- file (worker thread only) ------------------------------------------ */

static void file_open(int trunc)
{
	if (g_file_bad || g_fd >= 0)
		return;
	sceIoMkdir(VTSS_DIR, 0777);
	g_fd = sceIoOpen(g_path, SCE_O_WRONLY | SCE_O_CREAT | (trunc ? SCE_O_TRUNC : SCE_O_APPEND), 0666);
	if (g_fd < 0) {
		g_file_bad = 1;
		return;
	}
	SceOff end = trunc ? 0 : sceIoLseek(g_fd, 0, SCE_SEEK_END);
	g_bytes = end > 0 ? (uint32_t)end : 0;
}

static void file_close(void)
{
	if (g_fd >= 0) {
		sceIoClose(g_fd);
		g_fd = -1;
	}
}

/* One write per flush. Rotation happens only between flushes, never mid-line. */
static void flush(void)
{
	if (!g_len)
		return;
	if (g_fd < 0 || g_file_bad) {
		g_len = 0;
		return;
	}
	if (g_bytes + (uint32_t)g_len > BLOG_MAX_BYTES) {
		file_close();
		sceIoRemove(g_path_old);
		sceIoRename(g_path, g_path_old);
		file_open(1);
		if (g_fd < 0) {
			g_len = 0;
			return;
		}
		/* the new file starts with its own header (the buffer holds data lines) */
		char hdr[160];
		int n = 0;
		const char *parts[] = { "# VTSS ", VTSS_BUILD, " | ", g_title,
		                        " | continued (older lines in .old.log)\n",
		                        g_cpu ? COLUMNS_CPU : COLUMNS };
		for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
			for (const char *c = parts[i]; *c && n < (int)sizeof(hdr); c++)
				hdr[n++] = *c;
		if (sceIoWrite(g_fd, hdr, n) == n)
			g_bytes += (uint32_t)n;
	}
	int w = sceIoWrite(g_fd, g_buf, g_len);
	if (w != g_len) {
		file_close();
		g_file_bad = 1;
	} else {
		g_bytes += (uint32_t)w;
	}
	g_len = 0;
}

/* room for one more line, else flush first */
static void need(int n)
{
	if (g_len + n > (int)sizeof(g_buf))
		flush();
}

/* ---- CPU sampling (worker thread only) ----------------------------------- */

static int      g_cpu_bad;                /* the call failed: no CPU stats      */
static int      g_cpu_primed;
static uint64_t g_cpu_t;
static uint64_t g_cpu_idle[BLOG_CPU_CORES];
static uint8_t  g_cpu_last[BLOG_CPU_CORES];  /* latest loads, for the log lines */
static uint16_t g_cpu_mhz_logged;

static void cpu_sample(void)
{
	SceKernelSystemInfo si;

	if (g_cpu_bad)
		return;
	si.size = sizeof(si);
	if (sceKernelGetSystemInfo(&si) < 0) {
		g_cpu_bad = 1;
		return;
	}
	uint64_t now = sceKernelGetProcessTimeWide();

	if (g_cpu_primed) {
		uint64_t dt = now - g_cpu_t;
		if (!dt)
			return;
		for (int c = 0; c < BLOG_CPU_CORES; c++) {
			uint64_t idle = si.cpuInfo[c].idleClock - g_cpu_idle[c];
			g_cpu_last[c] = idle >= dt ? 0 : (uint8_t)(((dt - idle) * 100 + dt / 2) / dt);
		}
		int mhz = scePowerGetArmClockFrequency();
		uint32_t s = LD_RLX(g_cpu_seq);
		ST_RLX(g_cpu_seq, s + 1);         /* odd: being written                  */
		__atomic_thread_fence(__ATOMIC_RELEASE);
		for (int c = 0; c < BLOG_CPU_CORES; c++)
			ST_RLX(g_cpu_load[c], g_cpu_last[c]);
		ST_RLX(g_cpu_mhz, mhz > 0 ? (uint16_t)mhz : 0);
		ST_REL(g_cpu_seq, s + 2);
	}
	g_cpu_primed = 1;
	g_cpu_t = now;
	for (int c = 0; c < BLOG_CPU_CORES; c++)
		g_cpu_idle[c] = si.cpuInfo[c].idleClock;
}

/* ---- session state (worker thread only) ---------------------------------- */

static uint64_t g_t_us;                   /* session time, sum of windows       */
static uint16_t g_w, g_h;                 /* resolution last logged             */
static uint32_t g_dropped_seen;

typedef struct {
	uint32_t secs, frames;
	uint64_t us;
	uint32_t fps_min, fps_max;            /* tenths                              */
	uint32_t ft_min, ft_max;              /* us                                  */
	uint32_t pauses;
	uint32_t cpu_n, cpu_sum[BLOG_CPU_CORES];
} Sum;

static Sum      g_sum;
static uint32_t g_section = 1;

static void sum_clear(void)
{
	Sum z = {0};
	g_sum = z;
	g_sum.fps_min = 0xFFFFFFFFu;
	g_sum.ft_min = 0xFFFFFFFFu;
}

static void put_sum(const char *why)
{
	if (!g_sum.secs)
		return;
	need(240);
	out_str("# sum ");
	out_str(why);
	out_str(" section=");
	out_uint(g_section, 0, ' ');
	out_str(" time=");
	out_uint(g_sum.secs, 0, ' ');
	out_str("s frames=");
	out_uint(g_sum.frames, 0, ' ');
	out_str(" fps avg=");
	out_tenths(fps10(g_sum.frames, g_sum.us), 0);
	out_str(" min=");
	out_tenths(g_sum.fps_min, 0);
	out_str(" max=");
	out_tenths(g_sum.fps_max, 0);
	out_str(" | ft avg=");
	out_ms(g_sum.frames ? (uint32_t)(g_sum.us / g_sum.frames) : 0, 0);
	out_str(" min=");
	out_ms(g_sum.ft_min, 0);
	out_str(" max=");
	out_ms(g_sum.ft_max, 0);
	out_str(" ms");
	if (g_sum.cpu_n) {
		out_str(" | cpu avg");
		for (int c = 0; c < BLOG_CPU_CORES; c++) {
			out_str(" c");
			out_ch((char)('0' + c));
			out_ch('=');
			out_uint((g_sum.cpu_sum[c] + g_sum.cpu_n / 2) / g_sum.cpu_n, 0, ' ');
			out_ch('%');
		}
	}
	if (g_sum.pauses) {
		out_str(" | pauses=");
		out_uint(g_sum.pauses, 0, ' ');
	}
	out_ch('\n');
}

static void put_header(const char *what)
{
	SceDateTime dt;

	need(200);
	out_str("# VTSS ");
	out_str(VTSS_BUILD);
	out_str(" | ");
	out_str(g_title);
	out_str(" | ");
	out_str(what);
	if (sceRtcGetCurrentClockLocalTime(&dt) >= 0) {
		out_str(" | ");
		out_uint(dt.year, 4, '0');  out_ch('-');
		out_uint(dt.month, 2, '0'); out_ch('-');
		out_uint(dt.day, 2, '0');   out_ch(' ');
		out_uint(dt.hour, 2, '0');  out_ch(':');
		out_uint(dt.minute, 2, '0'); out_ch(':');
		out_uint(dt.second, 2, '0');
	}
	out_ch('\n');
	out_str(g_cpu ? COLUMNS_CPU : COLUMNS);
	g_w = g_h = 0;                        /* print the resolution again          */
}

static void handle(const BlogRec *r)
{
	if (r->kind == REC_RESET) {
		put_sum("reset");
		need(40);
		out_str("# reset (START+RIGHT)\n");
		g_section++;
		sum_clear();
		return;
	}

	g_t_us += r->dt_us;
	uint32_t t = (uint32_t)(g_t_us / 1000000u);

	if (r->dt_us >= BLOG_PAUSE_US) {      /* suspend, loading screen, ...: kept out of the summary */
		need(60);
		out_str("# pause ");
		out_ms(r->dt_us, 0);
		out_str(" ms at t=");
		out_uint(t, 0, ' ');
		out_ch('\n');
		g_sum.pauses++;
		return;
	}

	if (r->w != g_w || r->h != g_h) {
		need(40);
		out_str("# res ");
		out_uint(r->w, 0, ' ');
		out_ch('x');
		out_uint(r->h, 0, ' ');
		out_ch('\n');
		g_w = r->w;
		g_h = r->h;
	}

	int cpu = g_cpu && !g_cpu_bad && LD_RLX(g_cpu_seq);
	uint16_t mhz = LD_RLX(g_cpu_mhz);
	if (cpu && mhz != g_cpu_mhz_logged) {
		need(30);
		out_str("# cpu ");
		out_uint(mhz, 0, ' ');
		out_str(" MHz\n");
		g_cpu_mhz_logged = mhz;
	}

	uint32_t f10 = fps10(r->frames, r->dt_us);
	uint32_t avg = r->frames ? r->dt_us / r->frames : 0;

	need(64);
	out_uint(t, 7, ' ');
	out_ch(' ');
	out_tenths(f10, 3);
	out_ch(' ');
	out_ms(avg, 4);
	out_ch(' ');
	out_ms(r->ft_min_us, 4);
	out_ch(' ');
	out_ms(r->ft_max_us, 4);
	if (cpu) {
		for (int c = 0; c < BLOG_CPU_CORES; c++) {
			out_ch(' ');
			out_uint(g_cpu_last[c], 3, ' ');
			g_sum.cpu_sum[c] += g_cpu_last[c];
		}
		g_sum.cpu_n++;
	}
	out_ch('\n');

	g_sum.secs++;
	g_sum.frames += r->frames;
	g_sum.us += r->dt_us;
	if (f10 < g_sum.fps_min) g_sum.fps_min = f10;
	if (f10 > g_sum.fps_max) g_sum.fps_max = f10;
	if (r->frames) {
		if (r->ft_min_us < g_sum.ft_min) g_sum.ft_min = r->ft_min_us;
		if (r->ft_max_us > g_sum.ft_max) g_sum.ft_max = r->ft_max_us;
	}
	if (g_sum.secs % BLOG_SUM_SECS == 0)
		put_sum("minute");
}

static void save_cfg(void)
{
	ST_RLX(g_cfg_dirty, 0);
	int cfg = LD_RLX(g_cfg_mode);
	if (!g_cfg_path)
		return;
	sceIoMkdir(VTSS_DIR, 0777);
	SceUID fd = sceIoOpen(g_cfg_path, SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, &cfg, sizeof(cfg));
		sceIoClose(fd);
	}
}

/* Drain the ring into the buffer (formatting only, no I/O unless it fills). */
static void drain(void)
{
	if (g_log && g_fd < 0 && !g_file_bad) {   /* first time: open, session header */
		file_open(0);
		if (g_fd >= 0)
			put_header("start");
	}

	uint32_t tail = LD_RLX(g_tail);
	while (tail != LD_ACQ(g_head)) {      /* index read before the record        */
		BlogRec r = g_ring[tail];
		tail = (tail + 1) & RING_MASK;
		ST_REL(g_tail, tail);             /* slot free after the copy            */
		if (g_fd >= 0)
			handle(&r);
	}

	uint32_t d = LD_RLX(g_dropped);
	if (d != g_dropped_seen && g_fd >= 0) {
		need(40);
		out_str("# dropped ");
		out_uint(d - g_dropped_seen, 0, ' ');
		out_str(" s (worker late)\n");
		g_dropped_seen = d;
	}
}

static int worker(SceSize args, void *argp)
{
	(void)args; (void)argp;
	/* CPU stats: wake every second (sample, format), write every 5 s.
	 * Log only: wake every 5 s. */
	const SceUInt period = g_cpu ? BLOG_CPU_US : BLOG_FLUSH_US;
	uint64_t last_flush = sceKernelGetProcessTimeWide();

	if (g_cpu)
		cpu_sample();                     /* prime                               */
	while (!LD_ACQ(g_stop)) {
		SceUInt timeout = period;
		sceKernelWaitSema(g_sema, 1, &timeout);   /* timeout = the normal wake */
		if (LD_ACQ(g_cfg_dirty))
			save_cfg();
		if (g_cpu)
			cpu_sample();
		if (g_log) {
			drain();
			uint64_t now = sceKernelGetProcessTimeWide();
			if (now - last_flush >= BLOG_FLUSH_US - 50000) {
				flush();
				last_flush = now;
			}
		}
	}
	if (LD_ACQ(g_cfg_dirty))
		save_cfg();
	if (g_log) {
		drain();
		if (g_fd >= 0)
			put_sum("stop");
		flush();
		file_close();
	}
	return 0;
}

/* ---- init / fini --------------------------------------------------------- */

static void cpy(char *d, int cap, const char *a, const char *b, const char *c)
{
	int n = 0;
	for (const char *s = a; s && *s && n < cap - 1; s++) d[n++] = *s;
	for (const char *s = b; s && *s && n < cap - 1; s++) d[n++] = *s;
	for (const char *s = c; s && *s && n < cap - 1; s++) d[n++] = *s;
	d[n] = 0;
}

int blog_init(const char *titleid, const char *cfg_path, int log, int cpu)
{
	g_cfg_path = cfg_path;
	g_cpu = cpu ? 1 : 0;
	g_log = 0;
	if (log && titleid && blog_title_ok(titleid) && !blog_title_system(titleid)) {
		cpy(g_title, sizeof(g_title), titleid, 0, 0);
		cpy(g_path, sizeof(g_path), VTSS_DIR "/", titleid, ".log");
		cpy(g_path_old, sizeof(g_path_old), VTSS_DIR "/", titleid, ".old.log");
		g_log = 1;
	}
	sum_clear();
	if (!g_log && !g_cpu)
		return -1;                        /* nothing to do: no thread            */

	g_sema = sceKernelCreateSema("VTSSWorkerSema", 0, 0, 1, NULL);
	if (g_sema < 0)
		goto fail;
	g_thid = sceKernelCreateThread("VTSSWorker", worker, BLOG_THREAD_PRIO,
	                               BLOG_THREAD_STACK, 0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
	if (g_thid < 0)
		goto fail;
	if (sceKernelStartThread(g_thid, 0, NULL) < 0) {
		sceKernelDeleteThread(g_thid);
		g_thid = -1;
		goto fail;
	}
	return 0;
fail:
	g_log = g_cpu = 0;                    /* the hook pushes nothing             */
	if (g_sema >= 0) {
		sceKernelDeleteSema(g_sema);
		g_sema = -1;
	}
	return -2;
}

int blog_cpu_active(void)
{
	return g_cpu;
}

void blog_fini(void)
{
	if (g_thid < 0)
		return;
	SceUID th = g_thid;
	ST_REL(g_stop, 1);
	sceKernelSignalSema(g_sema, 1);
	SceUInt timeout = 2000000;
	sceKernelWaitThreadEnd(th, NULL, &timeout);
	sceKernelDeleteThread(th);
	g_thid = -1;
	g_log = g_cpu = 0;
	sceKernelDeleteSema(g_sema);
	g_sema = -1;
}
