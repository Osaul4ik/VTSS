/*
 * vtss.h - VTSS shared definitions.
 *
 * One module, VTSS.suprx, listed under *ALL in config.txt:
 *   - in the Settings app (NPXS10015) it only adds the VTSS page under
 *     Settings > Devices (settings_page.c);
 *   - in any other app it reads settings.bin first: VTSS Enable off -> it
 *     returns SCE_KERNEL_START_NO_RESIDENT and is unloaded at once (one small
 *     file read, no hook, no thread, no memory kept);
 *     on -> the overlay, and the worker thread only when Log to file or
 *     CPU stats is on.
 */
#ifndef _VTSS_H_
#define _VTSS_H_

#include <stdint.h>

#define VTSS_DIR            "ux0:data/VTSS"
#define VTSS_SETTINGS_PATH  VTSS_DIR "/settings.bin"

#define VTSS_MAGIC          0x53535456u   /* "VTSS" */
#define VTSS_VERSION        1

typedef struct {
	uint32_t magic;
	uint8_t  version;
	uint8_t  enable;                      /* 0: not loaded in apps              */
	uint8_t  log;                         /* 1: log to file                     */
	uint8_t  cpu;                         /* 1: CPU stats (overlay + log)       */
} VtssSettings;

/* Defaults: enabled, no log file, no CPU stats. */
#define VTSS_DEF_ENABLE 1
#define VTSS_DEF_LOG    0
#define VTSS_DEF_CPU    0

/* Reads settings.bin; a missing or non-matching file gives the defaults
 * (no compatibility layer). */
void vtss_settings_load(VtssSettings *s);
/* Writes settings.bin (Settings app only, on a change). 0 or <0. */
int  vtss_settings_save(const VtssSettings *s);

#endif
