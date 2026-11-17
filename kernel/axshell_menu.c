/*
 * ============================================================
 * axshell_menu.c
 * ============================================================
 *
 * Per-app menu bars (macOS model).
 *
 * In real macOS the APPLICATION owns its menu bar: each app
 * provides its own File / Edit / <specialty> menus, the system
 * only renders them next to the app name and forwards clicks.
 * This module is the ArtyomX64 counterpart:
 *
 *   * An app registers its whole bar once through the
 *     AX_SYS_MENU_SET syscall (ax_menu_set() in libax.h).
 *     Every string is copied into kernel memory during the
 *     call, so the app's buffers may go away afterwards.
 *
 *   * While a window of that process is focused, the menubar
 *     renderer (axshell_ui.c) shows exactly these menus via
 *     menu_lookup(), and menu_click() forwards a click on an
 *     item to the focused window as AX_EV_MENU
 *     (ev.key = menu index, ev.w = item index).
 *
 *   * The registration dies with the process (menu_drop is
 *     called from reap_closed_windows(); menu_lookup() also
 *     re-checks task_is_alive() every frame, so a crashed app
 *     can never keep a zombie menu bar alive).
 *
 * The [logo] and <AppName> menus (About / Hide / Quit) are NOT
 * app-defined -- the shell owns them, like macOS owns the
 * Apple menu and the bold app menu.
 */

#include "axshell_internal.h"

#include "../fs/task.h"

/*
 * ============================================================
 * Module state
 * ============================================================
 *
 * ax_app_menu_t / ax_app_menubar_t come from axshell_internal.h
 * (layout synced with the AX_* limits in axipc.h).
 */

static ax_app_menubar_t app_menubars[8];

/*
 * ============================================================
 * Bounded string copy from the caller's address space
 * ============================================================
 *
 * The syscall runs with the calling task's page tables active,
 * so user pointers can be dereferenced directly -- the same
 * way ax_syscall_surface() copies the window title.
 */

static int copy_user_str(char *dst, const char *usrc, int cap)
{
    if (!dst || !usrc)
        return -1;

    int n = 0;
    while (n < cap - 1) {
        char c = usrc[n];
        if (!c)
            break;
        dst[n] = c;
        n++;
    }
    dst[n] = 0;
    return (n > 0 || usrc[0] == 0) ? 0 : -1;
}

/*
 * ============================================================
 * Registration (AX_SYS_MENU_SET)
 * ============================================================
 */

int ax_syscall_menu_set(const ax_menubar_def_t *udef)
{
    if (!udef)
        return -1;

    int pid = runningTask ? runningTask->pid : -1;
    if (pid < 0)
        return -1;

    /* Read the top-level description (caller's address space). */
    const char          *app_name   = udef->app_name;
    const ax_menu_def_t *umenus    = udef->menus;
    uint32_t             menu_count = udef->menu_count;

    if (menu_count > AX_APP_MENUS_MAX)
        menu_count = AX_APP_MENUS_MAX;

    if (!app_name || !umenus || menu_count == 0)
        return -1;

    /*
     * Find the slot: an existing registration for this pid is
     * replaced in place, otherwise take the first free slot.
     */
    ax_app_menubar_t *mb = 0;
    for (int i = 0; i < 8 && !mb; i++)
        if (app_menubars[i].pid == pid)
            mb = &app_menubars[i];

    for (int i = 0; i < 8 && !mb; i++)
        if (app_menubars[i].pid == 0)
            mb = &app_menubars[i];

    if (!mb) {
        serial_puts_ax("MENU_SET: table full for pid ");
        debug_putnum((uint64_t)pid);
        serial_putc_ax('\n');
        return -1;
    }

    mb->pid = pid;

    if (copy_user_str(mb->app_name, app_name,
                      (int)sizeof(mb->app_name)) != 0) {
        mb->pid = 0;
        return -1;
    }

    /* Copy every menu and every item string. */
    for (uint32_t m = 0; m < menu_count; m++) {
        const ax_menu_def_t *um = &umenus[m];

        ax_app_menu_t *dst = &mb->menus[m];

        const char *utitle = um->title;
        uint32_t ucount = um->item_count;

        if (copy_user_str(dst->title, utitle,
                          (int)sizeof(dst->title)) != 0) {
            dst->title[0] = '?';
            dst->title[1] = 0;
        }

        if (ucount > AX_MENU_ITEMS_MAX)
            ucount = AX_MENU_ITEMS_MAX;

        dst->item_count = (int)ucount;

        for (uint32_t it = 0; it < ucount; it++) {
            const char *ulabel = um->items[it];

            if (copy_user_str(dst->item_text[it], ulabel,
                              (int)sizeof(dst->item_text[it])) != 0) {
                dst->item_text[it][0] = '?';
                dst->item_text[it][1] = 0;
            }

            /* Point the public list at the private copy. */
            dst->items[it] = dst->item_text[it];
        }
    }

    mb->menu_count = (int)menu_count;

    serial_puts_ax("MENU_SET: pid=");
    debug_putnum((uint64_t)pid);
    serial_puts_ax(" app=");
    serial_puts_ax(mb->app_name);
    serial_puts_ax(" menus=");
    debug_putnum((uint64_t)mb->menu_count);
    serial_putc_ax('\n');

    return 0;
}

/*
 * ============================================================
 * Lookup + cleanup
 * ============================================================
 */

/*
 * Menubar of a live process, or 0.  Registrations of dead
 * tasks are dropped here as well, so the menu bar can never
 * outlive its app.
 */
ax_app_menubar_t *menu_lookup(int pid)
{
    if (pid < 0)
        return 0;

    for (int i = 0; i < 8; i++) {
        ax_app_menubar_t *mb = &app_menubars[i];
        if (mb->pid == pid) {
            if (!task_is_alive(pid)) {
                mb->pid = 0;
                return 0;
            }
            return mb;
        }
    }
    return 0;
}

/* Explicit drop (used by reap_closed_windows on dead owners). */
void menu_drop(int pid)
{
    if (pid < 0)
        return;

    for (int i = 0; i < 8; i++) {
        if (app_menubars[i].pid == pid) {
            app_menubars[i].pid = 0;
            serial_puts_ax("MENU_DROP: pid=");
            debug_putnum((uint64_t)pid);
            serial_putc_ax('\n');
        }
    }
}
