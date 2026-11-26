/*
 * benchlog.c - per-game benchmark log. See benchlog.h.
 *
 * Producer: the display hook (one thread). Consumer: the writer thread. The
 * ring is single-producer / single-consumer, so no lock: the producer owns
 * g_head, the consumer owns g_tail, a barrier orders the record before the
 * index. A full ring drops the record and counts it (reported in the log).
 *
 * Formatting is a tiny integer-only formatter (no libc printf, no floats):
 * times are kept in microseconds and printed as ms with 2 decimals, fps with
 * one decimal.
 */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <stddef.h>
#include <stdint.h>

#include "benchlog.h"

#ifndef VIBES_BUILD
#define VIBES_BUILD "dev"
#endif

#define BLOG_THREAD_PRIO   0xA0           /* below the game's threads           */
#define BLOG_THREAD_STACK  0x2000
#define COLUMNS "#     t   fps  ft_avg  ft_min  ft_max  (s, ms)\n"
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
static volatile uint32_t g_head;          /* written by the producer only       */
static volatile uint32_t g_tail;          /* written by the consumer only       */
static volatile uint32_t g_dropped;       /* producer only                      */

static volatile int g_on;                 /* logging on/off                     */
static volatile int g_stop;
static volatile int g_cfg_dirty;
static volatile int g_cfg_mode, g_cfg_log;

static SceUID g_thid = -1;
static SceUID g_sema = -1;
static char   g_title[16];
static char   g_path[64], g_path_old[64];
static const char *g_cfg_path;

static void push(const BlogRec *r)
{
	uint32_t h = g_head, next = (h + 1) & RING_MASK;

	if (next == g_tail) {
		g_dropped++;
		return;
	}
	g_ring[h] = *r;
	__sync_synchronize();                 /* record visible before the index    */
	g_head = next;
}

void blog_second(uint32_t frames, uint32_t dt_us, uint32_t ft_min_us,
                 uint32_t ft_max_us, uint16_t width, uint16_t height)
{
	if (!g_on || g_thid < 0)
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
	if (!g_on || g_thid < 0)
		return;
	BlogRec r = {0};
	r.kind = REC_RESET;
	push(&r);
}

void blog_set_on(int on) { g_on = on ? 1 : 0; }
int  blog_is_on(void)    { return g_on; }
int  blog_active(void)   { return g_thid >= 0; }

int blog_save_cfg(int mode_ui, int log_on)
{
	if (g_thid < 0)
		return -1;
	g_cfg_mode = mode_ui;
	g_cfg_log = log_on;
	__sync_synchronize();
	g_cfg_dirty = 1;
	sceKernelSignalSema(g_sema, 1);        /* a button press, not every frame: wake the writer */
	return 0;
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

/* ---- output buffer + formatter (writer thread only) ---------------------- */

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

/* ---- file (writer thread only) ------------------------------------------ */

static void file_open(int trunc)
{
	if (g_file_bad || g_fd >= 0)
		return;
	sceIoMkdir(BLOG_DIR, 0777);
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
		char hdr[128];
		int n = 0;
		const char *parts[] = { "# ViBeS ", VIBES_BUILD, " | ", g_title,
		                        " | continued (older lines in .old.log)\n", COLUMNS };
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

/* ---- session state (writer thread only) ---------------------------------- */

static uint64_t g_t_us;                   /* session time, sum of windows       */
static uint16_t g_w, g_h;                 /* resolution last logged             */
static uint32_t g_dropped_seen;
static int      g_header_done;

typedef struct {
	uint32_t secs, frames;
	uint64_t us;
	uint32_t fps_min, fps_max;            /* tenths                              */
	uint32_t ft_min, ft_max;              /* us                                  */
	uint32_t pauses;
} Sum;

static Sum      g_sum;
static uint32_t g_section = 1;

static void sum_clear(void)
{
	g_sum.secs = g_sum.frames = 0;
	g_sum.us = 0;
	g_sum.fps_min = 0xFFFFFFFFu;
	g_sum.fps_max = 0;
	g_sum.ft_min = 0xFFFFFFFFu;
	g_sum.ft_max = 0;
	g_sum.pauses = 0;
}

static void put_sum(const char *why)
{
	if (!g_sum.secs)
		return;
	need(200);
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
	if (g_sum.pauses) {
		out_str(" | pauses=");
		out_uint(g_sum.pauses, 0, ' ');
	}
	out_ch('\n');
}

static void put_columns(void)
{
	need(80);
	out_str(COLUMNS);
}

static void put_header(const char *what)
{
	SceDateTime dt;

	need(160);
	out_str("# ViBeS ");
	out_str(VIBES_BUILD);
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
	put_columns();
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

	uint32_t f10 = fps10(r->frames, r->dt_us);
	uint32_t avg = r->frames ? r->dt_us / r->frames : 0;

	need(48);
	out_uint(t, 7, ' ');
	out_ch(' ');
	out_tenths(f10, 3);
	out_ch(' ');
	out_ms(avg, 4);
	out_ch(' ');
	out_ms(r->ft_min_us, 4);
	out_ch(' ');
	out_ms(r->ft_max_us, 4);
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
	int cfg[2] = { g_cfg_mode, g_cfg_log };

	g_cfg_dirty = 0;
	if (!g_cfg_path)
		return;
	SceUID fd = sceIoOpen(g_cfg_path, SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, cfg, sizeof(cfg));
		sceIoClose(fd);
	}
}

/* one wake of the writer */
static void step(void)
{
	if (g_cfg_dirty)
		save_cfg();

	int on = g_on;

	if (on && g_fd < 0 && !g_file_bad) {
		file_open(0);
		if (g_fd >= 0) {
			put_header(g_header_done ? "log on" : "start");
			g_header_done = 1;
		}
	}

	uint32_t tail = g_tail;
	while (tail != g_head) {
		__sync_synchronize();             /* index read before the record        */
		BlogRec r = g_ring[tail];
		tail = (tail + 1) & RING_MASK;
		g_tail = tail;
		if (g_fd >= 0)
			handle(&r);
	}

	uint32_t d = g_dropped;
	if (d != g_dropped_seen && g_fd >= 0) {
		need(40);
		out_str("# dropped ");
		out_uint(d - g_dropped_seen, 0, ' ');
		out_str(" s (writer late)\n");
		g_dropped_seen = d;
	}

	if (!on && g_fd >= 0) {
		put_sum("log off");
		need(20);
		out_str("# log off\n");
		g_section++;
		sum_clear();
		flush();
		file_close();
		return;
	}
	flush();
}

static int writer(SceSize args, void *argp)
{
	(void)args; (void)argp;
	while (!g_stop) {
		SceUInt timeout = BLOG_FLUSH_US;
		sceKernelWaitSema(g_sema, 1, &timeout);   /* timeout = the normal wake */
		step();
	}
	step();
	if (g_fd >= 0) {
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

int blog_init(const char *titleid, int log_on, const char *cfg_path)
{
	g_cfg_path = cfg_path;
	g_on = log_on ? 1 : 0;
	if (!titleid || !blog_title_ok(titleid) || blog_title_system(titleid))
		return -1;

	cpy(g_title, sizeof(g_title), titleid, 0, 0);
	cpy(g_path, sizeof(g_path), BLOG_DIR "/", titleid, ".log");
	cpy(g_path_old, sizeof(g_path_old), BLOG_DIR "/", titleid, ".old.log");
	sum_clear();

	g_sema = sceKernelCreateSema("ViBeSLogSema", 0, 0, 1, NULL);
	if (g_sema < 0)
		return -2;
	g_thid = sceKernelCreateThread("ViBeSLog", writer, BLOG_THREAD_PRIO,
	                               BLOG_THREAD_STACK, 0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
	if (g_thid < 0) {
		sceKernelDeleteSema(g_sema);
		g_sema = -1;
		return -3;
	}
	if (sceKernelStartThread(g_thid, 0, NULL) < 0) {
		sceKernelDeleteThread(g_thid);
		g_thid = -1;
		sceKernelDeleteSema(g_sema);
		g_sema = -1;
		return -4;
	}
	return 0;
}

void blog_fini(void)
{
	if (g_thid < 0)
		return;
	SceUID th = g_thid;
	g_stop = 1;
	sceKernelSignalSema(g_sema, 1);
	SceUInt timeout = 2000000;
	sceKernelWaitThreadEnd(th, NULL, &timeout);
	sceKernelDeleteThread(th);
	g_thid = -1;
	sceKernelDeleteSema(g_sema);
	g_sema = -1;
}
