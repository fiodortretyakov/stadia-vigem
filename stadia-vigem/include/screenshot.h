/*
 * screenshot.h -- Saves a screenshot without key presses, for the Capture button.
 */

#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Saves a PNG of the monitor that shows the foreground window to the user's Screenshots folder.
    // Works while a game has focus (games often block the Windows key, so Win+PrintScreen does nothing there).
    BOOL take_screenshot(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSHOT_H */
