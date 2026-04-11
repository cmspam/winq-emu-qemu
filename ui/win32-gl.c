/*
 * QEMU Win32 native display with WGL (native OpenGL)
 *
 * Uses native Windows OpenGL (WGL) for full GL version support,
 * matching what SDL and GTK achieve.  Avoids ANGLE's GLES limitation.
 *
 * Features:
 *   - Native Win32 window with WGL (OpenGL 4.6 on modern GPUs)
 *   - DPI-aware (per-monitor)
 *   - USB tablet support (no SDL grab deadlocks)
 *   - Fullscreen toggle via Ctrl+Alt+F
 *   - Mouse grab via Ctrl+Alt+G
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#ifdef _WIN32

#include <windows.h>
#include <windowsx.h>
#include <epoxy/gl.h>
#include <epoxy/wgl.h>

#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/egl-helpers.h"
#include "ui/shader.h"
#include "system/runstate.h"
#include "system/system.h"
#include "qemu-main.h"
#include "ui/kbd-state.h"
#include <shellapi.h>
#include <commctrl.h>

/* Menu item IDs */
#define IDM_VM_SHUTDOWN_ACPI    40001
#define IDM_VM_SHUTDOWN_GUEST   40002
#define IDM_VM_PAUSE            40003
#define IDM_VM_RESET            40004
#define IDM_VM_QUIT             40005
#define IDM_VIEW_FULLSCREEN     40010
#define IDM_VIEW_GRAB_MOUSE     40011
#define IDM_INPUT_TABLET        40020
#define IDM_HELP_ABOUT          40030
#define IDM_HELP_WEBSITE        40031

typedef struct Win32Console {
    DisplayChangeListener dcl;
    DisplayGLCtx dgc;
    DisplaySurface *surface;
    QKbdState *kbd;
    const DisplayOptions *opts;

    /* Win32 + WGL */
    HWND hwnd;
    HDC hdc;
    HGLRC hglrc;

    /* GL state */
    QemuGLShader *gls;
    egl_fb guest_fb;
    egl_fb win_fb;
    bool scanout_mode;
    bool y0_top;

    /* Window state */
    int win_width, win_height;
    int guest_width, guest_height;
    bool fullscreen;
    RECT saved_rect;
    DWORD saved_style;

    /* Input state */
    bool mouse_grabbed;
    bool absolute_input;
    int updates;
    int idle_counter;

    /* Guest cursor */
    HCURSOR guest_cursor;
    bool guest_cursor_on;
    int guest_cursor_x, guest_cursor_y;
} Win32Console;

static Win32Console *win32_console;
static Notifier win32_mouse_notifier;
static HHOOK win32_keyboard_hook;

/* Forward declarations */
static int win32_vk_to_qcode(WPARAM vk);
static void win32_toggle_fullscreen(Win32Console *con);

#define WINQEMU_REG_KEY "Software\\WINQ-EMU\\Display"

static void win32_save_window_state(Win32Console *con)
{
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, WINQEMU_REG_KEY, 0, NULL,
                        0, KEY_WRITE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RECT r;
        GetClientRect(con->hwnd, &r);
        DWORD w = r.right, h = r.bottom;
        RegSetValueExA(key, "Width", 0, REG_DWORD, (BYTE *)&w, sizeof(w));
        RegSetValueExA(key, "Height", 0, REG_DWORD, (BYTE *)&h, sizeof(h));
        DWORD fs = con->fullscreen ? 1 : 0;
        RegSetValueExA(key, "Fullscreen", 0, REG_DWORD, (BYTE *)&fs, sizeof(fs));
        RegCloseKey(key);
    }
}

static bool win32_load_saved_size(int *w, int *h, bool *fullscreen)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, WINQEMU_REG_KEY, 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    DWORD size, type, val;
    bool ok = false;

    size = sizeof(val);
    if (RegQueryValueExA(key, "Width", NULL, &type, (BYTE *)&val, &size)
        == ERROR_SUCCESS && type == REG_DWORD && val > 0) {
        *w = val;
        size = sizeof(val);
        if (RegQueryValueExA(key, "Height", NULL, &type, (BYTE *)&val, &size)
            == ERROR_SUCCESS && type == REG_DWORD && val > 0) {
            *h = val;
            ok = true;
        }
    }

    size = sizeof(val);
    if (RegQueryValueExA(key, "Fullscreen", NULL, &type, (BYTE *)&val, &size)
        == ERROR_SUCCESS && type == REG_DWORD) {
        *fullscreen = val != 0;
    }

    RegCloseKey(key);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Low-level keyboard hook — capture Win key, Alt+Tab etc.             */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK win32_ll_keyboard_proc(int nCode, WPARAM wParam,
                                                LPARAM lParam)
{
    if (nCode == HC_ACTION && win32_console) {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
        Win32Console *con = &win32_console[0];

        /* Only intercept when our window has focus */
        if (GetForegroundWindow() == con->hwnd) {
            /* Intercept Windows key, Alt+Tab, Alt+Esc, Ctrl+Esc */
            if (kb->vkCode == VK_LWIN || kb->vkCode == VK_RWIN ||
                (kb->vkCode == VK_TAB && (kb->flags & LLKHF_ALTDOWN)) ||
                (kb->vkCode == VK_ESCAPE && (kb->flags & LLKHF_ALTDOWN)) ||
                (kb->vkCode == VK_ESCAPE &&
                 (GetKeyState(VK_CONTROL) & 0x8000))) {

                /* Don't intercept Ctrl+Alt combos — those are our hotkeys */
                if ((GetKeyState(VK_CONTROL) & 0x8000) &&
                    (GetKeyState(VK_MENU) & 0x8000))
                    return CallNextHookEx(win32_keyboard_hook, nCode,
                                         wParam, lParam);

                int qcode = win32_vk_to_qcode(kb->vkCode);
                if (qcode != Q_KEY_CODE_UNMAPPED) {
                    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
                    qkbd_state_key_event(con->kbd, qcode, down);
                }
                return 1; /* Swallow the key */
            }
        }
    }
    return CallNextHookEx(win32_keyboard_hook, nCode, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Keyboard: Win32 VK code to QEMU qcode                              */
/* ------------------------------------------------------------------ */

static int win32_vk_to_qcode(WPARAM vk)
{
    /*
     * For OEM keys (VK_OEM_1 through VK_OEM_8, VK_OEM_102), the
     * auto-generated win32-to-qcode table has ambiguous mappings because
     * keymaps.csv has multiple entries per VK code for different keyboard
     * layouts.  Use MapVirtualKey to get the real AT set 1 scan code for
     * the user's current keyboard layout, then look it up in the
     * atset1-to-qcode table which has unambiguous per-key mappings.
     */
    if ((vk >= VK_OEM_1 && vk <= VK_OEM_3) ||
        (vk >= VK_OEM_4 && vk <= VK_OEM_8) ||
        vk == VK_OEM_102) {
        UINT scancode = MapVirtualKey((UINT)vk, MAPVK_VK_TO_VSC);
        if (scancode && scancode < qemu_input_map_atset1_to_qcode_len) {
            int qcode = qemu_input_map_atset1_to_qcode[scancode];
            if (qcode != Q_KEY_CODE_UNMAPPED) {
                return qcode;
            }
        }
    }

    if (vk < qemu_input_map_win32_to_qcode_len)
        return qemu_input_map_win32_to_qcode[vk];
    return Q_KEY_CODE_UNMAPPED;
}

/* ------------------------------------------------------------------ */
/* Window management                                                   */
/* ------------------------------------------------------------------ */

static void win32_update_title(Win32Console *con)
{
    char title[256];
    snprintf(title, sizeof(title), "WINQ-EMU%s%s",
             con->fullscreen ? " (fullscreen)" : "",
             (con->mouse_grabbed && !con->absolute_input)
                 ? " - Press Ctrl+Alt+G to release mouse" : "");
    SetWindowTextA(con->hwnd, title);
}

static void win32_toggle_fullscreen(Win32Console *con)
{
    con->fullscreen = !con->fullscreen;
    if (con->fullscreen) {
        GetWindowRect(con->hwnd, &con->saved_rect);
        con->saved_style = GetWindowLongA(con->hwnd, GWL_STYLE);
        HMONITOR mon = MonitorFromWindow(con->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { .cbSize = sizeof(mi) };
        GetMonitorInfoA(mon, &mi);
        SetWindowLongA(con->hwnd, GWL_STYLE,
                       con->saved_style & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowPos(con->hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(con->hwnd, GWL_STYLE, con->saved_style);
        SetWindowPos(con->hwnd, NULL,
                     con->saved_rect.left, con->saved_rect.top,
                     con->saved_rect.right - con->saved_rect.left,
                     con->saved_rect.bottom - con->saved_rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    win32_update_title(con);
    win32_save_window_state(con);
}

static void win32_grab_mouse(Win32Console *con)
{
    if (!con->mouse_grabbed) {
        SetCapture(con->hwnd);
        RECT r;
        GetClientRect(con->hwnd, &r);
        MapWindowPoints(con->hwnd, NULL, (POINT *)&r, 2);
        ClipCursor(&r);
        /* Show guest cursor if available, otherwise hide */
        if (con->guest_cursor_on && con->guest_cursor)
            SetCursor(con->guest_cursor);
        else
            ShowCursor(FALSE);
        con->mouse_grabbed = true;
        win32_update_title(con);
    }
}

static void win32_ungrab_mouse(Win32Console *con)
{
    if (con->mouse_grabbed) {
        ClipCursor(NULL);
        ReleaseCapture();
        ShowCursor(TRUE);
        con->mouse_grabbed = false;
        win32_update_title(con);
    }
}

/* ------------------------------------------------------------------ */
/* Close confirmation dialog                                           */
/* ------------------------------------------------------------------ */

static INT_PTR CALLBACK win32_close_dialog_proc(HWND dlg, UINT msg,
                                                 WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1001: /* Shut Down */
        case 1002: /* Force Kill */
        case IDCANCEL: /* Cancel / Escape */
            EndDialog(dlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/*
 * Build an in-memory DLGTEMPLATE for the close dialog.
 * This avoids resource files and gives us full control over button labels.
 */
static DLGTEMPLATE *win32_close_dialog_template(void)
{
    static WORD dlg_data[512];
    WORD *p = dlg_data;

    /* DLGTEMPLATE */
    *p++ = 0; *p++ = 0;  /* style (filled below) */
    *p++ = 0; *p++ = 0;  /* dwExtendedStyle */
    /* DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU */
    DWORD s = 0x80C80080 | WS_CAPTION | WS_SYSMENU;
    dlg_data[0] = (WORD)s;
    dlg_data[1] = (WORD)(s >> 16);

    *p++ = 4;   /* cdit: number of controls */
    *p++ = 0;   /* x */ *p++ = 0;   /* y */
    *p++ = 220; /* cx */ *p++ = 85; /* cy */

    *p++ = 0; /* menu */
    *p++ = 0; /* class */

    /* title: "WINQ-EMU" */
    const char *title = "WINQ-EMU \xE2\x80\x94 Close VM";
    while (*title) *p++ = (WORD)(unsigned char)*title++;
    *p++ = 0;

    /* Static text control */
    if ((ULONG_PTR)p & 2) p++; /* DWORD align */
    *p++ = 0; *p++ = 0; /* style */
    DWORD cs = WS_CHILD | WS_VISIBLE | SS_LEFT;
    p[-2] = (WORD)cs; p[-1] = (WORD)(cs >> 16);
    *p++ = 0; *p++ = 0; /* exStyle */
    *p++ = 10; *p++ = 8; /* x, y */
    *p++ = 200; *p++ = 32; /* cx, cy */
    *p++ = (WORD)-1; /* id */
    *p++ = 0xFFFF; *p++ = 0x0082; /* class: Static */
    const char *text = "How would you like to close the virtual machine?\n(Tip: Hold Alt and click X to force quit instantly)";
    while (*text) *p++ = (WORD)(unsigned char)*text++;
    *p++ = 0;
    *p++ = 0; /* extra */

    /* Button: "Shut Down" (id=1001) */
    if ((ULONG_PTR)p & 2) p++;
    cs = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
    *p++ = (WORD)cs; *p++ = (WORD)(cs >> 16);
    *p++ = 0; *p++ = 0;
    *p++ = 10; *p++ = 58; /* x, y */
    *p++ = 62; *p++ = 16; /* cx, cy */
    *p++ = 1001;
    *p++ = 0xFFFF; *p++ = 0x0080; /* class: Button */
    const char *b1 = "Power Off";
    while (*b1) *p++ = (WORD)(unsigned char)*b1++;
    *p++ = 0;
    *p++ = 0;

    /* Button: "Force Kill" (id=1002) */
    if ((ULONG_PTR)p & 2) p++;
    cs = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    *p++ = (WORD)cs; *p++ = (WORD)(cs >> 16);
    *p++ = 0; *p++ = 0;
    *p++ = 80; *p++ = 58;
    *p++ = 62; *p++ = 16;
    *p++ = 1002;
    *p++ = 0xFFFF; *p++ = 0x0080;
    const char *b2 = "Force Kill";
    while (*b2) *p++ = (WORD)(unsigned char)*b2++;
    *p++ = 0;
    *p++ = 0;

    /* Button: "Cancel" (id=IDCANCEL=2) */
    if ((ULONG_PTR)p & 2) p++;
    cs = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    *p++ = (WORD)cs; *p++ = (WORD)(cs >> 16);
    *p++ = 0; *p++ = 0;
    *p++ = 150; *p++ = 58;
    *p++ = 62; *p++ = 16;
    *p++ = IDCANCEL;
    *p++ = 0xFFFF; *p++ = 0x0080;
    const char *b3 = "Cancel";
    while (*b3) *p++ = (WORD)(unsigned char)*b3++;
    *p++ = 0;
    *p++ = 0;

    return (DLGTEMPLATE *)dlg_data;
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK win32_wndproc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam)
{
    Win32Console *con = (Win32Console *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    if (!con) return DefWindowProcA(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_SIZE:
        con->win_width = LOWORD(lParam);
        con->win_height = HIWORD(lParam);
        if (con->hglrc && con->win_width > 0 && con->win_height > 0 &&
            dpy_ui_info_supported(con->dcl.con)) {
            QemuUIInfo info = *dpy_get_ui_info(con->dcl.con);
            info.width = con->win_width;
            info.height = con->win_height;
            dpy_set_ui_info(con->dcl.con, &info, true);
        }
        if (!con->fullscreen && con->win_width > 0 && con->win_height > 0)
            win32_save_window_state(con);
        return 0;

    case WM_CLOSE: {
        /* Alt+Click on close button = force quit immediately */
        if (GetKeyState(VK_MENU) & 0x8000) {
            exit(0);
        }

        int result = DialogBoxIndirectParamA(
            GetModuleHandleA(NULL),
            win32_close_dialog_template(),
            hwnd,
            win32_close_dialog_proc,
            0);
        if (result == 1001)
            qemu_system_powerdown_request();
        else if (result == 1002)
            exit(0);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int qcode = win32_vk_to_qcode(wParam);
        if (qcode == Q_KEY_CODE_G &&
            (GetKeyState(VK_CONTROL) & 0x8000) &&
            (GetKeyState(VK_MENU) & 0x8000)) {
            if (con->mouse_grabbed) win32_ungrab_mouse(con);
            else win32_grab_mouse(con);
            return 0;
        }
        if (qcode == Q_KEY_CODE_F &&
            (GetKeyState(VK_CONTROL) & 0x8000) &&
            (GetKeyState(VK_MENU) & 0x8000)) {
            win32_toggle_fullscreen(con);
            return 0;
        }
        if (qcode != Q_KEY_CODE_UNMAPPED)
            qkbd_state_key_event(con->kbd, qcode, true);
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int qcode = win32_vk_to_qcode(wParam);
        if (qcode != Q_KEY_CODE_UNMAPPED)
            qkbd_state_key_event(con->kbd, qcode, false);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        int ww = con->win_width ? con->win_width : 1;
        int wh = con->win_height ? con->win_height : 1;

        con->idle_counter = 0;

        if (qemu_input_is_absolute(con->dcl.con) || con->absolute_input) {
            qemu_input_queue_abs(con->dcl.con, INPUT_AXIS_X, x, 0, ww);
            qemu_input_queue_abs(con->dcl.con, INPUT_AXIS_Y, y, 0, wh);
            qemu_input_event_sync();
        }
        /* Relative mode handled by WM_INPUT (raw input) below */
        return 0;
    }

    case WM_INPUT: {
        if (!con->mouse_grabbed) break;

        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) == (UINT)-1)
            break;

        if (raw.header.dwType == RIM_TYPEMOUSE &&
            (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
            int dx = raw.data.mouse.lLastX;
            int dy = raw.data.mouse.lLastY;
            if (dx != 0 || dy != 0) {
                qemu_input_queue_rel(con->dcl.con, INPUT_AXIS_X, dx);
                qemu_input_queue_rel(con->dcl.con, INPUT_AXIS_Y, dy);
                qemu_input_event_sync();
            }
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        con->idle_counter = 0;
        if (!con->mouse_grabbed && !qemu_input_is_absolute(con->dcl.con)
            && msg == WM_LBUTTONDOWN) {
            win32_grab_mouse(con);
            return 0;
        }
        qemu_input_queue_btn(con->dcl.con, INPUT_BUTTON_LEFT,
                             msg == WM_LBUTTONDOWN);
        qemu_input_event_sync();
        return 0;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        con->idle_counter = 0;
        qemu_input_queue_btn(con->dcl.con, INPUT_BUTTON_RIGHT,
                             msg == WM_RBUTTONDOWN);
        qemu_input_event_sync();
        return 0;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        con->idle_counter = 0;
        qemu_input_queue_btn(con->dcl.con, INPUT_BUTTON_MIDDLE,
                             msg == WM_MBUTTONDOWN);
        qemu_input_event_sync();
        return 0;

    case WM_MOUSEWHEEL: {
        con->idle_counter = 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        InputButton btn = delta > 0 ? INPUT_BUTTON_WHEEL_UP
                                    : INPUT_BUTTON_WHEEL_DOWN;
        qemu_input_queue_btn(con->dcl.con, btn, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(con->dcl.con, btn, false);
        qemu_input_event_sync();
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (con->guest_cursor_on && con->guest_cursor) {
                /* Always show guest cursor when available */
                SetCursor(con->guest_cursor);
                return TRUE;
            }
            if (con->mouse_grabbed) {
                /* Grabbed with no guest cursor — hide completely */
                SetCursor(NULL);
                return TRUE;
            }
        }
        break;

    case WM_DPICHANGED: {
        RECT *r = (RECT *)lParam;
        SetWindowPos(hwnd, NULL, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void win32_mouse_mode_change(Notifier *notify, void *data)
{
    if (!win32_console) return;
    Win32Console *con = &win32_console[0];
    con->absolute_input = qemu_input_is_absolute(con->dcl.con);
    if (con->absolute_input)
        win32_ungrab_mouse(con);
}

/* ------------------------------------------------------------------ */
/* WGL context setup                                                   */
/* ------------------------------------------------------------------ */

static bool win32_create_window(Win32Console *con, int w, int h)
{
    /* DPI awareness */
    typedef HRESULT (WINAPI *pSetProcessDpiAwareness)(int);
    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        pSetProcessDpiAwareness fn = (pSetProcessDpiAwareness)
            GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) fn(2);
    }

    /* Load icon from winq-emu.ico next to the executable */
    HICON app_icon = NULL;
    {
        char ico_path[MAX_PATH];
        GetModuleFileNameA(NULL, ico_path, MAX_PATH);
        char *last_sep = strrchr(ico_path, '\\');
        if (last_sep) {
            strcpy(last_sep + 1, "winq-emu.ico");
            app_icon = (HICON)LoadImageA(NULL, ico_path, IMAGE_ICON,
                                          0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        }
    }

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = win32_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "WINQ_EMU_Display";
    wc.hIcon = app_icon;
    wc.hIconSm = app_icon;
    RegisterClassExA(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, style, FALSE);

    con->hwnd = CreateWindowExA(0, "WINQ_EMU_Display", "WINQ-EMU",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                NULL, NULL, wc.hInstance, NULL);
    if (!con->hwnd) return false;

    SetWindowLongPtrA(con->hwnd, GWLP_USERDATA, (LONG_PTR)con);
    con->win_width = w;
    con->win_height = h;

    /* Register for raw mouse input (used for relative mode deltas) */
    RAWINPUTDEVICE rid = {
        .usUsagePage = 0x01, /* HID_USAGE_PAGE_GENERIC */
        .usUsage = 0x02,     /* HID_USAGE_GENERIC_MOUSE */
        .dwFlags = 0,
        .hwndTarget = con->hwnd,
    };
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    return true;
}

static bool win32_wgl_init(Win32Console *con)
{
    /*
     * Create a native WGL OpenGL context — same approach as SDL and GTK.
     * This gives full OpenGL 4.6 from the GPU's ICD driver, not GLES
     * via ANGLE.
     */
    con->hdc = GetDC(con->hwnd);
    if (!con->hdc) return false;

    PIXELFORMATDESCRIPTOR pfd = {
        .nSize = sizeof(pfd),
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 32,
        .cDepthBits = 0,
        .cStencilBits = 0,
        .iLayerType = PFD_MAIN_PLANE,
    };

    int pixel_format = ChoosePixelFormat(con->hdc, &pfd);
    if (!pixel_format) {
        fprintf(stderr, "win32-gl: ChoosePixelFormat failed\n");
        return false;
    }
    SetPixelFormat(con->hdc, pixel_format, &pfd);

    /* Create initial context to get wglCreateContextAttribsARB */
    HGLRC tmp_ctx = wglCreateContext(con->hdc);
    if (!tmp_ctx) {
        fprintf(stderr, "win32-gl: wglCreateContext failed\n");
        return false;
    }
    wglMakeCurrent(con->hdc, tmp_ctx);

    /* Try to create a core profile context for maximum GL version */
    if (epoxy_has_wgl_extension(con->hdc, "WGL_ARB_create_context")) {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        HGLRC core_ctx = wglCreateContextAttribsARB(con->hdc, NULL, attribs);
        if (core_ctx) {
            wglMakeCurrent(con->hdc, core_ctx);
            wglDeleteContext(tmp_ctx);
            con->hglrc = core_ctx;
        } else {
            /* Fall back to the basic context */
            con->hglrc = tmp_ctx;
        }
    } else {
        con->hglrc = tmp_ctx;
    }

    /* Disable vsync if supported */
    if (epoxy_has_wgl_extension(con->hdc, "WGL_EXT_swap_control"))
        wglSwapIntervalEXT(0);

    /* Set qemu_egl_display — grab from current context like SDL does.
     * This may return NULL if there's no EGL backing the WGL context,
     * which is fine — virglrenderer will use the QEMU callbacks instead
     * of the winsys path.
     */
    qemu_egl_display = eglGetCurrentDisplay();

    return true;
}

/* ------------------------------------------------------------------ */
/* DisplayChangeListener ops                                           */
/* ------------------------------------------------------------------ */

static void win32_gl_refresh(DisplayChangeListener *dcl)
{
    Win32Console *con = container_of(dcl, Win32Console, dcl);

    MSG msg;
    while (PeekMessageA(&msg, con->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    graphic_hw_update(dcl->con);

    if (con->updates && con->hwnd) {
        con->updates = 0;
        if (!con->scanout_mode && con->surface && con->gls) {
            wglMakeCurrent(con->hdc, con->hglrc);
            surface_gl_setup_viewport(con->gls, con->surface,
                                      con->win_width, con->win_height);
            surface_gl_render_texture(con->gls, con->surface);
            SwapBuffers(con->hdc);
        }
    }

    con->dcl.update_interval = 1;
}

static void win32_gl_update(DisplayChangeListener *dcl,
                             int x, int y, int w, int h)
{
    Win32Console *con = container_of(dcl, Win32Console, dcl);
    if (!con->hwnd) return;

    wglMakeCurrent(con->hdc, con->hglrc);
    surface_gl_update_texture(con->gls, con->surface, x, y, w, h);
    con->updates++;
}

static void win32_gl_switch(DisplayChangeListener *dcl,
                             DisplaySurface *new_surface)
{
    Win32Console *con = container_of(dcl, Win32Console, dcl);

    wglMakeCurrent(con->hdc, con->hglrc);

    if (con->surface)
        surface_gl_destroy_texture(con->gls, con->surface);

    con->surface = new_surface;
    if (!new_surface) return;

    con->guest_width = surface_width(new_surface);
    con->guest_height = surface_height(new_surface);

    if (!con->gls)
        con->gls = qemu_gl_init_shader();
    surface_gl_create_texture(con->gls, con->surface);

    /* Resize window to match guest resolution changes, but only if the
     * guest requests a resolution >= the current window size.  This
     * prevents BIOS (640x480) from shrinking the window on startup
     * while still allowing the guest desktop to resize the window.
     */
    if (!con->fullscreen &&
        con->guest_width >= con->win_width &&
        con->guest_height >= con->win_height &&
        (con->guest_width != con->win_width ||
         con->guest_height != con->win_height)) {
        DWORD style = GetWindowLongA(con->hwnd, GWL_STYLE);
        RECT wr = { 0, 0, con->guest_width, con->guest_height };
        AdjustWindowRect(&wr, style, FALSE);
        SetWindowPos(con->hwnd, NULL, 0, 0,
                     wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
}

/* ------------------------------------------------------------------ */
/* Guest cursor                                                        */
/* ------------------------------------------------------------------ */

static void win32_gl_cursor_define(DisplayChangeListener *dcl,
                                    QEMUCursor *c)
{
    Win32Console *con = container_of(dcl, Win32Console, dcl);

    if (con->guest_cursor) {
        DestroyCursor(con->guest_cursor);
        con->guest_cursor = NULL;
    }

    /* Build BGRA bitmap from guest's RGBA data (Win32 expects BGRA) */
    int npixels = c->width * c->height;
    uint32_t *bgra = g_malloc(npixels * 4);
    for (int i = 0; i < npixels; i++) {
        uint32_t px = c->data[i];
        uint8_t r = (px >> 0) & 0xff;
        uint8_t g = (px >> 8) & 0xff;
        uint8_t b = (px >> 16) & 0xff;
        uint8_t a = (px >> 24) & 0xff;
        bgra[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }

    HBITMAP hbmColor = CreateBitmap(c->width, c->height, 1, 32, bgra);
    g_free(bgra);

    /* AND mask — all zeros (color bitmap has alpha) */
    int mask_size = ((c->width + 31) / 32) * 4 * c->height;
    uint8_t *mask = g_malloc0(mask_size);
    HBITMAP hbmMask = CreateBitmap(c->width, c->height, 1, 1, mask);
    g_free(mask);

    ICONINFO ii = {
        .fIcon = FALSE,
        .xHotspot = c->hot_x,
        .yHotspot = c->hot_y,
        .hbmMask = hbmMask,
        .hbmColor = hbmColor,
    };
    con->guest_cursor = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);

    /* Apply immediately if cursor is active or if we're grabbed
     * (first cursor define after grab may arrive late)
     */
    if (con->guest_cursor && (con->guest_cursor_on || con->mouse_grabbed)) {
        SetCursor(con->guest_cursor);
        ShowCursor(TRUE);
    }
}

static void win32_gl_mouse_set(DisplayChangeListener *dcl,
                                int x, int y, bool on)
{
    Win32Console *con = container_of(dcl, Win32Console, dcl);

    con->guest_cursor_on = on;
    con->guest_cursor_x = x;
    con->guest_cursor_y = y;

    if (on && con->guest_cursor) {
        SetCursor(con->guest_cursor);
        /* Move Windows cursor to match guest cursor position.
         * Map guest coords to window client coords.
         */
        if (con->mouse_grabbed && con->guest_width && con->guest_height) {
            int wx = (int64_t)x * con->win_width / con->guest_width;
            int wy = (int64_t)y * con->win_height / con->guest_height;
            POINT pt = { wx, wy };
            ClientToScreen(con->hwnd, &pt);
            SetCursorPos(pt.x, pt.y);
        }
    }
}

/* ------------------------------------------------------------------ */
/* GL scanout ops (virtio-gpu GL path)                                 */
/* ------------------------------------------------------------------ */

static void win32_gl_scanout_disable(DisplayChangeListener *dcl)
{
    Win32Console *con = container_of(dcl, Win32Console, dcl);
    wglMakeCurrent(con->hdc, con->hglrc);
    con->scanout_mode = false;
    egl_fb_destroy(&con->guest_fb);
    if (con->surface && con->gls) {
        surface_gl_destroy_texture(con->gls, con->surface);
        surface_gl_create_texture(con->gls, con->surface);
    }
}

static void win32_gl_scanout_texture(DisplayChangeListener *dcl,
                                      uint32_t backing_id,
                                      bool backing_y_0_top,
                                      uint32_t backing_width,
                                      uint32_t backing_height,
                                      uint32_t x, uint32_t y,
                                      uint32_t w, uint32_t h,
                                      void *d3d_tex2d)
{
    Win32Console *con = container_of(dcl, Win32Console, dcl);

    wglMakeCurrent(con->hdc, con->hglrc);
    con->scanout_mode = true;
    con->y0_top = backing_y_0_top;
    egl_fb_setup_for_tex(&con->guest_fb, backing_width, backing_height,
                         backing_id, false);
}

static void win32_gl_scanout_flush(DisplayChangeListener *dcl,
                                    uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h)
{
    Win32Console *con = container_of(dcl, Win32Console, dcl);

    if (!con->scanout_mode || !con->guest_fb.framebuffer) return;

    wglMakeCurrent(con->hdc, con->hglrc);

    egl_fb_setup_default(&con->win_fb, con->win_width, con->win_height, 0, 0);
    egl_fb_blit(&con->win_fb, &con->guest_fb, !con->y0_top);

    SwapBuffers(con->hdc);

    con->idle_counter = 0;
    con->dcl.update_interval = 1;
}

/* ------------------------------------------------------------------ */
/* GL context ops (for virglrenderer)                                  */
/* ------------------------------------------------------------------ */

static QEMUGLContext win32_gl_create_context(DisplayGLCtx *dgc,
                                              QEMUGLParams *params)
{
    Win32Console *con = container_of(dgc, Win32Console, dgc);

    wglMakeCurrent(con->hdc, con->hglrc);

    HGLRC ctx;
    if (epoxy_has_wgl_extension(con->hdc, "WGL_ARB_create_context")) {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, params->major_ver,
            WGL_CONTEXT_MINOR_VERSION_ARB, params->minor_ver,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        ctx = wglCreateContextAttribsARB(con->hdc, con->hglrc, attribs);
    } else {
        ctx = wglCreateContext(con->hdc);
        if (ctx) wglShareLists(con->hglrc, ctx);
    }

    return (QEMUGLContext)ctx;
}

static void win32_gl_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    wglDeleteContext((HGLRC)ctx);
}

static int win32_gl_make_context_current(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    Win32Console *con = container_of(dgc, Win32Console, dgc);
    return wglMakeCurrent(con->hdc, (HGLRC)ctx) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Compatibility check                                                 */
/* ------------------------------------------------------------------ */

static bool win32_gl_ctx_is_compatible_dcl(DisplayGLCtx *dgc,
                                            DisplayChangeListener *dcl)
{
    return true;  /* our GL contexts are compatible with our DCL */
}

static const DisplayChangeListenerOps win32_gl_dcl_ops = {
    .dpy_name               = "win32-gl",
    .dpy_refresh            = win32_gl_refresh,
    .dpy_gfx_update         = win32_gl_update,
    .dpy_gfx_switch         = win32_gl_switch,
    .dpy_mouse_set          = win32_gl_mouse_set,
    .dpy_cursor_define      = win32_gl_cursor_define,
    .dpy_gl_scanout_disable = win32_gl_scanout_disable,
    .dpy_gl_scanout_texture = win32_gl_scanout_texture,
    .dpy_gl_update          = win32_gl_scanout_flush,
};

static const DisplayGLCtxOps win32_gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = win32_gl_ctx_is_compatible_dcl,
    .dpy_gl_ctx_create            = win32_gl_create_context,
    .dpy_gl_ctx_destroy           = win32_gl_destroy_context,
    .dpy_gl_ctx_make_current      = win32_gl_make_context_current,
};

/* ------------------------------------------------------------------ */
/* Display init                                                        */
/* ------------------------------------------------------------------ */

static void win32_display_early_init(DisplayOptions *o)
{
    display_opengl = 1;
}

static void win32_display_init(DisplayState *ds, DisplayOptions *o)
{
    QemuConsole *qcon = qemu_console_lookup_by_index(0);
    if (!qcon || !qemu_console_is_graphic(qcon)) {
        fprintf(stderr, "win32-gl: no graphic console found\n");
        return;
    }

    win32_console = g_new0(Win32Console, 1);
    Win32Console *con = &win32_console[0];

    con->opts = o;
    con->dcl.ops = &win32_gl_dcl_ops;
    con->dgc.ops = &win32_gl_ctx_ops;
    con->dcl.con = qcon;
    con->kbd = qkbd_state_init(qcon);

    /* Load saved window size, or default to 1024x768 */
    int init_w = 1024, init_h = 768;
    bool init_fs = false;
    win32_load_saved_size(&init_w, &init_h, &init_fs);

    if (!win32_create_window(con, init_w, init_h)) {
        fprintf(stderr, "win32-gl: failed to create window\n");
        exit(1);
    }
    if (!win32_wgl_init(con)) {
        fprintf(stderr, "win32-gl: failed to init WGL\n");
        exit(1);
    }

    qemu_console_set_display_gl_ctx(qcon, &con->dgc);
    register_displaychangelistener(&con->dcl);
    qemu_console_set_window_id(qcon, (uintptr_t)con->hwnd);

    win32_mouse_notifier.notify = win32_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&win32_mouse_notifier);

    /* Install low-level keyboard hook to capture Win key, Alt+Tab etc. */
    win32_keyboard_hook = SetWindowsHookExA(WH_KEYBOARD_LL,
                                            win32_ll_keyboard_proc,
                                            GetModuleHandleA(NULL), 0);

    if (init_fs || (o->has_full_screen && o->full_screen))
        win32_toggle_fullscreen(con);

    qemu_main = NULL;
}

static QemuDisplay qemu_display_win32gl = {
    .type       = DISPLAY_TYPE_WIN32_GL,
    .early_init = win32_display_early_init,
    .init       = win32_display_init,
};

static void register_win32gl(void)
{
    qemu_display_register(&qemu_display_win32gl);
}

type_init(register_win32gl);

#endif /* _WIN32 */
