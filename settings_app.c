/*
 * settings_app.c - VTSS_settings.suprx, the Settings-app module.
 *
 * Listed under *NPXS10015 in config.txt. It only adds the VTSS page under
 * Settings > Devices (settings_page.c); the overlay module (VTSS.suprx)
 * carries none of this code. Loaded anywhere else by mistake, it unloads
 * itself at once.
 */
#include <psp2/kernel/modulemgr.h>

#include "vtss.h"
#include "settings_page.h"

int module_start(SceSize argc, const void *args);
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	if (!vtss_in_settings())
		return SCE_KERNEL_START_NO_RESIDENT;
	vtss_settings_page_install();
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	vtss_settings_page_release();
	return SCE_KERNEL_STOP_SUCCESS;
}
