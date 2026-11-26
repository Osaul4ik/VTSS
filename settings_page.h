/*
 * settings_page.h - the VTSS page in the Settings app. See settings_page.c.
 */
#ifndef _SETTINGS_PAGE_H_
#define _SETTINGS_PAGE_H_

int  vtss_in_settings(void);              /* 1: this process is the Settings app */
void vtss_settings_page_install(void);
void vtss_settings_page_release(void);

#endif
