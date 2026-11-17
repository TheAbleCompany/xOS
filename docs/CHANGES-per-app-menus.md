# ArtyomX64 -- per-app global menus + full multi-window support

Date: 2026-09-05
Follows: CHANGES-macos-model.md (global menu bar + "1 process -- N windows"
kernel model).  This round makes the menu bar belong to the APPLICATIONS,
not to the shell, and extends the macOS model to every GUI app.

## Problems fixed (user report)

1. "Not all applications support this, except Notes" -- only Notes handled
   the new process model (File > New Window, Quit).  Every other GUI app
   ignored the menu entirely.
2. "The X symbol is duplicated in the menu" -- the menubar reflow loop drew
   the logo menu's label ("X") a second time next to the real logo glyph.
3. "The menu must control each application, not just the kernel" -- the
   File/Edit/View menus were hardcoded in the shell, so menu items could
   never drive app features (Save, Clear, Refresh, ...).  In real macOS the
   APPLICATION owns its menus; the system only renders them and forwards
   clicks.

## New kernel facility: AX_SYS_MENU_SET (174) + AX_EV_MENU (7)

    lib/axipc.h

An app describes its whole menu bar once and registers it:

    typedef struct {
        const char *title;        /* "File", "Shell", "Calc", ...   */
        const char **items;       /* item labels                    */
        uint32_t    item_count;
    } ax_menu_def_t;

    typedef struct {
        const char          *app_name;   /* bold menubar label */
        const ax_menu_def_t *menus;
        uint32_t             menu_count;
    } ax_menubar_def_t;

    ax_menu_set(&my_menubar);      /* syscall 174 */

* The kernel copies EVERY string into kernel memory during the syscall
  (the syscall runs in the caller's address space, the same way
  ax_surface() copies its title), so the app may reuse its buffers.
* Re-registering replaces the bar; the registration dies with the process
  (menu_drop() from reap_closed_windows(), plus an alive-check in
  menu_lookup() every frame -- a crashed app cannot leave a zombie bar).
* Limits: 4 menus per app, 6 items per menu, title 15 chars, item 23
  chars (AX_* limits in axipc.h).
* While a window of the process is focused, the shell renders
  [logo] <AppName> <app menus...>; a click on an item is delivered to
  the FOCUSED window as:

      AX_EV_MENU,  ev.key = menu index,  ev.w = item index   (both 0-based)

* The [logo] system menu and the bold <AppName> menu (About / Hide /
  Quit) stay shell-owned -- like macOS's Apple menu and app menu.
* The bold label now comes from the registration (app_name), not the
  window title: temu shows "Terminal" even though its window says XSH.
* Apps that never register still get the old shell fallback
  (File/Edit/View), so the bar is never empty.

### Kernel files

* kernel/axshell_menu.c  (NEW) -- registration table (8 slots), syscall
  handler, menu_lookup()/menu_drop().
* kernel/axshell_internal.h -- ax_app_menu_t/ax_app_menubar_t (item_text
  copies + items[] pointer list, so the renderer's const char** walk is
  type-safe), menus[8].
* kernel/axshell_ui.c -- app_menu_refresh() rebuilds menus[2..] from the
  active app's registration each frame; draw_menubar() reflow now starts
  AFTER the logo and never prints the logo label (the duplicate-X fix);
  menu_click() forwards app-owned items as AX_EV_MENU and keeps the old
  File/Edit/View behaviour only for unregistered apps.
* kernel/axshell_input.c -- menubar hit-test starts at menu 1 (logo has
  its own fixed hit-box); diagnostic MENUBAR_CLICK/MENU_CLICK serial logs.
* kernel/axshell_surface.c -- reap_closed_windows() drops the menubar of
  dead owners.
* lib/syscalls.c -- case 174.
* kernel/Makefile, Makefile -- axmenu.o.

## Apps: every GUI app now owns its menu bar

All apps register their bar right after the first ax_surface() and handle
AX_EV_MENU / AX_EV_QUIT in their event loops.

* notes   -- File: New Window / Close Window / Save / Open...
            (menu actions reuse the Ctrl+S / Ctrl+O commands; New Window
            also still answers AX_EV_NEWWIN).  Multi-window, up to 4.
* calc    -- REWRITTEN for the macOS model: up to 4 independent windows,
            per-window accumulator/display.  Menus: File (New Window /
            Close Window) + Calc (Clear / Negate).  Also handles
            keyboard 0-9 + - * / = c.  File > New Window arrives as both
            AX_EV_MENU and AX_EV_NEWWIN.
* temu    -- Shell: Clear Screen / Close Window.  Registered app name
            "Terminal".
* xsh     -- Shell: Clear Screen / Close Window.
* files   -- File: Refresh / Close Window (Refresh = reload_directory(),
            same as the sidebar button).
* prefs   -- File: Close Window.
* raycaster -- Game: New Game (player back to 1.5,1.5,ang 0, fov 45) /
            Close Window.  (raycaster.c was CRLF -- normalised to LF.)

Closing the LAST window (menu, red button, or Quit) exits the process;
closing one window keeps it running -- the macOS model everywhere.

## Build + test artefacts

* bin/main.elf   -- kernel with the menu facility (164 KB)
* bin/initrd.img -- repacked with the 7 new app binaries
* bin/main.iso   -- Limine-bootable CD
* apps/*.bin     -- notes, calc, files, temu, xsh, prefs, raycaster

Verified in QEMU (HMP-driven mouse, screenshots at every step):

* menubar shows exactly one logo glyph (duplicate X gone);
* [X] Terminal Shell > Clear Screen / Close Window -- both delivered
  (PUSH_EVENT: type=7 to XSH), Close Window removed the window;
* [X] Calculator File Calc > Clear/Negate, File > New Window opened a
  SECOND window of the same PID; File > Close Window closed only the
  focused one (process stayed);
* [X] Notes File > New Window -> two windows; bold menu > Quit Notes
  closed both and exited the process;
* [X] Files File > Refresh re-read the initrd listing;
* [X] System Settings File > Close Window;
* no CPU exceptions across all runs.

## Known limits

* Edit fallback menu items (Cut/Copy/Paste) are placeholders -- there is
  no clipboard in the OS yet.
* raycaster's Game menu is registered but was only compile-verified
  (not in the dock; launch it from Files/terminal).
* Menu labels are ASCII only (8x8 font).
