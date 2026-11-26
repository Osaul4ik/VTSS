#include <vitasdk.h>
#include <taihen.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include "renderer.h"
#include "benchlog.h"
#include "vtss.h"

/*
 * HOT PATH: sceDisplaySetFrameBuf_patched runs once per frame in the game's
 * render thread. What it does per frame: one time read, buttons every 2nd
 * frame, a few integer updates, the text and charts of the current mode.
 * No file I/O, no float formatting, no division in the chart loops (the chart
 * y of each sample is computed once, when the sample is taken). Strings that
 * change once a second (FPS, CPU) are rebuilt only then.
 */

#define FPS_TIMER_TICK    1000000
#define FRAMETIMES_NUM 360
#define FRAMERATES_NUM 15

#define NUM_MODES 4
enum {
	NO_UI,
	MINIMAL_UI, // FPS counter
	STANDARD_UI, // Frametime chart + FPS counter
	EXTENDED_UI // Frametime chart + FPS chart + Framecounter
};
int mode_ui = STANDARD_UI;

static SceUID hook = -1;
static tai_hook_ref_t display_ref;
static uint32_t tick = 0;
static uint32_t frametime_tick = 0;
static uint32_t frames = 0;
static uint32_t poll_count = 0;
static int cur_fps = 0;
static int max_fps = 0, min_fps = 9999;
static uint32_t cur_ft_us = 0;
static uint32_t max_ft_us = 0, min_ft_us = 999000; // shown as 0.00 / 999.00 until set
static int frametime_idx = FRAMETIMES_NUM;
static int framerate_idx = FRAMERATES_NUM;
static SceCtrlData pad;
static uint32_t oldpad = 0;
char config_path[256];

// Chart y offsets, computed once per sample (hi: big charts, lo: small)
#define FRAMETIME_CHART_Y 15
#define FRAMETIME_BIG_CHART_Y 25
#define FRAMERATE_BIG_CHART_Y 160
#define FRAMERATE_CHART_Y 80
static uint8_t ft_hy[FRAMETIMES_NUM * 2]; // 100 - ms (ms clamped to 100)
static uint8_t ft_ly[FRAMETIMES_NUM * 2]; // 50 - ms / 2
static uint8_t fr_hy[FRAMERATES_NUM * 2]; // 60 - fps (fps clamped to 60)
static uint8_t fr_ly[FRAMERATES_NUM * 2]; // 30 - fps / 2

// Log: frametime min/max of the current one-second window (us)
static uint32_t win_ft_min = 0xFFFFFFFF, win_ft_max = 0;

// Cached strings
static char fps_str[48];
static int  fps_dirty = 1;
static char cpu_str[48];
static uint32_t cpu_seq_shown = 0;
static int  cpu_on = 0;
// Frame count as decimal digits, right-aligned, incremented in place
static char fc_dig[24];
static int  fc_start;

/* ---- tiny integer formatter (no printf, no floats) ---------------------- */
static char *p_str(char *p, const char *s) {
	while (*s)
		*p++ = *s++;
	return p;
}
static char *p_uint(char *p, uint32_t v) {
	char t[10];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n)
		*p++ = t[--n];
	return p;
}
// microseconds as milliseconds with 2 decimals, rounded (like "%.2f")
static char *p_ms(char *p, uint32_t us) {
	uint32_t c = (us + 5) / 10;
	p = p_uint(p, c / 100);
	*p++ = '.';
	c %= 100;
	*p++ = (char)('0' + c / 10);
	*p++ = (char)('0' + c % 10);
	return p;
}

static void build_fps_str(void) {
	char *p = p_str(fps_str, "FPS: ");
	p = p_uint(p, (uint32_t)cur_fps);
	p = p_str(p, " (Min: ");
	p = p_uint(p, (uint32_t)min_fps);
	p = p_str(p, ", Max: ");
	p = p_uint(p, (uint32_t)max_fps);
	*p++ = ')';
	*p = 0;
	fps_dirty = 0;
}

static void build_cpu_str(void) {
	uint8_t load[BLOG_CPU_CORES];
	uint16_t mhz;
	uint32_t seq;

	if (!blog_cpu_read(load, &mhz, &seq))
		return; // none yet or changed meanwhile: keep the old one, retry next frame
	char *p = p_str(cpu_str, "CPU:");
	for (int c = 0; c < BLOG_CPU_CORES; c++) {
		*p++ = ' ';
		p = p_uint(p, load[c]);
		*p++ = '%';
	}
	*p++ = ' ';
	p = p_uint(p, mhz);
	p = p_str(p, " MHz");
	*p = 0;
	cpu_seq_shown = seq;
}

static void fc_init(void) {
	fc_start = sizeof(fc_dig) - 2;
	fc_dig[fc_start] = '0';
	fc_dig[sizeof(fc_dig) - 1] = 0;
}
static void fc_inc(void) {
	int i = sizeof(fc_dig) - 2;
	const int lo = fc_start > 0 ? fc_start : 0; // explicit bound (fc_start is 0..22)
	while (i >= lo && fc_dig[i] == '9')
		fc_dig[i--] = '0';
	if (i >= lo)
		fc_dig[i]++;
	else if (lo > 0)
		fc_dig[fc_start = lo - 1] = '1';
}

static void save_config(void) {
	if (!config_path[0])
		return;
	if (blog_save_cfg(mode_ui) == 0) // the worker thread writes it, not the display hook
		return;
	sceIoMkdir(VTSS_DIR, 0777); // a button press, not every frame
	SceUID fd = sceIoOpen(config_path, SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, &mode_ui, sizeof(int));
		sceIoClose(fd);
	}
}

int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync) {
	if (!pParam)
		return TAI_CONTINUE(int, display_ref, pParam, sync);

	// Buttons every 2nd frame: enough for the combos, one syscall less per frame
	if (!(poll_count++ & 1)) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t pressed = pad.buttons & ~oldpad;
		if (pad.buttons & SCE_CTRL_START) {
			if (pressed & SCE_CTRL_LEFT) {
				mode_ui = (mode_ui + 1) % NUM_MODES;
				save_config();
			}
			if (pressed & SCE_CTRL_RIGHT) {
				max_ft_us = 0;
				min_ft_us = 999000;
				max_fps = 0;
				min_fps = 9999;
				fps_dirty = 1;
				blog_reset();
			}
		}
		oldpad = pad.buttons;
	}
	fc_inc();

	const uint32_t now = sceKernelGetProcessTimeLow();
	const int hi = pParam->height > 408;
	const int mode = mode_ui;
	updateFramebuf(pParam);

	if (tick == 0)
		tick = now;
	else {
		uint32_t dt = now - tick;
		if (dt > FPS_TIMER_TICK) {
			blog_second(frames, dt, win_ft_min == 0xFFFFFFFF ? 0 : win_ft_min, win_ft_max,
				pParam->width, pParam->height);
			win_ft_min = 0xFFFFFFFF;
			win_ft_max = 0;
			if (++framerate_idx >= FRAMERATES_NUM * 2)
				framerate_idx = 0;
			int f = (int)frames;
			int fc = f > 60 ? 60 : f;
			fr_hy[framerate_idx] = (uint8_t)(60 - fc);
			fr_ly[framerate_idx] = (uint8_t)(30 - fc / 2);
			cur_fps = f;
			if (f > max_fps)
				max_fps = f;
			if (f < min_fps)
				min_fps = f;
			fps_dirty = 1;
			frames = 0;
			tick = now;
		}

		if (mode != NO_UI) {
			if (fps_dirty)
				build_fps_str();
			if (hi)
				drawBigString(5, mode == MINIMAL_UI ? 5 : 140, fps_str);
			else
				drawString(5, mode == MINIMAL_UI ? 5 : 70, fps_str);
		}

		if (mode == EXTENDED_UI) {
			int idx = framerate_idx;
			if (hi) {
				int x = 80, cy = FRAMERATE_BIG_CHART_Y + fr_hy[idx];
				for (int i = 0; i < FRAMERATES_NUM; i++) {
					idx = idx ? idx - 1 : FRAMERATES_NUM * 2 - 1;
					int py = FRAMERATE_BIG_CHART_Y + fr_hy[idx];
					drawLine(x - 5, py, x, cy, 0x0000FF00);
					x -= 5;
					cy = py;
				}
			} else {
				int x = 35, cy = FRAMERATE_CHART_Y + fr_ly[idx];
				for (int i = 0; i < FRAMERATES_NUM; i++) {
					idx = idx ? idx - 1 : FRAMERATES_NUM * 2 - 1;
					int py = FRAMERATE_CHART_Y + fr_ly[idx];
					drawLine(x - 2, py, x, cy, 0x0000FF00);
					x -= 2;
					cy = py;
				}
			}
		}
	}
	frames++;

	if (frametime_tick) {
		uint32_t ft = now - frametime_tick;
		if (ft < win_ft_min)
			win_ft_min = ft;
		if (ft > win_ft_max)
			win_ft_max = ft;
		uint32_t c = 100000 - (ft > 100000 ? 100000 : ft);
		ft_hy[frametime_idx] = (uint8_t)(c / 1000);
		ft_ly[frametime_idx] = (uint8_t)(c / 2000);
		cur_ft_us = ft;
		if (ft < min_ft_us)
			min_ft_us = ft;
		if (ft > max_ft_us)
			max_ft_us = ft;
		if (mode >= STANDARD_UI) {
			char s[64];
			char *p = p_str(s, "Frametime: ");
			p = p_ms(p, cur_ft_us);
			p = p_str(p, "ms (Min: ");
			p = p_ms(p, min_ft_us);
			p = p_str(p, "ms, Max: ");
			p = p_ms(p, max_ft_us);
			p = p_str(p, "ms)");
			*p = 0;
			if (hi) {
				drawBigString(5, 5, s);
				if (mode == EXTENDED_UI) {
					drawBigString(5, 245, "Frame Count: ");
					drawBigString(5 + 13 * 12, 245, fc_dig + fc_start);
				}
			} else {
				drawString(5, 5, s);
				if (mode == EXTENDED_UI) {
					drawString(5, 115, "Frame Count: ");
					drawString(5 + 13 * 6, 115, fc_dig + fc_start);
				}
			}
		}
	}
	frametime_tick = now;

	// CPU stats line (CPU stats on): rebuilt once a second, under the other text
	if (cpu_on && mode != NO_UI) {
		if (blog_cpu_seq() != cpu_seq_shown)
			build_cpu_str();
		if (cpu_str[0]) {
			if (hi)
				drawBigString(5, mode == MINIMAL_UI ? 27 : mode == STANDARD_UI ? 162 : 267, cpu_str);
			else
				drawString(5, mode == MINIMAL_UI ? 17 : mode == STANDARD_UI ? 82 : 127, cpu_str);
		}
	}

	if (mode >= STANDARD_UI) {
		int idx = frametime_idx;
		if (hi) {
			int x = 365, cy = FRAMETIME_BIG_CHART_Y + ft_hy[idx];
			for (int i = 0; i < FRAMETIMES_NUM; i++) {
				idx = idx ? idx - 1 : FRAMETIMES_NUM * 2 - 1;
				int py = FRAMETIME_BIG_CHART_Y + ft_hy[idx];
				drawLine(x - 1, py, x, cy, 0x0000FF00);
				x--;
				cy = py;
			}
		} else {
			int x = 185, cy = FRAMETIME_CHART_Y + ft_ly[idx];
			for (int i = 0; i < FRAMETIMES_NUM / 2; i++) {
				idx = idx ? idx - 1 : FRAMETIMES_NUM - 1;
				int py = FRAMETIME_CHART_Y + ft_ly[idx];
				drawLine(x - 1, py, x, cy, 0x0000FF00);
				x--;
				cy = py;
			}
		}
	}
	if (++frametime_idx >= (hi ? FRAMETIMES_NUM * 2 : FRAMETIMES_NUM))
		frametime_idx = 0;

	return TAI_CONTINUE(int, display_ref, pParam, sync);
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	// Settings app: the page is VTSS_settings.suprx's job, nothing to do here
	if (vtss_in_settings())
		return SCE_KERNEL_START_NO_RESIDENT;

	// VTSS Enable off: unloaded right away (one small file read, nothing kept)
	VtssSettings set;
	vtss_settings_load(&set);
	if (!set.enable)
		return SCE_KERNEL_START_NO_RESIDENT;

	char titleid[16] = {0};
	if (sceAppMgrAppParamGetString(0, 12, titleid, sizeof(titleid)) < 0)
		titleid[0] = 0;
	titleid[sizeof(titleid) - 1] = 0;
	if (blog_title_ok(titleid)) { // only a clean TitleID becomes a file name
		sceClibSnprintf(config_path, sizeof(config_path), VTSS_DIR "/%s.cfg", titleid);
		int cfg = mode_ui;
		SceUID fd = sceIoOpen(config_path, SCE_O_RDONLY, 0777);
		if (fd >= 0) {
			if (sceIoRead(fd, &cfg, sizeof(cfg)) == (int)sizeof(cfg) && cfg >= 0 && cfg < NUM_MODES)
				mode_ui = cfg;
			sceIoClose(fd);
		}
	}

	// Charts start empty, as before (0 ms / 0 fps at the bottom)
	for (int i = 0; i < FRAMETIMES_NUM * 2; i++) {
		ft_hy[i] = 100;
		ft_ly[i] = 50;
	}
	for (int i = 0; i < FRAMERATES_NUM * 2; i++) {
		fr_hy[i] = 60;
		fr_ly[i] = 30;
	}
	fc_init();

	// Worker thread only for Log to file and/or CPU stats (system apps and a
	// bad TitleID: no log)
	blog_init(titleid, config_path[0] ? config_path : NULL, set.log, set.cpu);
	cpu_on = blog_cpu_active();

	// Hooking sceDisplaySetFrameBuf
	hook = taiHookFunctionImport(&display_ref,
						TAI_MAIN_MODULE,
						TAI_ANY_LIBRARY,
						0x7A410B64,
						sceDisplaySetFrameBuf_patched);

	setTextColor(0x00FFFF00);

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	// Freeing hook
	if (hook >= 0)
		taiHookRelease(hook, display_ref);
	blog_fini();

	return SCE_KERNEL_STOP_SUCCESS;
}
