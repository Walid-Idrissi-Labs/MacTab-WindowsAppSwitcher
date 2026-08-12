#pragma once

// Icons
#define IDI_APPICON             101

// Tray context menu command ids
#define IDM_TRAY_ABOUT          200
#define IDM_TRAY_OPEN_LOG       201
#define IDM_TRAY_RELOAD_HOOK    202
#define IDM_TRAY_DUMP_LIST      203
#define IDM_TRAY_AUTOSTART      204
#define IDM_TRAY_QUIT           205
#define IDM_TRAY_RELOAD_GLASS   206
#define IDM_TRAY_DUMP_DESKTOPS  207
#define IDM_TRAY_MISSION        208
#define IDM_TRAY_OPEN_SETTINGS  209
#define IDM_TRAY_RESET_SETTINGS 213
#define IDM_TRAY_GLASS          214

// Settings > Panel appears on. Contiguous, and CheckMenuRadioItem is given the
// first and last of them as its range, so keep them adjacent and in this order.
#define IDM_TRAY_DISPLAY_ACTIVE 210
#define IDM_TRAY_DISPLAY_MOUSE  211
#define IDM_TRAY_DISPLAY_MAIN   212

#define IDM_TRAY_UNINSTALL      216

// Settings > Appearance. Same rule: contiguous and in this order.
#define IDM_TRAY_THEME_AUTO     220
#define IDM_TRAY_THEME_LIGHT    221
#define IDM_TRAY_THEME_DARK     222
