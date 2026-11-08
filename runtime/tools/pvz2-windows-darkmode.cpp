/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Windows host's dark mode, through win32-darkmodelib.
 *
 * The host follows Windows' own light or dark preference; there is no new
 * setting. The library is built with _DARKMODELIB_NO_INI_CONFIG, so its
 * initDarkMode() reads the preference and applies dark, or its classic mode
 * (which is Windows' light appearance) when Windows is light. In classic mode
 * every library subclass passes the message on, so the host's own light
 * painting is unchanged.
 */
#include <windows.h>

#include "Darkmodelib.h"
#include "pvz2-windows-darkmode.h"

extern "C" {

void tl_win_darkmode_init(void)
{
    dmlib::initDarkMode();
}

int tl_win_darkmode_active(void)
{
    return dmlib::isEnabled() ? 1 : 0;
}

void tl_win_darkmode_window(HWND hwnd)
{
    if (!hwnd) return;
    dmlib::setDarkWndNotifySafe(hwnd);
    /* The window's own client area, below the controls. */
    dmlib::setWindowEraseBgSubclass(hwnd);
}

void tl_win_darkmode_page(HWND hwnd)
{
    if (!hwnd) return;
    /* WM_CTLCOLORSTATIC goes to the control's parent, which is the page, not
     * the Settings window, so the page gets the colour subclass itself. */
    dmlib::setWindowCtlColorSubclass(hwnd);
    dmlib::setWindowEraseBgSubclass(hwnd);
}

void tl_win_darkmode_title(HWND hwnd)
{
    if (!hwnd) return;
    dmlib::setDarkTitleBarEx(hwnd, true);
}

int tl_win_darkmode_system_changed(LPARAM lp)
{
    return dmlib::handleSettingChange(lp) ? 1 : 0;
}

void tl_win_darkmode_redraw(HWND hwnd)
{
    if (!hwnd) return;
    dmlib::setDarkTitleBarEx(hwnd, true);
    dmlib::setChildCtrlsTheme(hwnd);
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
}

} /* extern "C" */
