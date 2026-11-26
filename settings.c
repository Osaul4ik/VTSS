/*
 * settings.c - settings.bin load/save. See vtss.h.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "vtss.h"

static void defaults(VtssSettings *s)
{
	s->magic = VTSS_MAGIC;
	s->version = VTSS_VERSION;
	s->enable = VTSS_DEF_ENABLE;
	s->log = VTSS_DEF_LOG;
	s->reserved = 0;
}

void vtss_settings_load(VtssSettings *s)
{
	VtssSettings f;
	SceUID fd = sceIoOpen(VTSS_SETTINGS_PATH, SCE_O_RDONLY, 0);

	defaults(s);
	if (fd < 0)
		return;
	int n = sceIoRead(fd, &f, sizeof(f));
	sceIoClose(fd);
	if (n != (int)sizeof(f) || f.magic != VTSS_MAGIC || f.version != VTSS_VERSION ||
	    f.enable > 1 || f.log > 1)
		return;                           /* not ours / old: defaults            */
	*s = f;
}

int vtss_settings_save(const VtssSettings *s)
{
	VtssSettings f = *s;

	f.magic = VTSS_MAGIC;
	f.version = VTSS_VERSION;
	f.reserved = 0;
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(VTSS_DIR, 0777);
	SceUID fd = sceIoOpen(VTSS_SETTINGS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;
	int n = sceIoWrite(fd, &f, sizeof(f));
	sceIoClose(fd);
	return n == (int)sizeof(f) ? 0 : -1;
}
