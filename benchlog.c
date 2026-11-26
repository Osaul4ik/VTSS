/*
 * benchlog.c - VTSS worker: per-game log and CPU stats. See benchlog.h.
 *
 * Producer: the display hook (one thread). Consumer: the worker thread. The
 * ring is single-producer / single-consumer, so no lock: the producer owns
 * g_head, the consumer owns g_tail; release/acquire on the indexes orders the
 * word before the index. A full ring drops the word and counts it (reported in
 * the log).
 *
 * The hook pushes one 32-bit word per frame (the frametime) and, rarely, an
 * event word (reset, resolution, VTSS cost). Everything else - windows, min/
 * max, jitter, hitches, the histogram, percentiles - is computed here, in the
 * worker, from those words.
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
#include <psp2/kernel/sysmem.h>
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
#define COLUMNS     "#     t   fps  ft_avg  ft_min  ft_max  jitter  hitch  (s, ms)\n"
#define COLUMNS_CPU "#     t   fps  ft_avg  ft_min  ft_max  jitter  hitch  c0  c1  c2  c3  (s, ms, % load)\n"
#define BLOG_PAUSE_US      3000000u       /* a frame this long = suspend/load   */
#define BLOG_SEC_US        1000000u       /* one line per this much frame time  */
#define HITCH_LINES_SEC    4              /* `# hitch` lines per second, at most (all are counted) */

/* ---- ring word ------------------------------------------------------------ */

#define W_EVENT     0x80000000u           /* bit 31: event, else a frametime    */
#define W_FT_MAX    0x7FFFFFFFu
enum { EV_RESET = 1, EV_RES = 2, EV_COST = 3 };
#define W_EV(t, p)  (W_EVENT | ((uint32_t)(t) << 28) | ((uint32_t)(p) & 0x0FFFFFFFu))
#define W_TYPE(w)   (((w) >> 28) & 7u)
#define W_PAYLOAD(w) ((w) & 0x0FFFFFFFu)

/* ---- the one memblock (allocated only when Log to file is on) ------------- */

#define RING_N      4096                  /* ~68 s of frames at 60 fps (the worker wakes every second) */
#define RING_MASK   (RING_N - 1)
#define HIST_STEP_US 100                  /* histogram step: 0.1 ms             */
#define HIST_N      2000                  /* 0 .. 200 ms; longer frames go to the overflow counters */
#define OBUF_SZ     2048                  /* lines of ~1 s; flushed early if full */

typedef struct {
	uint32_t ring[RING_N];
	uint32_t hist[HIST_N];
	char     buf[OBUF_SZ];
} BlogMem;

#define MEM_SIZE    ((sizeof(BlogMem) + 0xFFF) & ~(size_t)0xFFF)

static BlogMem *g_m;                      /* NULL: no log, no memory            */
static SceUID   g_mem_uid = -1;

static uint32_t *g_ring;                  /* the hook's pointer: NULL = push nothing */
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
static char   g_name[96];                 /* the game's title (header only)     */
static char   g_path[64], g_path_old[64];
static const char *g_cfg_path;

/* CPU stats, published by the worker (seqlock: odd = being written). */
static uint32_t g_cpu_seq;
static uint8_t  g_cpu_load[BLOG_CPU_CORES];
static uint16_t g_cpu_mhz;

/* ---- hot path: one word into the ring ------------------------------------- */

static inline void push(uint32_t w)
{
	uint32_t *ring = LD_RLX(g_ring);

	if (!ring)
		return;
	uint32_t h = LD_RLX(g_head), next = (h + 1) & RING_MASK;

	if (next == LD_ACQ(g_tail)) {         /* full: the word is dropped          */
		ST_RLX(g_dropped, LD_RLX(g_dropped) + 1);
		return;
	}
	ring[h] = w;
	ST_REL(g_head, next);                 /* word visible before the index      */
}

void blog_frame(uint32_t ft_us)
{
	push(ft_us > W_FT_MAX ? W_FT_MAX : ft_us);
}

void blog_res(uint16_t width, uint16_t height)
{
	push(W_EV(EV_RES, ((uint32_t)(width & 0xFFF) << 12) | (height & 0xFFF)));
}

void blog_cost(uint32_t us)
{
	push(W_EV(EV_COST, us > 0x0FFFFFFFu ? 0x0FFFFFFFu : us));
}

void blog_reset(void)
{
	push(W_EV(EV_RESET, 0));
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

static int      g_len;
static SceUID   g_fd = -1;
static uint32_t g_bytes;
static int      g_file_bad;               /* a write failed: stop trying        */

static void out_str(const char *s)
{
	while (*s && g_len < OBUF_SZ)
		g_m->buf[g_len++] = *s++;
}

static void out_ch(char c)
{
	if (g_len < OBUF_SZ)
		g_m->buf[g_len++] = c;
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

/* bytes as MiB with one decimal */
static void out_mib(uint32_t bytes)
{
	out_tenths(((bytes >> 10) * 10) >> 10, 0);
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

static void put_header(const char *what);

/* The buffer in one write. */
static void write_out(void)
{
	if (!g_len)
		return;
	if (g_fd < 0 || g_file_bad) {
		g_len = 0;
		return;
	}
	int w = sceIoWrite(g_fd, g_m->buf, g_len);
	if (w != g_len) {
		file_close();
		g_file_bad = 1;
	} else {
		g_bytes += (uint32_t)w;
	}
	g_len = 0;
}

/* One write per flush. Rotation happens only after a write, between flushes,
 * never mid-line: BLOG_MAX_BYTES is exceeded by at most one flush. */
static void flush(void)
{
	write_out();
	if (g_fd >= 0 && g_bytes >= BLOG_MAX_BYTES) {
		file_close();
		sceIoRemove(g_path_old);
		sceIoRename(g_path, g_path_old);
		file_open(1);
		if (g_fd >= 0) {                  /* the new file starts with its own header */
			put_header("continued (older lines in .old.log)");
			write_out();
		}
	}
}

/* room for one more line, else flush first */
static void need(int n)
{
	if (g_len + n > OBUF_SZ)
		flush();
}

/* ---- CPU sampling (worker thread only) ----------------------------------- */

static int      g_cpu_bad;                /* the call failed: no CPU stats      */
static int      g_cpu_primed;
static uint64_t g_cpu_t;
static uint64_t g_cpu_idle[BLOG_CPU_CORES];
static uint8_t  g_cpu_last[BLOG_CPU_CORES];  /* latest loads, for the log lines */

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

/* ---- clocks and free memory (worker thread only) ------------------------- */

typedef struct { uint16_t cpu, bus, gpu, xbar; } Clk;

static Clk g_clk;                         /* last logged                        */

static uint16_t mhz16(int v)
{
	return v > 0 ? (uint16_t)v : 0;
}

static void clk_read(Clk *c)
{
	c->cpu  = mhz16(scePowerGetArmClockFrequency());
	c->bus  = mhz16(scePowerGetBusClockFrequency());
	c->gpu  = mhz16(scePowerGetGpuClockFrequency());
	c->xbar = mhz16(scePowerGetGpuXbarClockFrequency());
}

static void out_clk(const Clk *c)
{
	out_str("# clk cpu=");
	out_uint(c->cpu, 0, ' ');
	out_str(" bus=");
	out_uint(c->bus, 0, ' ');
	out_str(" gpu=");
	out_uint(c->gpu, 0, ' ');
	out_str(" xbar=");
	out_uint(c->xbar, 0, ' ');
	out_str(" MHz");
}

static int      g_mem_ok;                 /* sceKernelGetFreeMemorySize worked  */
static uint32_t g_mem_user, g_mem_cdram, g_mem_phy;   /* bytes free, latest     */

/* ---- session state (worker thread only) ---------------------------------- */

static uint64_t g_t_us;                   /* session time, sum of frametimes    */
static uint16_t g_w, g_h;                 /* resolution last seen               */
static uint32_t g_dropped_seen;

/* the running state of the frame stream */
static uint32_t g_ema;                    /* running average frametime (us), 0 = none yet */
static uint32_t g_prev;                   /* previous frametime (us), 0 = none  */

typedef struct {                          /* the current one-second window      */
	uint32_t frames, ft_min, ft_max, hitches, hitch_lines, jit_n;
	uint64_t us, jit_sum;
} Win;

typedef struct {                          /* the section (from start or START+RIGHT) */
	uint32_t secs;                        /* complete one-second lines          */
	uint32_t frames;
	uint64_t us;
	uint32_t fps_min, fps_max;            /* tenths, of the one-second lines    */
	uint32_t ft_min, ft_max;              /* us                                 */
	uint32_t hitches, pauses;
	uint32_t jit_n;
	uint64_t jit_sum;
	uint32_t over_n;                      /* frames >= HIST_N * HIST_STEP_US    */
	uint64_t over_sum;
	uint32_t cpu_n, cpu_sum[BLOG_CPU_CORES];
	uint32_t cost_n, cost_max;
	uint64_t cost_sum;
	uint32_t mem_min_user;                /* bytes                              */
} Sum;

static Win      g_win;
static Sum      g_sum;
static uint32_t g_section = 1;

static void win_clear(void)
{
	Win z = {0};
	g_win = z;
	g_win.ft_min = 0xFFFFFFFFu;
}

static void sum_clear(void)
{
	Sum z = {0};
	g_sum = z;
	g_sum.fps_min = 0xFFFFFFFFu;
	g_sum.ft_min = 0xFFFFFFFFu;
	g_sum.mem_min_user = 0xFFFFFFFFu;
	win_clear();
	if (g_m)
		for (int i = 0; i < HIST_N; i++)
			g_m->hist[i] = 0;
}

static void mem_sample(void)
{
	SceKernelFreeMemorySizeInfo mi;

	mi.size = sizeof(mi);
	if (sceKernelGetFreeMemorySize(&mi) < 0)
		return;
	g_mem_user  = mi.size_user > 0 ? (uint32_t)mi.size_user : 0;
	g_mem_cdram = mi.size_cdram > 0 ? (uint32_t)mi.size_cdram : 0;
	g_mem_phy   = mi.size_phycont > 0 ? (uint32_t)mi.size_phycont : 0;
	g_mem_ok = 1;
	if (g_mem_user < g_sum.mem_min_user)
		g_sum.mem_min_user = g_mem_user;
}

/* ---- histogram: percentiles and lows ------------------------------------- */

/* The bin (0.1 ms steps, lower edge) holding the per_mille/1000 quantile of the
 * frametimes; HIST_N if it lies in the overflow (>= 200 ms). */
static uint32_t hist_pct(uint32_t per_mille)
{
	uint64_t rank = ((uint64_t)g_sum.frames * per_mille + 999) / 1000, acc = 0;

	if (!rank)
		rank = 1;
	for (uint32_t i = 0; i < HIST_N; i++) {
		acc += g_m->hist[i];
		if (acc >= rank)
			return i;
	}
	return HIST_N;
}

/* Average FPS (tenths) of the slowest per_mille/1000 of the frames: their count
 * over their total time. A bin counts as its middle (<= 0.05 ms off per frame);
 * the overflow counts as its exact average. */
static uint32_t hist_low10(uint32_t per_mille)
{
	uint64_t k = ((uint64_t)g_sum.frames * per_mille + 999) / 1000, left, sum = 0;

	if (!k)
		k = 1;
	left = k;
	if (g_sum.over_n) {
		uint64_t t = left < g_sum.over_n ? left : g_sum.over_n;
		sum += g_sum.over_sum * t / g_sum.over_n;
		left -= t;
	}
	for (uint32_t i = HIST_N; i-- > 0 && left; ) {
		uint64_t t = left < g_m->hist[i] ? left : g_m->hist[i];
		sum += t * (i * HIST_STEP_US + HIST_STEP_US / 2);
		left -= t;
	}
	return fps10((uint32_t)(k - left), sum);
}

static void out_bin(uint32_t bin)         /* a hist_pct() result, in ms         */
{
	if (bin >= HIST_N)
		out_ch('>');
	out_tenths(bin >= HIST_N ? HIST_N : bin, 0);
}

/* ---- summary -------------------------------------------------------------- */

static void put_sum(const char *why)
{
	if (!g_sum.frames)
		return;

	need(160);
	out_str("# sum ");
	out_str(why);
	out_str(" section=");
	out_uint(g_section, 0, ' ');
	out_str(" time=");
	out_uint((uint32_t)(g_sum.us / 1000000u), 0, ' ');
	out_str("s frames=");
	out_uint(g_sum.frames, 0, ' ');
	out_ch('\n');

	need(160);
	out_str("#   fps    avg=");
	out_tenths(fps10(g_sum.frames, g_sum.us), 0);
	if (g_sum.secs) {                     /* min/max of the one-second lines */
		out_str(" min=");
		out_tenths(g_sum.fps_min, 0);
		out_str(" max=");
		out_tenths(g_sum.fps_max, 0);
	}
	out_str(" | low 1%=");
	out_tenths(hist_low10(10), 0);
	out_str(" 0.1%=");
	out_tenths(hist_low10(1), 0);
	out_ch('\n');

	need(200);
	out_str("#   ft     avg=");
	out_ms((uint32_t)(g_sum.us / g_sum.frames), 0);
	out_str(" min=");
	out_ms(g_sum.ft_min, 0);
	out_str(" max=");
	out_ms(g_sum.ft_max, 0);
	out_str(" | p50=");
	out_bin(hist_pct(500));
	out_str(" p95=");
	out_bin(hist_pct(950));
	out_str(" p99=");
	out_bin(hist_pct(990));
	out_str(" p99.9=");
	out_bin(hist_pct(999));
	out_str(" ms\n");

	need(100);
	out_str("#   jitter avg=");
	out_ms(g_sum.jit_n ? (uint32_t)(g_sum.jit_sum / g_sum.jit_n) : 0, 0);
	out_str(" ms | hitches=");
	out_uint(g_sum.hitches, 0, ' ');
	out_str(" pauses=");
	out_uint(g_sum.pauses, 0, ' ');
	out_ch('\n');

	if (g_sum.cpu_n) {
		need(100);
		out_str("#   cpu    avg");
		for (int c = 0; c < BLOG_CPU_CORES; c++) {
			out_str(" c");
			out_ch((char)('0' + c));
			out_ch('=');
			out_uint((g_sum.cpu_sum[c] + g_sum.cpu_n / 2) / g_sum.cpu_n, 0, ' ');
			out_ch('%');
		}
		out_ch('\n');
	}

	if (g_mem_ok) {
		need(120);
		out_str("#   mem    free user=");
		out_mib(g_mem_user);
		out_str(" (min ");
		out_mib(g_sum.mem_min_user < g_mem_user ? g_sum.mem_min_user : g_mem_user);
		out_str(") cdram=");
		out_mib(g_mem_cdram);
		out_str(" phycont=");
		out_mib(g_mem_phy);
		out_str(" MiB\n");
	}

	if (g_sum.cost_n) {
		need(100);
		out_str("#   vtss   cost avg=");
		out_uint((uint32_t)((g_sum.cost_sum + g_sum.cost_n / 2) / g_sum.cost_n), 0, ' ');
		out_str(" max=");
		out_uint(g_sum.cost_max, 0, ' ');
		out_str(" us/frame (1 in ");
		out_uint(BLOG_COST_EVERY, 0, ' ');
		out_str(" frames, n=");
		out_uint(g_sum.cost_n, 0, ' ');
		out_str(")\n");
	}
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
	if (g_name[0]) {
		need(sizeof(g_name) + 16);
		out_str("# game ");
		out_str(g_name);
		out_ch('\n');
	}
	need(80);
	out_clk(&g_clk);
	out_ch('\n');
	if (g_mem_ok) {
		need(80);
		out_str("# mem free user=");
		out_mib(g_mem_user);
		out_str(" cdram=");
		out_mib(g_mem_cdram);
		out_str(" phycont=");
		out_mib(g_mem_phy);
		out_str(" MiB\n");
	}
	if (g_w) {                            /* a continued file: the resolution again */
		need(40);
		out_str("# res ");
		out_uint(g_w, 0, ' ');
		out_ch('x');
		out_uint(g_h, 0, ' ');
		out_ch('\n');
	}
	need(100);
	out_str(g_cpu ? COLUMNS_CPU : COLUMNS);
}

/* ---- the frame stream ------------------------------------------------------ */

static void emit_second(void)
{
	uint32_t t = (uint32_t)(g_t_us / 1000000u);
	uint32_t f10 = fps10(g_win.frames, g_win.us);
	uint32_t avg = (uint32_t)(g_win.us / g_win.frames);
	int cpu = g_cpu && !g_cpu_bad && LD_RLX(g_cpu_seq);

	need(96);
	out_uint(t, 7, ' ');
	out_ch(' ');
	out_tenths(f10, 3);
	out_ch(' ');
	out_ms(avg, 4);
	out_ch(' ');
	out_ms(g_win.ft_min, 4);
	out_ch(' ');
	out_ms(g_win.ft_max, 4);
	out_ch(' ');
	out_ms(g_win.jit_n ? (uint32_t)(g_win.jit_sum / g_win.jit_n) : 0, 4);
	out_ch(' ');
	out_uint(g_win.hitches, 6, ' ');
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
	if (f10 < g_sum.fps_min) g_sum.fps_min = f10;
	if (f10 > g_sum.fps_max) g_sum.fps_max = f10;
	win_clear();
	if (g_sum.secs % BLOG_SUM_SECS == 0)
		put_sum("minute");
}

static void on_frame(uint32_t ft)
{
	g_t_us += ft;

	if (ft >= BLOG_PAUSE_US) {            /* suspend, loading screen, ...: kept out of every statistic */
		need(60);
		out_str("# pause ");
		out_ms(ft, 0);
		out_str(" ms at t=");
		out_uint((uint32_t)(g_t_us / 1000000u), 0, ' ');
		out_ch('\n');
		g_sum.pauses++;
		g_ema = g_prev = 0;               /* the stream restarts after a pause  */
		return;
	}

	/* hitch: well above the running average, and long enough to be seen */
	if (g_ema && ft >= BLOG_HITCH_MIN_US && ft * 100 > g_ema * BLOG_HITCH_PCT) {
		g_win.hitches++;
		g_sum.hitches++;
		if (g_win.hitch_lines++ < HITCH_LINES_SEC) {
			need(64);
			out_str("# hitch ");
			out_ms(ft, 0);
			out_str(" ms at t=");
			out_tenths((uint32_t)(g_t_us / 100000u), 0);
			out_ch('\n');
		}
	}
	if (!g_ema)
		g_ema = ft;
	else if (ft > g_ema)
		g_ema += (ft - g_ema) >> 4;
	else
		g_ema -= (g_ema - ft) >> 4;

	/* jitter: how much one frame differs from the one before */
	if (g_prev) {
		uint32_t d = ft > g_prev ? ft - g_prev : g_prev - ft;
		g_win.jit_sum += d;
		g_win.jit_n++;
		g_sum.jit_sum += d;
		g_sum.jit_n++;
	}
	g_prev = ft;

	/* section: histogram and totals */
	uint32_t bin = ft / HIST_STEP_US;
	if (bin < HIST_N) {
		g_m->hist[bin]++;
	} else {
		g_sum.over_n++;
		g_sum.over_sum += ft;
	}
	g_sum.frames++;
	g_sum.us += ft;
	if (ft < g_sum.ft_min) g_sum.ft_min = ft;
	if (ft > g_sum.ft_max) g_sum.ft_max = ft;

	/* window */
	g_win.frames++;
	g_win.us += ft;
	if (ft < g_win.ft_min) g_win.ft_min = ft;
	if (ft > g_win.ft_max) g_win.ft_max = ft;
	if (g_win.us >= BLOG_SEC_US)
		emit_second();
}

static void on_event(uint32_t w)
{
	uint32_t p = W_PAYLOAD(w);

	switch (W_TYPE(w)) {
	case EV_RESET:
		put_sum("reset");
		need(40);
		out_str("# reset (START+RIGHT)\n");
		g_section++;
		sum_clear();
		break;
	case EV_RES:
		g_w = (uint16_t)(p >> 12);
		g_h = (uint16_t)(p & 0xFFF);
		need(40);
		out_str("# res ");
		out_uint(g_w, 0, ' ');
		out_ch('x');
		out_uint(g_h, 0, ' ');
		out_ch('\n');
		break;
	case EV_COST:
		g_sum.cost_n++;
		g_sum.cost_sum += p;
		if (p > g_sum.cost_max) g_sum.cost_max = p;
		break;
	}
}

/* a change of the clocks, with the session time it was noticed at */
static void clk_check(void)
{
	Clk c;

	clk_read(&c);
	if (c.cpu == g_clk.cpu && c.bus == g_clk.bus && c.gpu == g_clk.gpu && c.xbar == g_clk.xbar)
		return;
	g_clk = c;
	need(100);
	out_clk(&c);
	out_str(" at t=");
	out_tenths((uint32_t)(g_t_us / 100000u), 0);
	out_ch('\n');
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
	if (g_fd < 0 && !g_file_bad) {        /* first time: open, session header */
		file_open(0);
		if (g_fd >= 0) {
			clk_read(&g_clk);
			mem_sample();
			put_header("start");
		}
	}

	uint32_t tail = LD_RLX(g_tail);
	while (tail != LD_ACQ(g_head)) {      /* index read before the word          */
		uint32_t w = g_m->ring[tail];
		tail = (tail + 1) & RING_MASK;
		ST_REL(g_tail, tail);             /* slot free after the copy            */
		if (g_fd >= 0) {
			if (w & W_EVENT)
				on_event(w);
			else
				on_frame(w);
		}
	}

	if (g_fd >= 0) {
		mem_sample();
		clk_check();
	}

	uint32_t d = LD_RLX(g_dropped);
	if (d != g_dropped_seen && g_fd >= 0) {
		need(40);
		out_str("# dropped ");
		out_uint(d - g_dropped_seen, 0, ' ');
		out_str(" (worker late)\n");
		g_dropped_seen = d;
	}
}

static int worker(SceSize args, void *argp)
{
	(void)args; (void)argp;
	/* Wake every BLOG_WAKE_US: sample the CPU, drain the ring; write the file
	 * every BLOG_FLUSH_US. */
	uint64_t last_flush = sceKernelGetProcessTimeWide();

	if (g_cpu)
		cpu_sample();                     /* prime                               */
	while (!LD_ACQ(g_stop)) {
		SceUInt timeout = BLOG_WAKE_US;
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

/* The title goes into a text file: control characters (a newline in a title
 * would start a fake log line) become spaces. */
static void set_name(const char *name)
{
	int n = 0;

	for (; name && name[n] && n < (int)sizeof(g_name) - 1; n++)
		g_name[n] = ((unsigned char)name[n] < 0x20 || name[n] == 0x7F) ? ' ' : name[n];
	while (n && g_name[n - 1] == ' ')
		n--;
	g_name[n] = 0;
}

static void mem_release(void)
{
	if (g_mem_uid >= 0)
		sceKernelFreeMemBlock(g_mem_uid);
	g_mem_uid = -1;
	g_m = NULL;
}

int blog_init(const char *titleid, const char *name, const char *cfg_path, int log, int cpu)
{
	g_cfg_path = cfg_path;
	g_cpu = cpu ? 1 : 0;
	g_log = 0;
	if (log && titleid && blog_title_ok(titleid) && !blog_title_system(titleid)) {
		void *base = 0;

		/* the log's ONE memblock (ring + histogram + line buffer): only here */
		g_mem_uid = sceKernelAllocMemBlock("vtss_log", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MEM_SIZE, 0);
		if (g_mem_uid < 0 || sceKernelGetMemBlockBase(g_mem_uid, &base) < 0 || !base) {
			mem_release();                /* no memory: no log (CPU stats may still run) */
		} else {
			g_m = (BlogMem *)base;
			cpy(g_title, sizeof(g_title), titleid, 0, 0);
			set_name(name);
			cpy(g_path, sizeof(g_path), VTSS_DIR "/", titleid, ".log");
			cpy(g_path_old, sizeof(g_path_old), VTSS_DIR "/", titleid, ".old.log");
			g_log = 1;
		}
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
	if (g_log)
		ST_REL(g_ring, g_m->ring);        /* the hook starts pushing             */
	return 0;
fail:
	g_log = g_cpu = 0;                    /* the hook pushes nothing             */
	if (g_sema >= 0) {
		sceKernelDeleteSema(g_sema);
		g_sema = -1;
	}
	mem_release();
	return -2;
}

int blog_cpu_active(void)
{
	return g_cpu;
}

int blog_log_active(void)
{
	return g_log;
}

void blog_fini(void)
{
	if (g_thid < 0)
		return;
	SceUID th = g_thid;
	ST_REL(g_ring, NULL);                 /* the hook pushes nothing from now on */
	ST_REL(g_stop, 1);
	sceKernelSignalSema(g_sema, 1);
	SceUInt timeout = 2000000;
	sceKernelWaitThreadEnd(th, NULL, &timeout);
	sceKernelDeleteThread(th);
	g_thid = -1;
	g_log = g_cpu = 0;
	sceKernelDeleteSema(g_sema);
	g_sema = -1;
	/* The memblock is NOT freed: the display hook may still be inside a blog_*
	 * call, and a push into freed memory would fault. The process owns the
	 * block and the kernel reclaims it at exit; g_ring is already NULL, so
	 * nothing pushes into it any more. */
}