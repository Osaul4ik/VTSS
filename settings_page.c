/*
 * settings_page.c - the VTSS page in the Settings app (NPXS10015).
 *
 * Same recipe as vita-memio's page: hook sceKernelLoadStartModule in
 * SceSettings to wait for system_settings_core.suprx, then hook
 *   - scePafMiscLoadXmlLayout: splice a "VTSS" setting_list into the Devices
 *     page, last in its main list (id="peripherals_settings");
 *   - the SceRegMgr get/set/info in SceSystemSettingsCore: back the rows with
 *     a private /CONFIG/VTSS namespace stored in ux0:data/VTSS/settings.bin
 *     (no registry write); a change is saved at once and applies to apps
 *     started after it;
 *   - scePafToplevelGetText: every title/hint id ("vtss_...") resolved here,
 *     the status row from the current settings;
 *   - scePafToplevelInitPluginFunctions: the Devices page's button handler is
 *     wrapped so the status button opens its dialog.
 *
 * The page:  VTSS Enable  (toggle)
 *            Status       (button: Loaded / Not loaded, details in its dialog)
 *            Log to file  (toggle)
 *            CPU stats    (toggle)
 *
 * All hooks are import hooks (passthrough): if anything does not match, the
 * page simply does not appear. Other plugins' pages on the Devices page
 * (vita-memio) chain through the same hooks.
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <taihen.h>
#include <stdint.h>

#include "vtss.h"
#include "settings_page.h"

/* ---- small string helpers ------------------------------------------------ */
static int s_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int s_eq(const char *a, const char *b)
{
	for (int i = 0;; i++) {
		if (a[i] != b[i]) return 0;
		if (!a[i]) return 1;
	}
}
static int s_pfx(const char *s, const char *p)
{
	for (int i = 0; p[i]; i++)
		if (s[i] != p[i]) return 0;
	return 1;
}
static const char *mem_find(const char *h, int hn, const char *n, int nn)
{
	for (int i = 0; i + nn <= hn; i++) {
		int j = 0;
		while (j < nn && h[i + j] == n[j]) j++;
		if (j == nn) return h + i;
	}
	return 0;
}
static void s_cpy(char *d, const char *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }

/* ---- settings ------------------------------------------------------------ */
static VtssSettings g_set;

#define CAT "/CONFIG/VTSS"

/* ---- the page ------------------------------------------------------------ */
#define STATUS_ID "vtss_status"

static const char X_PAGE[] =
	"<setting_list id=\"vtss_settings\" title=\"vtss_t_title\" icon=\"tex_spanner\">"
	"<toggle_switch id=\"vtss_enable\" title=\"vtss_t_enable\" icon=\"tex_spanner\" "
	"hint=\"vtss_h_enable\" key=\"" CAT "/enable\"/>"
	"<button id=\"" STATUS_ID "\" title=\"vtss_t_status\" icon=\"tex_spanner\" "
	"hint=\"vtss_h_status\" success_dialog_message=\"vtss_d_status\"/>"
	"<toggle_switch id=\"vtss_log\" title=\"vtss_t_log\" icon=\"tex_spanner\" "
	"hint=\"vtss_h_log\" key=\"" CAT "/log\"/>"
	"<toggle_switch id=\"vtss_cpu\" title=\"vtss_t_cpu\" icon=\"tex_spanner\" "
	"hint=\"vtss_h_cpu\" key=\"" CAT "/cpu\"/>"
	"</setting_list>";
#define X_LEN ((int)sizeof(X_PAGE) - 1)

/* Leave a Devices page bigger than this alone (stock is a few KB, YAMT's ~50 KB). */
#define PAGE_ORIG_MAX 65536

static SceUID g_page_uid = -1, g_page_old = -1;
static char  *g_page;
static int    g_page_cap;

/* The spliced page lives in its own memblock, allocated when the page is
 * opened; the previous one is kept until the next open (paf may still read it). */
static char *page_buf(int need)
{
	SceUID uid;
	void *base = 0;
	int cap;

	if (g_page && need <= g_page_cap)
		return g_page;
	cap = (need + 0xFFF) & ~0xFFF;
	uid = sceKernelAllocMemBlock("vtss_page", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, cap, 0);
	if (uid < 0 || sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
		if (uid >= 0)
			sceKernelFreeMemBlock(uid);
		return 0;
	}
	if (g_page_old >= 0)
		sceKernelFreeMemBlock(g_page_old);
	g_page_old = g_page_uid;
	g_page_uid = uid;
	g_page = base;
	g_page_cap = cap;
	return g_page;
}

static void page_free(void)
{
	if (g_page_uid >= 0)
		sceKernelFreeMemBlock(g_page_uid);
	if (g_page_old >= 0)
		sceKernelFreeMemBlock(g_page_old);
	g_page_uid = g_page_old = -1;
	g_page = 0;
	g_page_cap = 0;
}

/* ---- text (UTF-16 for paf) ----------------------------------------------- */
typedef unsigned short u16;
#define TXT_UNITS 384
static u16 g_txt[4][TXT_UNITS];
static int g_txt_next;

static const u16 *text(const char *a)
{
	u16 *o = g_txt[g_txt_next];
	int n = 0;

	g_txt_next = (g_txt_next + 1) % 4;
	for (; a && *a && n < TXT_UNITS - 1; a++)
		o[n++] = (u16)(unsigned char)*a;    /* ASCII only (English page) */
	o[n] = 0;
	return o;
}

static const u16 *status_title(void)
{
	return text(g_set.enable ? "Status: Loaded" : "Status: Not loaded");
}

static const u16 *status_hint(void)
{
	return text(g_set.enable
		? "VTSS loads in games and apps (overlay; START+LEFT: mode, START+RIGHT: reset min/max)."
		: "VTSS Enable is off: VTSS is not loaded in games and apps.");
}

static const u16 *status_dialog(void)
{
	static const char *const loaded[4] = {
		"Status: Loaded\nLog to file: Off\nCPU stats: Off\n\n"
		"Changes apply to games started after them.",
		"Status: Loaded\nLog to file: On\nCPU stats: Off\n\nLogs: ux0:data/VTSS/<TITLEID>.log\n\n"
		"Changes apply to games started after them.",
		"Status: Loaded\nLog to file: Off\nCPU stats: On\n\n"
		"Changes apply to games started after them.",
		"Status: Loaded\nLog to file: On\nCPU stats: On\n\nLogs: ux0:data/VTSS/<TITLEID>.log\n\n"
		"Changes apply to games started after them.",
	};
	if (g_set.enable)
		return text(loaded[(g_set.log ? 1 : 0) | (g_set.cpu ? 2 : 0)]);
	return text("Status: Not loaded\n\nVTSS is unloaded right at start in every app "
	            "(almost no memory or CPU).\n\nChanges apply to games started after them.");
}

static const u16 *page_text(const char *id)
{
	if (!s_pfx(id, "vtss_"))
		return 0;
	if (s_eq(id, "vtss_t_title"))  return text("VTSS");
	if (s_eq(id, "vtss_t_enable")) return text("VTSS Enable");
	if (s_eq(id, "vtss_h_enable")) return text("Off: VTSS is not loaded anywhere (almost no memory or CPU used).");
	if (s_eq(id, "vtss_t_status")) return status_title();
	if (s_eq(id, "vtss_h_status")) return status_hint();
	if (s_eq(id, "vtss_d_status")) return status_dialog();
	if (s_eq(id, "vtss_t_log"))    return text("Log to file");
	if (s_eq(id, "vtss_h_log"))    return text("FPS and frametime of each game to ux0:data/VTSS/<TITLEID>.log");
	if (s_eq(id, "vtss_t_cpu"))    return text("CPU stats");
	if (s_eq(id, "vtss_h_cpu"))    return text("Load of each CPU core and the CPU clock, once a second, on the overlay and in the log.");
	return 0;
}

/* ---- hooks --------------------------------------------------------------- */
static SceUID g_h_load = -1, g_h_xml = -1, g_h_get = -1, g_h_set = -1, g_h_info = -1,
              g_h_text = -1, g_h_init = -1;
static tai_hook_ref_t r_load, r_xml, r_get, r_set, r_info, r_text, r_init;
static int g_installed;

static int is_ours(const char *cat) { return cat && s_eq(cat, CAT); }

static uint8_t *key_ptr(const char *name)
{
	if (!name) return 0;
	if (s_eq(name, "enable")) return &g_set.enable;
	if (s_eq(name, "log"))    return &g_set.log;
	if (s_eq(name, "cpu"))    return &g_set.cpu;
	return 0;
}

static const char CLOSE_LIST[] = "</setting_list>";

/* The </setting_list> closing the list that opens with `open` (nesting counted). */
static const char *list_close(const char *s, int size, const char *open, int open_n)
{
	static const char OPEN[] = "<setting_list";
	const char *end = s + size;
	const char *p = mem_find(s, size, open, open_n);
	int depth = 0;

	if (!p)
		return 0;
	while (p < end) {
		const char *o = mem_find(p, (int)(end - p), OPEN, (int)sizeof(OPEN) - 1);
		const char *cl = mem_find(p, (int)(end - p), CLOSE_LIST, (int)sizeof(CLOSE_LIST) - 1);
		if (!cl)
			return 0;
		if (o && o < cl) {
			depth++;
			p = o + sizeof(OPEN) - 1;
		} else {
			if (--depth == 0)
				return cl;
			p = cl + sizeof(CLOSE_LIST) - 1;
		}
	}
	return 0;
}

static int xml_hook(void *out, void *buf, int size, void *a4)
{
	static const char PLUG[] = "plugin=\"peripherals_settings_plugin\"";
	static const char MINE[] = "<setting_list id=\"vtss_settings\"";
	static const char DEV[]  = "<setting_list id=\"peripherals_settings\"";
	const char *s = (const char *)buf;

	if (s && size > 0 && size <= PAGE_ORIG_MAX &&
	    mem_find(s, size, PLUG, (int)sizeof(PLUG) - 1) &&
	    !mem_find(s, size, MINE, (int)sizeof(MINE) - 1)) {
		const char *ins = list_close(s, size, DEV, (int)sizeof(DEV) - 1);
		char *pb = ins ? page_buf(size + X_LEN) : 0;

		if (pb) {
			int head = (int)(ins - s);
			s_cpy(pb, s, head);
			s_cpy(pb + head, X_PAGE, X_LEN);
			s_cpy(pb + head + X_LEN, ins, size - head);
			vtss_settings_load(&g_set);   /* fresh from the file each time the page opens */
			buf = pb;
			size += X_LEN;
		}
	}
	return TAI_CONTINUE(int, r_xml, out, buf, size, a4);
}

static int get_hook(const char *cat, const char *name, int *val)
{
	if (is_ours(cat) && val) {
		uint8_t *p = key_ptr(name);
		*val = p ? *p : 0;
		return 0;
	}
	return TAI_CONTINUE(int, r_get, cat, name, val);
}

static int set_hook(const char *cat, const char *name, int v)
{
	if (is_ours(cat)) {
		uint8_t *p = key_ptr(name);
		if (p && (v == 0 || v == 1) && *p != (uint8_t)v) {
			*p = (uint8_t)v;
			vtss_settings_save(&g_set);   /* a tap in Settings: one small write */
		}
		return 0;
	}
	return TAI_CONTINUE(int, r_set, cat, name, v);
}

static int info_hook(const char *cat, void *info, int unk)
{
	if (is_ours(cat)) {
		if (info)
			*(int *)((char *)info + 8) = 0x00040000;   /* int */
		return 0;
	}
	return TAI_CONTINUE(int, r_info, cat, info, unk);
}

static void *text_hook(void *arg, char **msg)
{
	if (msg && *msg) {
		const u16 *t = page_text(*msg);
		if (t)
			return (void *)t;
	}
	return TAI_CONTINUE(void *, r_text, arg, msg);
}

typedef int (*button_fn)(const char *id, int a2, void *a3);
static button_fn g_next_button;

static int button_hook(const char *id, int a2, void *a3)
{
	if (id && s_eq(id, STATUS_ID))
		return 0;                         /* 0: shows the status dialog          */
	return g_next_button ? g_next_button(id, a2, a3) : 0;
}

static int init_hook(void *a1, int a2, uint32_t *funcs)
{
	static const char PLUG[] = "peripherals_settings_plugin";
	int ret = TAI_CONTINUE(int, r_init, a1, a2, funcs);
	const char *plugin = a1 ? (const char *)((uint32_t *)a1)[1] : 0;

	if (plugin && funcs && s_eq(plugin, PLUG) && funcs[6] != (uint32_t)button_hook) {
		g_next_button = (button_fn)funcs[6];
		funcs[6] = (uint32_t)button_hook;
	}
	return ret;
}

/* NID 0x2DCC4AFA as SceSettings imports it takes six args (as in HENkaku). */
static SceUID load_hook(const char *path, SceSize args, void *argp,
                        int flags, void *option, int *status)
{
	SceUID ret = TAI_CONTINUE(SceUID, r_load, path, args, argp, flags, option, status);

	if (!g_installed && path && mem_find(path, s_len(path), "system_settings_core", 20)) {
		g_installed = 1;
		g_h_init = taiHookFunctionImport(&r_init, "SceSettings", 0x4D9A9DD0, 0xF5354FEF, init_hook);
		g_h_xml  = taiHookFunctionImport(&r_xml,  "SceSettings", 0x3D643CE8, 0x19FE55A8, xml_hook);
		g_h_get  = taiHookFunctionImport(&r_get,  "SceSystemSettingsCore", 0xC436F916, 0x16DDF3DC, get_hook);
		g_h_set  = taiHookFunctionImport(&r_set,  "SceSystemSettingsCore", 0xC436F916, 0xD72EA399, set_hook);
		g_h_info = taiHookFunctionImport(&r_info, "SceSystemSettingsCore", 0xC436F916, 0x58421DD1, info_hook);
		g_h_text = taiHookFunctionImport(&r_text, "SceSystemSettingsCore", 0x4D9A9DD0, 0x19CEFDA7, text_hook);
	}
	return ret;
}

int vtss_in_settings(void)
{
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	return taiGetModuleInfo("SceSettings", &ti) >= 0;
}

void vtss_settings_page_install(void)
{
	vtss_settings_load(&g_set);
	g_h_load = taiHookFunctionImport(&r_load, "SceSettings", 0xCAE9ACE6, 0x2DCC4AFA, load_hook);
}

void vtss_settings_page_release(void)
{
	if (g_h_text >= 0) { taiHookRelease(g_h_text, r_text); g_h_text = -1; }
	if (g_h_info >= 0) { taiHookRelease(g_h_info, r_info); g_h_info = -1; }
	if (g_h_set  >= 0) { taiHookRelease(g_h_set,  r_set);  g_h_set  = -1; }
	if (g_h_get  >= 0) { taiHookRelease(g_h_get,  r_get);  g_h_get  = -1; }
	if (g_h_xml  >= 0) { taiHookRelease(g_h_xml,  r_xml);  g_h_xml  = -1; }
	if (g_h_init >= 0) { taiHookRelease(g_h_init, r_init); g_h_init = -1; }
	if (g_h_load >= 0) { taiHookRelease(g_h_load, r_load); g_h_load = -1; }
	page_free();
}
