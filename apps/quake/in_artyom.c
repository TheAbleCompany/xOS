/*
 * in_artyom.c -- ввод Quake для ArtyomX64.
 *
 * Клавиатура: wserver присылает AX_EV_RAWKEY на каждый make/break
 * (сканкод set 1 + признак extended), здесь он превращается в
 * knum_t Quake и передаётся в Key_Event().
 *
 * Мышь: AX_EV_MOUSE содержит АБСОЛЮТНЫЕ координаты внутри клиентской
 * области окна и маску кнопок (1=левая, 2=правая).  Захвата указателя
 * в WindowServer'е нет, поэтому накапливаем разности координат --
 * этого достаточно для осмотра, пока курсор в пределах окна.
 */

#include "client.h"
#include "common.h"
#include "console.h"
#include "input.h"
#include "keys.h"
#include "quakedef.h"
#include "sys.h"
#include "vid.h"

#include "artyom.h"
#include "axglue.h"

uint32_t *ax_win;
int ax_win_w;
int ax_win_h;

cvar_t _windowed_mouse = { "_windowed_mouse", "0", CVAR_CONFIG };
static cvar_t m_filter = { "m_filter", "0" };

static qboolean mouse_available;
static qboolean have_focus = true;

static int mouse_x, mouse_y;
static int old_mouse_x, old_mouse_y;
static int last_mx, last_my;
static qboolean have_last_mouse;
static unsigned last_buttons;

/* ------------------------------------------------------------------ */
/* Сканкод set 1 -> knum_t Quake                                      */
/* ------------------------------------------------------------------ */

static knum_t map_scancode(unsigned sc, int ext)
{
    static const char digits[]   = "1234567890";
    static const char row_qwer[] = "qwertyuiop";
    static const char row_asdf[] = "asdfghjkl";
    static const char row_zxcv[] = "zxcvbnm";

    if (ext) {
        switch (sc) {
        case 0x1c: return K_KP_ENTER;
        case 0x1d: return K_RCTRL;
        case 0x35: return K_KP_DIVIDE;
        case 0x38: return K_RALT;
        case 0x47: return K_HOME;
        case 0x48: return K_UPARROW;
        case 0x49: return K_PGUP;
        case 0x4b: return K_LEFTARROW;
        case 0x4d: return K_RIGHTARROW;
        case 0x4f: return K_END;
        case 0x50: return K_DOWNARROW;
        case 0x51: return K_PGDN;
        case 0x52: return K_INS;
        case 0x53: return K_DEL;
        case 0x5b: return K_LSUPER;
        case 0x5c: return K_RSUPER;
        case 0x5d: return K_MENU;
        default:   return K_UNKNOWN;
        }
    }

    if (sc >= 0x02 && sc <= 0x0b)
        return (knum_t)(unsigned char)digits[sc - 0x02];
    if (sc >= 0x10 && sc <= 0x19)
        return (knum_t)(unsigned char)row_qwer[sc - 0x10];
    if (sc >= 0x1e && sc <= 0x26)
        return (knum_t)(unsigned char)row_asdf[sc - 0x1e];
    if (sc >= 0x2c && sc <= 0x32)
        return (knum_t)(unsigned char)row_zxcv[sc - 0x2c];

    switch (sc) {
    case 0x01: return K_ESCAPE;
    case 0x0c: return K_MINUS;
    case 0x0d: return K_EQUALS;
    case 0x0e: return K_BACKSPACE;
    case 0x0f: return K_TAB;
    case 0x1a: return K_LEFTBRACKET;
    case 0x1b: return K_RIGHTBRACKET;
    case 0x1c: return K_ENTER;
    case 0x1d: return K_LCTRL;
    case 0x27: return K_SEMICOLON;
    case 0x28: return K_QUOTE;
    case 0x29: return K_BACKQUOTE;
    case 0x2a: return K_LSHIFT;
    case 0x2b: return K_BACKSLASH;
    case 0x33: return K_COMMA;
    case 0x34: return K_PERIOD;
    case 0x35: return K_SLASH;
    case 0x36: return K_RSHIFT;
    case 0x37: return K_KP_MULTIPLY;
    case 0x38: return K_LALT;
    case 0x39: return K_SPACE;
    case 0x3a: return K_CAPSLOCK;
    case 0x45: return K_NUMLOCK;
    case 0x46: return K_SCROLLOCK;

    /* F1..F10 */
    case 0x3b: return K_F1;
    case 0x3c: return K_F2;
    case 0x3d: return K_F3;
    case 0x3e: return K_F4;
    case 0x3f: return K_F5;
    case 0x40: return K_F6;
    case 0x41: return K_F7;
    case 0x42: return K_F8;
    case 0x43: return K_F9;
    case 0x44: return K_F10;

    case 0x57: return K_F11;
    case 0x58: return K_F12;

    /* Keypad (NumLock on) */
    case 0x47: return K_KP7;
    case 0x48: return K_KP8;
    case 0x49: return K_KP9;
    case 0x4a: return K_KP_MINUS;
    case 0x4b: return K_KP4;
    case 0x4c: return K_KP5;
    case 0x4d: return K_KP6;
    case 0x4e: return K_KP_PLUS;
    case 0x4f: return K_KP1;
    case 0x50: return K_KP2;
    case 0x51: return K_KP3;
    case 0x52: return K_KP0;
    case 0x53: return K_KP_PERIOD;

    default: return K_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ */
/* Насос событий WindowServer'а                                        */
/* ------------------------------------------------------------------ */

void AX_PumpEvents(void)
{
    if (!ax_win)
        return;

    {
        static int dbg_pump;
        if (++dbg_pump == 100 || dbg_pump == 600) {
            AX_Info("QUAKE: pump n=%i cli=%i srv=%i conn=%i ring=%i\n",
                    dbg_pump, axg_cli_port, axg_srv_port,
                    axg_connected(), axg_ring_n);
        }
    }

    for (int i = 0; i < 128; i++) {
        ax_event ev;
        int r = axg_poll(ax_win, &ev);

        if (r <= 0)
            break;

        {
            static int dbg_ev;
            if (dbg_ev < 40) {
                AX_Info("QUAKE: ev type=%i key=%i w=%i\n",
                        (int)ev.type, (int)ev.key, (int)ev.w);
                dbg_ev++;
            }
        }

        switch (ev.type) {
        case AX_EV_RAWKEY: {
            knum_t k = map_scancode(ev.key & 0xFFu,
                                    (ev.buttons & 1u) != 0);
            AX_Info("QUAKE: rawkey sc=%i down=%i knum=%i\n",
                    (int)(ev.key & 0xFFu), (int)ev.w, (int)k);
            if (k != K_UNKNOWN)
                Key_Event(k, ev.w != 0);
            break;
        }

        case AX_EV_MOUSE: {
            int mx = (int)ev.mx;
            int my = (int)ev.my;

            if (!have_last_mouse) {
                last_mx = mx;
                last_my = my;
                have_last_mouse = true;
            } else {
                int dx = mx - last_mx;
                int dy = my - last_my;

                last_mx = mx;
                last_my = my;

                /* Большой скачок -- курсор вошёл в окно; не крутим камеру. */
                if (dx > -200 && dx < 200 && dy > -200 && dy < 200) {
                    mouse_x += dx;
                    mouse_y += dy;
                }
            }

            if (mouse_available) {
                unsigned b = ev.buttons & 3u;

                if ((b & 1u) != (last_buttons & 1u))
                    Key_Event(K_MOUSE1, (b & 1u) != 0);
                if ((b & 2u) != (last_buttons & 2u))
                    Key_Event(K_MOUSE2, (b & 2u) != 0);

                last_buttons = b;
            }
            break;
        }

        case AX_EV_CLOSE:
        case AX_EV_QUIT:
            exit(0);
            break;

        case AX_EV_RESIZE:
        default:
            /* Разрешение Quake фиксировано -- размер канваса не меняем. */
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Интерфейс input.h                                                  */
/* ------------------------------------------------------------------ */

void IN_Init(void)
{
    mouse_available = COM_CheckParm("-nomouse") ? false : true;
    have_focus = true;
    have_last_mouse = false;
    last_buttons = 0;
    mouse_x = mouse_y = 0;
    old_mouse_x = old_mouse_y = 0;
}

void IN_Shutdown(void)
{
    mouse_available = false;
}

void IN_Commands(void)
{
}

void IN_Accumulate(void)
{
    old_mouse_x = mouse_x;
    old_mouse_y = mouse_y;
}

void IN_ModeChanged(void)
{
}

void IN_AddCommands(void)
{
}

void IN_RegisterVariables(void)
{
    Cvar_RegisterVariable(&m_filter);
    Cvar_RegisterVariable(&_windowed_mouse);
}

void IN_ClearStates(void)
{
    mouse_x = mouse_y = 0;
    old_mouse_x = old_mouse_y = 0;
}

void IN_SetFocus(qboolean focus)
{
    have_focus = focus;
    if (!focus) {
        have_last_mouse = false;
        Key_ClearAllStates();
    }
}

qboolean IN_HaveFocus(void)
{
    return have_focus;
}

/* Логика из in_x11.c: добавляем движение мыши к usercmd. */
void IN_Move(usercmd_t *cmd)
{
    if (!mouse_available)
        return;

    if (m_filter.value) {
        mouse_x = (mouse_x + old_mouse_x) * 0.5;
        mouse_y = (mouse_y + old_mouse_y) * 0.5;
    }
    old_mouse_x = mouse_x;
    old_mouse_y = mouse_y;

    mouse_x *= sensitivity.value;
    mouse_y *= sensitivity.value;

    if ((in_strafe.state & 1) ||
        (lookstrafe.value && ((in_mlook.state & 1) ^ (int)m_freelook.value))) {
        cmd->sidemove += m_side.value * mouse_x;
    } else {
        cl.viewangles[YAW] -= m_yaw.value * mouse_x;
    }

    if ((in_mlook.state & 1) ^ (int)m_freelook.value) {
        if (mouse_x || mouse_y)
            V_StopPitchDrift();
    }

    if (((in_mlook.state & 1) ^ (int)m_freelook.value) &&
        !(in_strafe.state & 1)) {
        cl.viewangles[PITCH] += m_pitch.value * mouse_y;
        if (cl.viewangles[PITCH] > cl_maxpitch.value)
            cl.viewangles[PITCH] = cl_maxpitch.value;
        if (cl.viewangles[PITCH] < cl_minpitch.value)
            cl.viewangles[PITCH] = cl_minpitch.value;
    } else {
        if ((in_strafe.state & 1) && noclip_anglehack)
            cmd->upmove -= m_forward.value * mouse_y;
        else
            cmd->forwardmove -= m_forward.value * mouse_y;
    }

    mouse_x = mouse_y = 0;
}
