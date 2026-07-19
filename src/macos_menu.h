#ifndef PREVIEW_MACOS_MENU_H
#define PREVIEW_MACOS_MENU_H

/* Install a standard macOS menu bar (App + Edit + View menus).
 *
 * This is what makes Cmd+C work: on macOS the standard editing shortcuts
 * are dispatched as menu key equivalents, so an app with no main menu
 * never delivers Cmd+C/Cmd+A to the web view and copying silently fails.
 * The webview library does not create a menu, so we install one.
 *
 * Safe to call more than once; does nothing if a menu already exists.
 * Requires NSApplication to exist, so call it after the window is created.
 * Returns 1 if a menu is in place, 0 otherwise. */
int preview_install_menu(void);

#endif
