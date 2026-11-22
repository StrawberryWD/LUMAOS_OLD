/* kernel/gui/window1.c - LumaOS 4.1 GUI Core
 *
 * DAY43 PRE-4 changes (this revision):
 *
 *   ① The `write` editor is a WINDOW: gui_modal_open()/gui_modal_pump()/
 *     modal_mouse(). Title-bar drag, [ ] maximize/restore, [_] minimize to a
 *     tab at the bottom-left (click it to restore; while minimized the
 *     desktop stays usable and write ignores typed keys), [x] = Esc. It is
 *     not part of windows[]/z_order[] because its owner is blocked inside a
 *     single shell_exec(); gui_modal_pump() polls the mouse itself.
 *
 *   ② 32bpp (VMware SVGA II): the GUI is unchanged - it still draws palette
 *     indices; vga_gfx.c converts at flip time. New: vga_blend_color() /
 *     vga_blend_rect() give REAL alpha on that indexed buffer through a
 *     pool of on-demand palette entries (see vga_gfx.c), identical on BGA
 *     8bpp and SVGA 32bpp.
 *
 *   ③ Opacity: with Settings > Window Opacity Sim ON the TITLE BAR of EVERY
 *     window (active or not, extra terminals, the write window) is blended
 *     with what is behind it (gui_title_fill, TITLE_ALPHA); the window
 *     CONTENT is always solid (PRE-3's checkerboard body is gone). The body
 *     is therefore drawn first and the title bar second, over the pixels
 *     that are still the desktop / the windows below (gui_draw_chrome).
 *
 *   ④ Top bar re-done macOS-style (logo, bold active-app menu, Programs,
 *     Help, [+ New Terminal] button, display mode, power menu; table-driven
 *     popups with hover highlight; hovering another title switches menus).
 *     Dock: 4 drawn icons (Terminal / Info / Settings / "+" New Terminal),
 *     focus backdrop, running marker, hover tooltip. Desktop: gradient
 *     wallpaper, same icons, labels with a 1px shadow drawn from the SAME
 *     origin as the text, soft window shadows (blend, not solid black).
 *     BUDGET RULE: every distinct (under, over, alpha) blend costs one of
 *     the 192 pool entries - keep alphas few (shadows 2, title 1, bands 8).
 *
 *   Also: Enter now runs a COPY of the input line and empties the live line
 *   first, so a long-running command (the write window) no longer shows the
 *   command twice.
 *
 * DAY43 PRE-3 changes (this revision):
 *
 *   ① DRAG (Settings could never be moved): in gui_handle_mouse() the
 *     "if (i == WIN_SETTINGS) {focus; settings_click; return;}" branch
 *     sat BEFORE the titlebar test and returned for EVERY click inside
 *     the Settings rectangle — titlebar included — so the drag state was
 *     never armed. The titlebar test now comes first; only clicks below
 *     the titlebar go to the Settings controls. Dragging a MAXIMIZED
 *     window now restores it first (like Windows), instead of leaving a
 *     full-screen-sized window stuck in "maximized" state at a new spot.
 *
 *   ② CAPACITY: MAX_WINDOWS 4 -> 8, dynamic window table. Ids 0..2 are the
 *     fixed apps, ids 3..7 are extra Terminal windows created by
 *     gui_window_new_terminal() / destroyed on close (see gui.h). z_order[]
 *     now holds exactly the LIVE windows (z_order_count changes at
 *     runtime); every hit-test already walked it, so they all keep working.
 *     Each extra terminal has a private terminal_t from g_extra_term[].
 *     g_terminal.redraw_needed stays the single global "something changed"
 *     flag for the whole desktop; it is cleared after every frame.
 *
 *   ② OPACITY: the Settings "Window Opacity Sim" switch (g_opacity_sim) was
 *     a no-op. gui_fill_body() now draws the BODY of every INACTIVE
 *     window as a 50% checkerboard when it is on, so the desktop / the
 *     windows behind show through. Titlebar, border, text and controls
 *     stay solid so the window remains readable. (8bpp indexed colour has
 *     no alpha channel; a checkerboard is the classic exact substitute.)
 *
 *   ③ terminal_render() now WRAPS long history lines to the window width
 *     instead of cutting them at visible_cols (an `echo` of 100+ chars used
 *     to lose its tail on screen even though the buffer held it).
 *
 * Original header continues below.
 *
 * STAGE3 changes (this revision):
 *
 *   INPUT HARDENING (①):
 *     gui_handle_key() dispatch is unchanged in shape but every branch
 *     now unconditionally marks g_terminal.redraw_needed = 1 after the
 *     window-specific handler runs, so a keypress can never be "eaten"
 *     silently if a handler forgets to set it. Terminal focus and click
 *     routing were audited end-to-end: IRQ1 -> keyboard_read() ->
 *     gui_handle_key() -> terminal_handle_key() -> redraw_needed=1 ->
 *     gui_refresh() draw. Clicking the Terminal window (titlebar or
 *     body) always calls gui_window_focus(WIN_TERMINAL) which sets
 *     active_window and raises z-order in one place.
 *
 *   Z-ORDER (②):
 *     z_order[] unchanged in concept: index 0 = back-most, last = front.
 *     gui_window_raise(id) moves a window to front. ALL hit-testing
 *     (buttons, resize handle, titlebar drag, body click) walks
 *     z_order[] top-to-bottom (reverse) so the topmost visible window
 *     always wins. The previously-unused spare slot (window 3) is kept
 *     out of z_order entirely to avoid stray hit-tests.
 *
 *   MAXIMIZE / MINIMIZE (③):
 *     Maximize now always uses DESK_X/DESK_Y/DESK_W/DESK_H (not
 *     hand-rolled DOCK_W/MENUBAR_H math) so it can never disagree with
 *     the desktop clip area. Restoring uses the saved geometry. Content
 *     renderers (terminal_render / settings draw / info draw) always
 *     read windows[id].x/y/w/h fresh on every draw call — never cached
 *     or hardcoded — so maximize/restore/resize all stay visually
 *     correct immediately.
 *
 *   RESIZE (④, NEW):
 *     Each visible, non-maximized window gets an 8x8 resize grip in its
 *     bottom-right corner (COL_RESIZE_HANDLE). Mouse-down on the grip
 *     enters resize mode (resize_win/resize_off_x/resize_off_y, mirrors
 *     the existing drag_win pattern). While resizing, w/h are updated
 *     live, clamped to MIN_WIN_W x MIN_WIN_H and to the desktop bounds
 *     (cannot resize under the menu bar or dock, or off the right/
 *     bottom edge of the screen). Mouse-up ends the resize.
 *
 *   LIGHT THEME (⑤):
 *     gui_set_palette() now builds the palette from g_theme_light
 *     (default 1 = light). All draw code still only ever references
 *     colour *indices* (COL_*), so switching themes is just a palette
 *     reprogram + full redraw — no drawing code changes.
 *
 *   SCROLL WHEEL (NEW, requested):
 *     gui_refresh() polls mouse_consume_wheel() every frame and calls
 *     gui_handle_wheel(mx,my,delta) when non-zero. gui_handle_wheel()
 *     routes to whichever window is under the cursor: Terminal scrolls
 *     its history buffer, Info/Settings scroll their content lists.
 *
 *   FIXES carried from Stage 1/2:
 *     DRV-1: terminal_newline scroll keeps total_lines = TERM_BUF_ROWS.
 *     DRV-2: terminal visible columns computed from window width.
 *     popup hit-test areas corrected for 800x450 menubar layout.
 */

#include "../core/kernel.h"
#include "gui.h"
#include "../drivers/keyboard.h"
#include "../fs/vfs/vfs.h"      /* DAY43: vfs_root / vfs_mount_name_of() for the prompt */
#include "../shell/shell.h"    /* DAY43: shell_get_cwd() */
#include "../lib/string.h"

/* ---- external drivers ---- */
extern int mouse_x, mouse_y, mouse_left_button, mouse_right_button;
extern int mouse_consume_wheel(void);

/* ---- forward decls from window2.c ---- */
void gui2_tick(void);
void gui2_draw_overlay(void);
void gui_window2_init(void);
void gui_window2_key(unsigned char key);
void gui_window2_scroll(int delta);
void gui_window2_settings_init(void);
void gui_window2_settings_key(unsigned char key);
void gui_window2_settings_draw(void);
void gui_window2_settings_scroll(int delta);
int  gui_window2_settings_click(int x, int y); /* returns 1 if handled */
void gui_window2_info_draw(void);              /* draws Info content, z-order-safe */

/* ===== Globals ===== */
gui_window_t windows[MAX_WINDOWS];
terminal_t   g_terminal;
int          active_window = WIN_TERMINAL;

/* DAY43: g_gui_mode / modal overlay hook.
 *
 * g_gui_mode is 0 in the text-mode build and until gui_init() has run,
 * 1 afterwards. Shell commands that talk to the screen DIRECTLY (today:
 * `write`, whose editor needs per-keystroke redraws while still inside
 * ONE shell_exec() call) read it to choose between "draw with console_*
 * (VGA text memory 0xB8000)" and "draw with the GUI overlay below".
 * In BGA graphics mode the text memory is not displayed at all, which
 * is exactly why `write` used to look frozen in the GUI: it drew into
 * invisible memory and then blocked in keyboard_getchar() while the
 * only code that repaints the framebuffer (gui_refresh) could not run.
 *
 * g_modal_draw is the hook such a command installs; gui_draw_frame()
 * calls it after all windows are drawn, so the command's panel sits on
 * top of the desktop. gui_modal_pump() is the "one iteration of the
 * GUI loop" the command calls while it waits for a key. */
int          g_gui_mode    = 0;
dock_item_t  dock_items[DOCK_APPS];
desk_icon_t  desk_icons[DESK_ICON_MAX];
int          desk_icon_count = 0;

/* Stage 3: theme + settings-owned toggles (definitions; declared extern
 * in gui.h so window2.c and this file share one copy). */
int g_theme_light        = 1;   /* default: light theme */
int g_show_desktop_icons = 1;
int g_opacity_sim        = 0;

/* ===== Z-ORDER array =====
 * z_order[0] = drawn first (back), z_order[MAX_WINDOWS-1] = drawn last (front).
 * Only WIN_TERMINAL/WIN_INFO/WIN_SETTINGS ever participate; the spare
 * window slot (id 3) is never placed in z_order and never drawn.
 */
static int z_order[MAX_WINDOWS];
static int z_order_count = 3; /* PRE-3: number of LIVE windows (3..MAX_WINDOWS), front of z_order[] */

/* PRE-3: private terminals for the dynamic windows (ids GUI_EXTRA_FIRST..),
 * 113 236 bytes each.
 *
 * They are NOT ordinary .bss arrays, and that is deliberate: the kernel
 * is linked at 0x10000 and its .bss already ends at ~0x79000, only ~0x27000
 * bytes below 0xA0000 — the start of VGA/ROM space in the PC memory map.
 * A first version with `static terminal_t g_extra_term[5]` put the 2nd
 * terminal across 0xA0000 (writes into video RAM, input silently lost —
 * found in QEMU: the 3rd window never accepted keys). The pool therefore
 * sits in plain RAM at 0x00500000 (map: 0x10000 kernel, 0x200000 stack
 * top, 0x300000 back buffer, 0x400000 SVGA prev_frame, 0x500000 THIS,
 * 5 x 113 236 B = 553 KB; free up to the -m 256 limit). terminal_init()
 * fully initialises a slot when its window is created. */
#define GUI_TERM_POOL  0x00500000u
#define g_extra_term   ((terminal_t *)GUI_TERM_POOL)
static int        win_alive[MAX_WINDOWS];      /* 1 = slot holds a live window */
/* id of the window terminal_render() is drawing (for gui_fill_body) */
static int        g_render_win = WIN_TERMINAL;

/* Raise window `id` to front (top of the live z_order range) */
static void gui_window_raise(int id)
{
    int pos = -1;
    for (int i = 0; i < z_order_count; i++) {
        if (z_order[i] == id) { pos = i; break; }
    }
    if (pos < 0 || pos == z_order_count - 1) return; /* not found or already front */
    for (int i = pos; i < z_order_count - 1; i++)
        z_order[i] = z_order[i + 1];
    z_order[z_order_count - 1] = id;
}

static int prev_mx = 0, prev_my = 0, prev_ml = 0, prev_mr = 0;


/* ---- drag state ---- */
static int drag_win   = -1;
static int drag_off_x = 0;
static int drag_off_y = 0;

/* ---- resize state (④ NEW) ---- */
static int resize_win   = -1;
static int resize_off_x = 0;   /* cursor offset from window's bottom-right corner at grab time */
static int resize_off_y = 0;

/* ---- per-window maximize state ---- */
static int win_maximized[MAX_WINDOWS];
static int win_saved_x[MAX_WINDOWS], win_saved_y[MAX_WINDOWS];
static int win_saved_w[MAX_WINDOWS], win_saved_h[MAX_WINDOWS];

/* ===== Palette ===== */
static void gui_set_palette_dark(void)
{
    /* Standard 16 (VGA-ish) */
    vga_set_palette( 0,  0,  0,  0);
    vga_set_palette( 1,  0,  0, 42);
    vga_set_palette( 2,  0, 42,  0);
    vga_set_palette( 3,  0, 42, 42);
    vga_set_palette( 4, 42,  0,  0);
    vga_set_palette( 5, 42,  0, 42);
    vga_set_palette( 6, 42, 21,  0);
    vga_set_palette( 7, 42, 42, 42);
    vga_set_palette( 8, 21, 21, 21);
    vga_set_palette( 9, 21, 21, 63);
    vga_set_palette(10, 21, 63, 21);
    vga_set_palette(11, 21, 63, 63);
    vga_set_palette(12, 63, 21, 21);
    vga_set_palette(13, 63, 21, 63);
    vga_set_palette(14, 63, 63, 21);
    vga_set_palette(15, 63, 63, 63);

    vga_set_palette(COL_DESKTOP,   24, 40, 63);
    vga_set_palette(COL_MENUBAR,    3,  3,  8);
    vga_set_palette(COL_MENUTEXT,  55, 55, 55);
    vga_set_palette(COL_DOCK_BG,   50, 52, 55);
    vga_set_palette(COL_DOCK_ICON0, 0, 30, 55);
    vga_set_palette(COL_DOCK_ICON1,10, 40, 20);
    vga_set_palette(COL_WIN_TITLE,  8, 20, 50);
    vga_set_palette(COL_WIN_BODY,   4,  4, 10);
    vga_set_palette(COL_WIN_BORDER, 0,  0,  0);
    vga_set_palette(COL_WIN_INACT, 18, 20, 28);
    vga_set_palette(COL_TERM_BG,    2,  2,  5);
    vga_set_palette(COL_TERM_FG,   48, 52, 48);
    vga_set_palette(COL_TERM_CUR,   0, 63,  0);
    vga_set_palette(COL_BTN_CLOSE, 55, 10, 10);
    vga_set_palette(COL_BTN_MIN,   55, 48,  5);
    vga_set_palette(COL_BTN_MAX,   10, 45, 10);
    vga_set_palette(COL_SHADOW,     1,  1,  3);
    vga_set_palette(COL_POPUP_BG,   5,  5, 12);
    vga_set_palette(COL_SCROLLBAR, 10, 15, 30);
    vga_set_palette(COL_SCROLLTHM, 25, 40, 63);
    vga_set_palette(COL_WIN_TITLE_INACTIVE, 12, 14, 20);
    vga_set_palette(COL_RESIZE_HANDLE,      30, 50, 63);
    vga_set_palette(COL_DESKTOP_ICON_BG,     8, 12, 20);
    vga_set_palette(COL_SETTINGS_BG,         6,  6, 14);
    vga_set_palette(COL_TEXT_PRIMARY,       55, 55, 55);
    vga_set_palette(COL_TEXT_SECONDARY,     35, 35, 38);
    vga_set_palette(COL_BTN_TOGGLE_ON,      10, 45, 10);
    vga_set_palette(COL_BTN_TOGGLE_OFF,     30, 10, 10);
    vga_set_palette(COL_PANEL_BORDER,       20, 22, 28);
}

/* ⑤ Light theme: bright desktop / menu / dock / titlebars, near-white
 * content backgrounds, dark text. Button semantic colours (red/yellow/
 * green) kept but toned to sit well on a light background. */
static void gui_set_palette_light(void)
{
    /* Standard 16 — keep VGA-standard so terminal ANSI-ish colour codes
     * (LRED/LGREEN/LCYAN/etc used by the shell) still read clearly on
     * a light terminal background. */
    vga_set_palette( 0,  0,  0,  0);
    vga_set_palette( 1,  0,  0, 42);
    vga_set_palette( 2,  0, 40,  0);
    vga_set_palette( 3,  0, 42, 42);
    vga_set_palette( 4, 45,  0,  0);
    vga_set_palette( 5, 42,  0, 42);
    vga_set_palette( 6, 42, 21,  0);
    vga_set_palette( 7, 42, 42, 42);
    vga_set_palette( 8, 21, 21, 21);
    vga_set_palette( 9, 10, 20, 50);
    vga_set_palette(10,  5, 45,  5);
    vga_set_palette(11, 10, 45, 45);
    vga_set_palette(12, 50, 10, 10);
    vga_set_palette(13, 45, 10, 45);
    vga_set_palette(14, 50, 42,  0);
    vga_set_palette(15, 63, 63, 63);

    /* Desktop: light sky-blue / light grey-blue */
    vga_set_palette(COL_DESKTOP,   45, 55, 63);
    /* Menu bar: light grey-white with dark text */
    vga_set_palette(COL_MENUBAR,   58, 59, 61);
    vga_set_palette(COL_MENUTEXT,   8,  8, 10);
    /* Dock: light grey-white */
    vga_set_palette(COL_DOCK_BG,   56, 57, 59);
    vga_set_palette(COL_DOCK_ICON0,30, 45, 60);
    vga_set_palette(COL_DOCK_ICON1,35, 50, 35);
    /* Active titlebar: light blue */
    vga_set_palette(COL_WIN_TITLE, 30, 48, 62);
    /* Window body: near-white */
    vga_set_palette(COL_WIN_BODY,  60, 60, 61);
    vga_set_palette(COL_WIN_BORDER,20, 22, 26);
    /* Inactive titlebar: lighter grey-blue */
    vga_set_palette(COL_WIN_INACT, 48, 50, 54);
    /* Terminal: light grey background, dark text */
    vga_set_palette(COL_TERM_BG,   54, 54, 55);
    vga_set_palette(COL_TERM_FG,    8,  9, 10);
    vga_set_palette(COL_TERM_CUR,   0, 40,  0);
    /* Buttons keep red/yellow/green semantics, softened for light bg */
    vga_set_palette(COL_BTN_CLOSE, 58, 22, 20);
    vga_set_palette(COL_BTN_MIN,   58, 50, 15);
    vga_set_palette(COL_BTN_MAX,   20, 48, 20);
    vga_set_palette(COL_SHADOW,    40, 41, 43);
    vga_set_palette(COL_POPUP_BG,  59, 60, 61);
    vga_set_palette(COL_SCROLLBAR, 45, 46, 48);
    vga_set_palette(COL_SCROLLTHM, 20, 30, 45);
    vga_set_palette(COL_WIN_TITLE_INACTIVE, 50, 51, 53);
    vga_set_palette(COL_RESIZE_HANDLE,      15, 35, 55);
    vga_set_palette(COL_DESKTOP_ICON_BG,    50, 52, 55);
    vga_set_palette(COL_SETTINGS_BG,        61, 61, 62);
    /* Dark text for light backgrounds */
    vga_set_palette(COL_TEXT_PRIMARY,        6,  7,  9);
    vga_set_palette(COL_TEXT_SECONDARY,     28, 29, 31);
    vga_set_palette(COL_BTN_TOGGLE_ON,      12, 45, 12);
    vga_set_palette(COL_BTN_TOGGLE_OFF,     45, 15, 15);
    vga_set_palette(COL_PANEL_BORDER,       40, 41, 44);
}

/* PRE-4: icon / menu / label colours (indices 45..56) for both themes */
static void gui_set_palette_extra(int light)
{
    vga_set_palette(COL_ICON_TERM,  10, 12, 18);
    vga_set_palette(COL_ICON_INFO,  12, 32, 56);
    vga_set_palette(COL_ICON_SET,   30, 32, 38);
    vga_set_palette(COL_ICON_NEW,    8, 36, 30);
    vga_set_palette(COL_MENU_HILITE, 14, 34, 60);
    vga_set_palette(COL_MENU_HILITE_TEXT, 63, 63, 63);
    vga_set_palette(COL_LABEL_TEXT,  63, 63, 63);
    vga_set_palette(COL_LABEL_SHADOW, 3,  5, 12);
    vga_set_palette(COL_ICON_GLYPH,  60, 62, 63);
    vga_set_palette(COL_DOCK_HILITE, light ? 44 : 20, light ? 49 : 24, light ? 58 : 36);
    vga_set_palette(COL_ICON_EDGE,    4,  6, 10);
    vga_set_palette(COL_MENUBAR_LINE, light ? 36 : 14, light ? 38 : 16, light ? 42 : 24);
}

void gui_set_palette(void)
{
    if (g_theme_light) gui_set_palette_light();
    else                gui_set_palette_dark();
    gui_set_palette_extra(g_theme_light);
}

void gui_apply_theme(int light)
{
    g_theme_light = light ? 1 : 0;
    gui_set_palette();
    g_terminal.redraw_needed = 1;
}

/* ===== gui_clip ===== */
int gui_clip(int *x, int *y, int *w, int *h,
             int cx, int cy, int cw, int ch)
{
    int x1 = *x, y1 = *y, x2 = *x + *w, y2 = *y + *h;
    int cx2 = cx + cw, cy2 = cy + ch;
    if (x1 < cx) x1 = cx;
    if (y1 < cy) y1 = cy;
    if (x2 > cx2) x2 = cx2;
    if (y2 > cy2) y2 = cy2;
    *x = x1; *y = y1; *w = x2 - x1; *h = y2 - y1;
    return (*w > 0 && *h > 0);
}

/* =====================================================================
 * DAY43 PRE-4: shared drawing helpers (icons, popups, translucency)
 * ===================================================================== */

#define TITLE_ALPHA   185   /* title bar: 72% title colour over what is behind (Opacity ON) */

/* Title-bar fill. With the Settings "Window Opacity Sim" switch ON the
 * colour is BLENDED with whatever is already in the back buffer (the
 * desktop or a lower window), so the bar is truly see-through; the window
 * CONTENT is never translucent. (PRE-3 did the opposite: it stippled the
 * body of inactive windows.) */
static void gui_title_fill(int x, int y, int w, int h, unsigned char color)
{
    if (g_opacity_sim) vga_blend_rect(x, y, w, h, color, TITLE_ALPHA);
    else               vga_draw_rect(x, y, w, h, color);
}

/* soft drop shadow: two 1px rings, lighter outwards. The alphas are FIXED
 * (not per-window): every distinct (under colour, alpha) pair costs one entry
 * of the 192-entry blend pool, so variety here is expensive. */
#define SHADOW_A0 72
#define SHADOW_A1 36
static void gui_draw_shadow(int x, int y, int w, int h, int strength)
{
    (void)strength;
    vga_blend_rect(x + w,     y + 2, 1, h,     COL_BLACK, SHADOW_A0);   /* right  */
    vga_blend_rect(x + 2,     y + h, w, 1,     COL_BLACK, SHADOW_A0);   /* bottom */
    vga_blend_rect(x + w + 1, y + 3, 1, h,     COL_BLACK, SHADOW_A1);
    vga_blend_rect(x + 3,     y + h + 1, w, 1, COL_BLACK, SHADOW_A1);
}

/* text with a 1px drop shadow (labels on the desktop) */
static void gui_draw_text_shadow(int x, int y, char *s, unsigned char fg, unsigned char sh)
{
    vga_draw_string(x + 1, y + 1, s, sh);
    vga_draw_string(x,     y,     s, fg);
}

/* faux-bold: same string twice, 1px apart */
static void gui_draw_text_bold(int x, int y, char *s, unsigned char col)
{
    vga_draw_string(x,     y, s, col);
    vga_draw_string(x + 1, y, s, col);
}

/* rounded tile, `s` px square, 2px corner radius, border + gloss + shade */
static void gui_draw_tile(int x, int y, int s, unsigned char color)
{
    vga_draw_rect(x + 2, y,     s - 4, s,     color);
    vga_draw_rect(x + 1, y + 1, s - 2, s - 2, color);
    vga_draw_rect(x,     y + 2, s,     s - 4, color);
    vga_blend_rect(x + 2, y + 1, s - 4, s / 2 - 1, COL_WHITE, 46);      /* gloss, top half */
    vga_blend_rect(x + 2, y + s - 3, s - 4, 2, COL_BLACK, 70);         /* shade, bottom   */
    vga_draw_hline(x + 2, y,         s - 4, COL_ICON_EDGE);             /* outline         */
    vga_draw_hline(x + 2, y + s - 1, s - 4, COL_ICON_EDGE);
    vga_draw_vline(x,         y + 2, s - 4, COL_ICON_EDGE);
    vga_draw_vline(x + s - 1, y + 2, s - 4, COL_ICON_EDGE);
    vga_put_pixel(x + 1,     y + 1,     COL_ICON_EDGE);
    vga_put_pixel(x + s - 2, y + 1,     COL_ICON_EDGE);
    vga_put_pixel(x + 1,     y + s - 2, COL_ICON_EDGE);
    vga_put_pixel(x + s - 2, y + s - 2, COL_ICON_EDGE);
}

static void gui_fill_disc(int cx, int cy, int r, unsigned char c)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r / 2)
                vga_put_pixel(cx + dx, cy + dy, c);
}

#define ICON_TERM    0
#define ICON_INFO    1
#define ICON_SET     2
#define ICON_NEWTERM 3

/* One app icon: tile + glyph, centred in an s x s square at (x,y). The
 * glyph is drawn from primitives around the tile centre, so the same code
 * serves the 36px dock icons and the 40px desktop icons. */
static void gui_draw_app_icon(int kind, int x, int y, int s)
{
    unsigned char bg = (kind == ICON_TERM) ? COL_ICON_TERM
                     : (kind == ICON_INFO) ? COL_ICON_INFO
                     : (kind == ICON_SET)  ? COL_ICON_SET : COL_ICON_NEW;
    gui_draw_tile(x, y, s, bg);
    int cx = x + s / 2, cy = y + s / 2;
    unsigned char g = COL_ICON_GLYPH;

    if (kind == ICON_TERM) {
        for (int i = 0; i <= 6; i++) {                       /* ">" chevron */
            vga_draw_rect(cx - 9 + i, cy - 7 + i, 2, 2, COL_LGREEN);
            vga_draw_rect(cx - 9 + i, cy + 5 - i, 2, 2, COL_LGREEN);
        }
        vga_draw_rect(cx + 1, cy + 4, 8, 2, g);              /* "_" cursor  */
    } else if (kind == ICON_INFO) {
        vga_draw_circle(cx, cy, 10, g);
        vga_draw_circle(cx, cy, 9,  g);
        vga_draw_rect(cx - 1, cy - 6, 3, 3, g);              /* dot of the i */
        vga_draw_rect(cx - 1, cy - 1, 3, 8, g);              /* stem         */
    } else if (kind == ICON_SET) {
        static const signed char tp[8][2] = {{0,-10},{0,10},{-10,0},{10,0},{-7,-7},{7,-7},{-7,7},{7,7}};
        for (int i = 0; i < 8; i++)
            vga_draw_rect(cx + tp[i][0] - 1, cy + tp[i][1] - 1, 3, 3, g);   /* teeth */
        gui_fill_disc(cx, cy, 7, g);                                        /* body  */
        gui_fill_disc(cx, cy, 3, bg);                                       /* hole  */
    } else {                                                 /* new terminal: frame + plus */
        vga_draw_hline(cx - 11, cy - 9, 22, g);
        vga_draw_hline(cx - 11, cy + 9, 22, g);
        vga_draw_vline(cx - 11, cy - 9, 19, g);
        vga_draw_vline(cx + 10, cy - 9, 19, g);
        vga_draw_rect(cx - 1, cy - 6, 3, 13, g);             /* + vertical   */
        vga_draw_rect(cx - 6, cy - 1, 13, 3, g);             /* + horizontal */
    }
}

/* ===== popup menus (macOS-style, table driven) ===== */
enum {
    A_NONE = 0, A_ABOUT, A_SETTINGS, A_SHUTDOWN, A_REBOOT, A_NEWTERM,
    A_CLOSEWIN, A_MINWIN, A_ZOOMWIN, A_OPEN_TERM, A_OPEN_INFO, A_OPEN_SET,
    A_HELP, A_VERSION
};
typedef struct { const char *label; int act; } mitem_t;      /* label "-" = separator */

#define MENU_LOGO  1
#define MENU_APP   2
#define MENU_PROG  3
#define MENU_HELP  4
#define MENU_POWER 5
#define MENU_ITEM_H 15
#define MENU_SEP_H  7

static const mitem_t menu_logo[]  = { {"About LumaOS", A_ABOUT}, {"System Settings", A_SETTINGS},
                                      {"-", 0}, {"Shut Down", A_SHUTDOWN}, {"Restart", A_REBOOT} };
static const mitem_t menu_app[]   = { {"New Terminal", A_NEWTERM}, {"-", 0}, {"Minimize", A_MINWIN},
                                      {"Zoom", A_ZOOMWIN}, {"Close Window", A_CLOSEWIN} };
static const mitem_t menu_prog[]  = { {"Terminal", A_OPEN_TERM}, {"Info", A_OPEN_INFO},
                                      {"Settings", A_OPEN_SET}, {"-", 0}, {"New Terminal", A_NEWTERM} };
static const mitem_t menu_help[]  = { {"Help", A_HELP}, {"Version", A_VERSION} };
static const mitem_t menu_power[] = { {"Shut Down", A_SHUTDOWN}, {"Restart", A_REBOOT} };

static const mitem_t *menu_items(int m, int *n)
{
    switch (m) {
    case MENU_LOGO:  *n = 5; return menu_logo;
    case MENU_APP:   *n = 5; return menu_app;
    case MENU_PROG:  *n = 5; return menu_prog;
    case MENU_HELP:  *n = 2; return menu_help;
    default:         *n = 2; return menu_power;
    }
}

/* popup state (declared here: the menu helpers below need it) */
static int popup_menu = 0;          /* 0 = none, MENU_* otherwise */

/* ---- menu-bar geometry (recomputed from the active window's title) ---- */
#define MB_LOGO_W   78
#define NEWTERM_BTN_W 124
static char app_name[24];

static void menubar_app_name(void)
{
    const char *t = (active_window >= 0 && (windows[active_window].flags & WIN_VISIBLE))
                  ? windows[active_window].title : "Desktop";
    int i = 0;
    while (t[i] && i < 14) { app_name[i] = t[i]; i++; }
    app_name[i] = 0;
}

/* x/width of the title of menu m (MENU_LOGO..MENU_HELP) */
static void menubar_title_rect(int m, int *x, int *w)
{
    int ax = MB_LOGO_W + 6;
    int aw = (int)strlen(app_name) * 8 + 14;
    int px = ax + aw;
    int pw = 8 * 8 + 14;
    int hx = px + pw;
    int hw = 4 * 8 + 14;
    switch (m) {
    case MENU_LOGO: *x = 0;  *w = MB_LOGO_W; break;
    case MENU_APP:  *x = ax; *w = aw; break;
    case MENU_PROG: *x = px; *w = pw; break;
    default:        *x = hx; *w = hw; break;
    }
}

static int menubar_menu_at(int x)
{
    for (int m = MENU_LOGO; m <= MENU_HELP; m++) {
        int mx, mw; menubar_title_rect(m, &mx, &mw);
        if (x >= mx && x < mx + mw) return m;
    }
    return 0;
}

#define POWER_X   (SCREEN_W - 26)
#define NEWBTN_X  (POWER_X - 8 - NEWTERM_BTN_W)

/* popup rectangle of menu m */
static void popup_rect(int m, int *px, int *py, int *pw, int *ph)
{
    int n; const mitem_t *it = menu_items(m, &n);
    int maxl = 0, h = 4;
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(it[i].label);
        if (it[i].label[0] != '-' && l > maxl) maxl = l;
        h += (it[i].label[0] == '-' && !it[i].label[1]) ? MENU_SEP_H : MENU_ITEM_H;
    }
    *pw = maxl * 8 + 26;
    *ph = h;
    *py = MENUBAR_H;
    if (m == MENU_POWER) { *px = SCREEN_W - *pw - 4; }
    else { int tw; menubar_title_rect(m, px, &tw); if (*px < 4) *px = 4; }
}

/* which item index is at (x,y) inside popup m, -1 if none/separator */
static int popup_item_at(int m, int x, int y)
{
    int px, py, pw, ph; popup_rect(m, &px, &py, &pw, &ph);
    if (x < px || x >= px + pw || y < py || y >= py + ph) return -1;
    int n; const mitem_t *it = menu_items(m, &n);
    int ry = py + 2;
    for (int i = 0; i < n; i++) {
        int sep = (it[i].label[0] == '-' && !it[i].label[1]);
        int h = sep ? MENU_SEP_H : MENU_ITEM_H;
        if (y >= ry && y < ry + h) return sep ? -1 : i;
        ry += h;
    }
    return -1;
}

static void popup_draw(int m)
{
    int px, py, pw, ph; popup_rect(m, &px, &py, &pw, &ph);
    gui_draw_shadow(px, py, pw, ph, 80);
    vga_draw_rect(px, py, pw, ph, COL_POPUP_BG);
    vga_draw_hline(px, py + ph - 1, pw, COL_DGRAY);
    vga_draw_vline(px, py, ph, COL_DGRAY);
    vga_draw_vline(px + pw - 1, py, ph, COL_DGRAY);
    int hov = popup_item_at(m, mouse_x, mouse_y);
    int n; const mitem_t *it = menu_items(m, &n);
    int ry = py + 2;
    for (int i = 0; i < n; i++) {
        int sep = (it[i].label[0] == '-' && !it[i].label[1]);
        if (sep) {
            vga_draw_hline(px + 6, ry + MENU_SEP_H / 2, pw - 12, COL_PANEL_BORDER);
            ry += MENU_SEP_H;
            continue;
        }
        unsigned char tc = COL_MENUTEXT;
        if (i == hov) {
            vga_draw_rect(px + 2, ry, pw - 4, MENU_ITEM_H, COL_MENU_HILITE);
            tc = COL_MENU_HILITE_TEXT;
        }
        vga_draw_string(px + 12, ry + 4, (char *)it[i].label, tc);
        ry += MENU_ITEM_H;
    }
}

/* ===== MenuBar (macOS-style) =====
 *   [logo] LumaOS | <App name, bold> | Programs | Help        ...   [+ New Terminal]  mode  (power)
 * The bar is translucent (blended over the wallpaper). The title of the open
 * menu is highlighted; the bold item is the active window's title. */
void gui_draw_menubar(void)
{
    menubar_app_name();

    vga_blend_rect(0, 0, SCREEN_W, MENUBAR_H, COL_MENUBAR, 215);
    vga_draw_hline(0, MENUBAR_H - 1, SCREEN_W, COL_MENUBAR_LINE);

    /* open-menu highlight behind its title */
    if (popup_menu >= MENU_LOGO && popup_menu <= MENU_HELP) {
        int hx, hw; menubar_title_rect(popup_menu, &hx, &hw);
        vga_draw_rect(hx, 1, hw, MENUBAR_H - 2, COL_MENU_HILITE);
    }
    unsigned char t_logo = (popup_menu == MENU_LOGO) ? COL_MENU_HILITE_TEXT : COL_MENUTEXT;
    unsigned char t_app  = (popup_menu == MENU_APP)  ? COL_MENU_HILITE_TEXT : COL_MENUTEXT;
    unsigned char t_prog = (popup_menu == MENU_PROG) ? COL_MENU_HILITE_TEXT : COL_MENUTEXT;
    unsigned char t_help = (popup_menu == MENU_HELP) ? COL_MENU_HILITE_TEXT : COL_MENUTEXT;

    /* logo: small rounded tile with an "L" */
    gui_draw_tile(8, 4, 12, COL_ICON_INFO);
    vga_draw_rect(12, 7, 2, 6, COL_ICON_GLYPH);
    vga_draw_rect(12, 11, 5, 2, COL_ICON_GLYPH);
    gui_draw_text_bold(26, 6, "LumaOS", t_logo);

    int x, w;
    menubar_title_rect(MENU_APP, &x, &w);
    gui_draw_text_bold(x + 7, 6, app_name, t_app);
    menubar_title_rect(MENU_PROG, &x, &w);
    vga_draw_string(x + 7, 6, "Programs", t_prog);
    menubar_title_rect(MENU_HELP, &x, &w);
    vga_draw_string(x + 7, 6, "Help", t_help);

    /* right side: [+ New Terminal] button, display mode, power */
    int hot = (mouse_y < MENUBAR_H && mouse_x >= NEWBTN_X && mouse_x < NEWBTN_X + NEWTERM_BTN_W);
    vga_draw_rect(NEWBTN_X, 2, NEWTERM_BTN_W, MENUBAR_H - 4, hot ? COL_MENU_HILITE : COL_ICON_NEW);
    vga_draw_hline(NEWBTN_X, 2, NEWTERM_BTN_W, COL_ICON_EDGE);
    vga_draw_hline(NEWBTN_X, MENUBAR_H - 3, NEWTERM_BTN_W, COL_ICON_EDGE);
    vga_draw_vline(NEWBTN_X, 2, MENUBAR_H - 4, COL_ICON_EDGE);
    vga_draw_vline(NEWBTN_X + NEWTERM_BTN_W - 1, 2, MENUBAR_H - 4, COL_ICON_EDGE);
    vga_draw_string(NEWBTN_X + 6, 6, "+ New Terminal", COL_MENU_HILITE_TEXT);

    char *mode = vga_gfx_is_svga() ? "SVGA 32bpp" : "BGA 8bpp";
    vga_draw_string(NEWBTN_X - 10 - (int)strlen(mode) * 8, 6, mode, COL_TEXT_SECONDARY);

    /* power glyph: ring + stem */
    unsigned char pc = (popup_menu == MENU_POWER) ? COL_LRED : COL_MENUTEXT;
    vga_draw_circle(POWER_X + 8, 11, 5, pc);
    vga_draw_circle(POWER_X + 8, 11, 4, pc);
    vga_draw_rect(POWER_X + 7, 3, 2, 8, pc);

    if (popup_menu) popup_draw(popup_menu);
}

/* ===== Dock (left) ===== */
#define DOCK_ICON_Y0   (MENUBAR_H + 10)
#define DOCK_ICON_STEP (DOCK_ICON_SZ + 10)

static int dock_hover_item(int x, int y)
{
    if (x < 0 || x >= DOCK_W) return -1;
    for (int i = 0; i < DOCK_APPS; i++)
        if (y >= dock_items[i].y && y < dock_items[i].y + DOCK_ICON_SZ) return i;
    return -1;
}

void gui_draw_dock(void)
{
    vga_draw_rect(0, MENUBAR_H, DOCK_W, SCREEN_H - MENUBAR_H, COL_DOCK_BG);
    vga_draw_vline(DOCK_W - 1, MENUBAR_H, SCREEN_H - MENUBAR_H, COL_DGRAY);
    vga_blend_rect(DOCK_W - 4, MENUBAR_H, 3, SCREEN_H - MENUBAR_H, COL_BLACK, 22);   /* inner edge shade */

    int hov = dock_hover_item(mouse_x, mouse_y);

    for (int i = 0; i < DOCK_APPS; i++) {
        dock_item_t *d = &dock_items[i];
        int wid = d->win_id;
        int open      = (wid >= 0 && (windows[wid].flags & WIN_VISIBLE));
        int minimized = (wid >= 0 && (windows[wid].flags & WIN_MINIMIZED));
        int focused   = (wid >= 0 && wid == active_window && open && !minimized);

        if (i == DOCK_APP_NEW)    /* separator above the "new terminal" button */
            vga_draw_hline(8, d->y - 9, DOCK_W - 16, COL_PANEL_BORDER);

        if (focused)
            vga_draw_rect(d->x - 3, d->y - 3, DOCK_ICON_SZ + 6, DOCK_ICON_SZ + 6, COL_DOCK_HILITE);

        gui_draw_app_icon(d->app_id, d->x, d->y, DOCK_ICON_SZ);
        if (i == hov)
            vga_blend_rect(d->x + 1, d->y + 1, DOCK_ICON_SZ - 2, DOCK_ICON_SZ - 2, COL_WHITE, 60);
        if (minimized)
            vga_blend_rect(d->x + 1, d->y + 1, DOCK_ICON_SZ - 2, DOCK_ICON_SZ - 2, COL_DOCK_BG, 120);

        /* "running" marker on the left edge */
        if (open)
            vga_draw_rect(1, d->y + DOCK_ICON_SZ / 2 - 2, 3, 4, COL_TEXT_PRIMARY);
    }

    /* tooltip for the hovered icon */
    if (hov >= 0) {
        dock_item_t *d = &dock_items[hov];
        int tw = (int)strlen(d->label) * 8 + 12;
        int tx = DOCK_W + 4, ty = d->y + DOCK_ICON_SZ / 2 - 8;
        gui_draw_shadow(tx, ty, tw, 16, 80);
        vga_draw_rect(tx, ty, tw, 16, COL_POPUP_BG);
        vga_draw_hline(tx, ty, tw, COL_DGRAY);
        vga_draw_hline(tx, ty + 15, tw, COL_DGRAY);
        vga_draw_vline(tx, ty, 16, COL_DGRAY);
        vga_draw_vline(tx + tw - 1, ty, 16, COL_DGRAY);
        vga_draw_string(tx + 6, ty + 4, d->label, COL_MENUTEXT);
    }
}

/* ===== Desktop ===== */
void gui_draw_desktop(void)
{
    /* wallpaper: vertical gradient built from blend-palette bands (light
     * theme fades toward white at the bottom, dark theme toward black). */
    const int bands = 8;
    int ys = 0, total = SCREEN_H;
    for (int b = 0; b < bands; b++) {
        int y0 = (total * b) / bands, y1 = (total * (b + 1)) / bands;
        unsigned char c = g_theme_light ? vga_blend_color(COL_DESKTOP, COL_WHITE, b * 12)
                                        : vga_blend_color(COL_DESKTOP, COL_BLACK, b * 18);
        vga_draw_rect(0, y0, SCREEN_W, y1 - y0, c);
        ys = y1;
    }
    (void)ys;

    if (g_show_desktop_icons) {
        for (int i = 0; i < desk_icon_count; i++) {
            if (desk_icons[i].visible)
                gui_draw_desktop_icon(&desk_icons[i]);
        }
    }
}

/* desk_icon_t.color carries the ICON_* kind; label is centred under the
 * tile with a 1px shadow that is offset from the SAME origin as the text
 * (the old label strip was a separate rectangle that did not line up). */
void gui_draw_desktop_icon(const desk_icon_t *icon)
{
    if (!icon) return;
    gui_draw_shadow(icon->x, icon->y, DESK_ICON_SZ, DESK_ICON_SZ, 60);
    gui_draw_app_icon((int)icon->color, icon->x, icon->y, DESK_ICON_SZ);

    int tw = (int)strlen(icon->label) * 8;
    int tx = icon->x + (DESK_ICON_SZ - tw) / 2;
    int ty = icon->y + DESK_ICON_SZ + 5;
    gui_draw_text_shadow(tx, ty, (char *)icon->label, COL_LABEL_TEXT, COL_LABEL_SHADOW);

    if (icon->win_id >= 0 && icon->win_id == active_window &&
        (windows[icon->win_id].flags & WIN_VISIBLE) &&
        !(windows[icon->win_id].flags & WIN_MINIMIZED)) {
        vga_draw_rect(icon->x + DESK_ICON_SZ / 2 - 2, ty + 11, 4, 2, COL_LABEL_TEXT);
    }
}

/* ===== Cursor ===== */
void gui_draw_cursor(int x, int y)
{
    static const unsigned char cur[8][8] = {
        {2,0,0,0,0,0,0,0},
        {2,2,0,0,0,0,0,0},
        {2,1,2,0,0,0,0,0},
        {2,1,1,2,0,0,0,0},
        {2,1,1,1,2,0,0,0},
        {2,1,2,2,0,0,0,0},
        {2,2,0,2,2,0,0,0},
        {0,0,0,0,2,0,0,0},
    };
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if      (cur[r][c] == 2) vga_put_pixel(x+c, y+r, COL_BLACK);
            else if (cur[r][c] == 1) vga_put_pixel(x+c, y+r, COL_WHITE);
}

/* ===== Window init ===== */
void gui_window_init(int id, int x, int y, int w, int h,
                     const char *title, unsigned char flags)
{
    if (id < 0 || id >= MAX_WINDOWS) return;
    gui_window_t *win = &windows[id];
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->flags = (unsigned char)(flags | WIN_VISIBLE);
    win->drag_ox = win->drag_oy = 0;
    win->saved_x = x; win->saved_y = y;
    win->saved_w = w; win->saved_h = h;
    int ti = 0;
    while (title[ti] && ti < 31) { win->title[ti] = title[ti]; ti++; }
    win->title[ti] = 0;
    win_maximized[id] = 0;
}

/* ===== Window focus =====
 * Single source of truth for "who owns keyboard focus + is on top".
 * Every click path (button row, titlebar, body, dock) funnels through
 * this function so active_window and z_order can never drift apart. */
void gui_window_focus(int id)
{
    if (id < 0 || id >= MAX_WINDOWS) return;
    if (!(windows[id].flags & WIN_VISIBLE)) return;
    if (  windows[id].flags & WIN_MINIMIZED) return;
    active_window = id;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (i == id) windows[i].flags |=  WIN_ACTIVE;
        else         windows[i].flags &= (unsigned char)~WIN_ACTIVE;
    }
    gui_window_raise(id);
    g_terminal.redraw_needed = 1;
}

/* ===== DAY43 PRE-3: dynamic window table helpers ===== */
terminal_t *gui_terminal_of(int id)
{
    if (id == WIN_TERMINAL) return &g_terminal;
    if (id >= GUI_EXTRA_FIRST && id < MAX_WINDOWS && win_alive[id] &&
        (windows[id].flags & WIN_HAS_TERM))
        return &g_extra_term[id - GUI_EXTRA_FIRST];
    return (terminal_t *)0;
}

int gui_window_count(void)
{
    int n = 0;
    for (int i = 0; i < z_order_count; i++) {
        int id = z_order[i];
        if (id >= 0 && id < MAX_WINDOWS && (windows[id].flags & WIN_VISIBLE)) n++;
    }
    return n;
}

/* Focus the front-most visible, non-minimized window (z_order top->bottom).
 * If there is none, active_window becomes -1 (nobody has the keyboard). */
void gui_focus_topmost(void)
{
    for (int zi = z_order_count - 1; zi >= 0; zi--) {
        int id = z_order[zi];
        if (id < 0 || id >= MAX_WINDOWS) continue;
        if (!(windows[id].flags & WIN_VISIBLE)) continue;
        if (  windows[id].flags & WIN_MINIMIZED) continue;
        gui_window_focus(id);
        return;
    }
    active_window = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].flags &= (unsigned char)~WIN_ACTIVE;
    g_terminal.redraw_needed = 1;
}

/* Remove an extra (dynamic) window completely and free its slot. */
static void gui_window_destroy(int id)
{
    if (id < GUI_EXTRA_FIRST || id >= MAX_WINDOWS || !win_alive[id]) return;
    int pos = -1;
    for (int i = 0; i < z_order_count; i++) if (z_order[i] == id) { pos = i; break; }
    if (pos >= 0) {
        for (int i = pos; i < z_order_count - 1; i++) z_order[i] = z_order[i + 1];
        z_order_count--;
    }
    win_alive[id]     = 0;
    win_maximized[id] = 0;
    windows[id].flags = 0;
    if (drag_win   == id) drag_win   = -1;
    if (resize_win == id) resize_win = -1;
    if (active_window == id) gui_focus_topmost();
    g_terminal.redraw_needed = 1;
}

int gui_window_new_terminal(void)
{
    if (!g_gui_mode) return -1;
    int id = -1;
    for (int i = GUI_EXTRA_FIRST; i < MAX_WINDOWS; i++)
        if (!win_alive[i]) { id = i; break; }
    if (id < 0 || z_order_count >= MAX_WINDOWS) return -1;

    int n  = id - GUI_EXTRA_FIRST;
    int k  = n % 6;                                  /* cascade offset */
    int w  = 420, h = 240;
    int x  = DESK_X + 30 + 24 * k;
    int y  = DESK_Y + 24 + 20 * k;
    if (x + w > SCREEN_W) x = SCREEN_W - w;
    if (y + h > SCREEN_H) y = SCREEN_H - h;

    char title[16] = "Terminal  ";
    title[9] = (char)('0' + (n + 2));                /* "Terminal 2".. */
    title[10] = 0;
    gui_window_init(id, x, y, w, h, title, WIN_HAS_TERM);
    terminal_init(&g_extra_term[n]);
    terminal_puts(&g_extra_term[n], "LumaOS 5.0 - ", TERM_COL_INFO);
    terminal_puts(&g_extra_term[n], title, TERM_COL_INFO);
    terminal_newline(&g_extra_term[n]);
    terminal_newline(&g_extra_term[n]);

    win_alive[id] = 1;
    z_order[z_order_count++] = id;
    gui_window_focus(id);
    g_terminal.redraw_needed = 1;
    return id;
}

/* gui_fill_body — kept for window2.c / terminal_render. PRE-4: window
 * CONTENT is always opaque now (translucency moved to the title bar). */
void gui_fill_body(int id, int x, int y, int w, int h, unsigned char color)
{
    (void)id;
    vga_draw_rect(x, y, w, h, color);
}

/* ===== window chrome (shared by real windows and the modal write window) =====
 * Order matters for translucency: shadow, then the (opaque) body BELOW the
 * title bar, then the title bar blended over whatever is behind the window. */
static void gui_draw_chrome(int ax, int ay, int aw, int ah, const char *title,
                            int active, int maximized, int grip)
{
    if (!maximized)
        gui_draw_shadow(ax, ay, aw, ah, active ? 90 : 55);

    /* body (everything under the title bar) */
    vga_draw_rect(ax, ay + TITLEBAR_H - 1, aw, ah - (TITLEBAR_H - 1), COL_WIN_BODY);

    /* title bar: translucent when Window Opacity Sim is ON */
    unsigned char tc = active ? (unsigned char)COL_WIN_TITLE
                              : (unsigned char)COL_WIN_INACT;
    gui_title_fill(ax + 1, ay + 1, aw - 2, TITLEBAR_H - 2, tc);

    /* border */
    unsigned char bc = active ? (unsigned char)COL_WIN_BORDER
                              : (unsigned char)COL_DGRAY;
    vga_draw_hline(ax,      ay,      aw, bc);
    vga_draw_hline(ax,      ay+ah-1, aw, bc);
    vga_draw_vline(ax,      ay,      ah, bc);
    vga_draw_vline(ax+aw-1, ay,      ah, bc);

    /* title text (always solid) */
    unsigned char txt_col = active ? (unsigned char)COL_TEXT_PRIMARY
                                   : (unsigned char)COL_TEXT_SECONDARY;
    vga_draw_string(ax+4, ay+3, (char *)title, txt_col);

    /* ---- buttons (top-right) ---- */
    int bx = ax + aw - 2;
    bx -= WIN_BTN_W;
    int clx = bx, cly = ay + 2;
    vga_draw_rect(clx, cly, WIN_BTN_W, WIN_BTN_H,
                  active ? (unsigned char)COL_BTN_CLOSE : (unsigned char)COL_DGRAY);
    if (active) {
        vga_put_pixel(clx+2,cly+2,COL_WHITE); vga_put_pixel(clx+6,cly+2,COL_WHITE);
        vga_put_pixel(clx+3,cly+3,COL_WHITE); vga_put_pixel(clx+5,cly+3,COL_WHITE);
        vga_put_pixel(clx+4,cly+4,COL_WHITE);
        vga_put_pixel(clx+3,cly+5,COL_WHITE); vga_put_pixel(clx+5,cly+5,COL_WHITE);
        vga_put_pixel(clx+2,cly+6,COL_WHITE); vga_put_pixel(clx+6,cly+6,COL_WHITE);
    }

    bx -= WIN_BTN_W + 1;
    int mxbx = bx;
    unsigned char btn_mx = active ? (unsigned char)COL_BTN_MAX : (unsigned char)COL_DGRAY;
    if (maximized && active) {
        vga_draw_rect(mxbx, cly, WIN_BTN_W, WIN_BTN_H, COL_BTN_MAX);
        vga_draw_hline(mxbx+2, cly+2, 5, COL_WHITE);
        vga_draw_hline(mxbx+2, cly+5, 5, COL_WHITE);
        vga_draw_vline(mxbx+2, cly+2, 4, COL_WHITE);
        vga_draw_vline(mxbx+6, cly+2, 4, COL_WHITE);
        vga_draw_hline(mxbx+4, cly+3, 4, COL_WHITE);
    } else {
        vga_draw_rect(mxbx, cly, WIN_BTN_W, WIN_BTN_H, btn_mx);
        if (active) {
            vga_draw_hline(mxbx+2, cly+2, WIN_BTN_W-4, COL_WHITE);
            vga_draw_hline(mxbx+2, cly+WIN_BTN_H-3, WIN_BTN_W-4, COL_WHITE);
            vga_draw_vline(mxbx+2, cly+2, WIN_BTN_H-4, COL_WHITE);
            vga_draw_vline(mxbx+WIN_BTN_W-3, cly+2, WIN_BTN_H-4, COL_WHITE);
        }
    }

    bx -= WIN_BTN_W + 1;
    int mnbx = bx;
    vga_draw_rect(mnbx, cly, WIN_BTN_W, WIN_BTN_H,
                  active ? (unsigned char)COL_BTN_MIN : (unsigned char)COL_DGRAY);
    if (active)
        vga_draw_hline(mnbx+2, cly+WIN_BTN_H-3, WIN_BTN_W-4, COL_WHITE);

    /* resize grip (real windows only) */
    if (grip && !maximized) {
        int gx = ax + aw - RESIZE_HANDLE_SZ;
        int gy = ay + ah - RESIZE_HANDLE_SZ;
        vga_draw_rect(gx, gy, RESIZE_HANDLE_SZ, RESIZE_HANDLE_SZ, COL_RESIZE_HANDLE);
        for (int i = 1; i < RESIZE_HANDLE_SZ; i += 2)
            vga_put_pixel(gx + i, gy + RESIZE_HANDLE_SZ - 1 - i, COL_WIN_BORDER);
    }
}

/* Which title-bar button is at (x,y) of a window at (wx,wy) width ww?
 * 0 = none, 1 = close, 2 = maximize, 3 = minimize. */
static int chrome_button_at(int x, int y, int wx, int wy, int ww)
{
    int bx_right = wx + ww - 2;
    int clx  = bx_right - WIN_BTN_W;
    int mxbx = clx  - WIN_BTN_W - 1;
    int mnbx = mxbx - WIN_BTN_W - 1;
    if (y < wy + 2 || y >= wy + 2 + WIN_BTN_H) return 0;
    if (x >= clx  && x < clx  + WIN_BTN_W) return 1;
    if (x >= mxbx && x < mxbx + WIN_BTN_W) return 2;
    if (x >= mnbx && x < mnbx + WIN_BTN_W) return 3;
    return 0;
}

/* ===== Window draw ===== */
void gui_window_draw(int id)
{
    if (id < 0 || id >= MAX_WINDOWS) return;
    gui_window_t *w = &windows[id];
    if (!(w->flags & WIN_VISIBLE)) return;
    if (  w->flags & WIN_MINIMIZED) return;

    int ax = w->x, ay = w->y, aw = w->w, ah = w->h;
    gui_draw_chrome(ax, ay, aw, ah, w->title, id == active_window, win_maximized[id], 1);

    /* Terminal content area */
    if (w->flags & WIN_HAS_TERM) {
        terminal_t *tt = gui_terminal_of(id);
        if (tt) {
            g_render_win = id;
            terminal_render(tt, ax, ay, aw, ah);
        }
    }

    /* Settings content drawn by window2 */
    if (id == WIN_SETTINGS)
        gui_window2_settings_draw();

    /* Info content drawn by window2 -- inside the z-order-respecting
     * per-window pass, so it can never paint over a window above it. */
    if (id == WIN_INFO)
        gui_window2_info_draw();
}

/* ===== terminal_init ===== */
void terminal_init(terminal_t *t)
{
    for (int i = 0; i < TERM_BUF_ROWS; i++) {
        for (int j = 0; j < TERM_BUF_COLS; j++) {
            t->lines[i].text[j]  = ' ';
            t->lines[i].color[j] = TERM_COL_DEFAULT;
        }
        t->lines[i].len = 0;
    }
    t->total_lines   = 1;
    t->scroll_offset = 0;
    t->cur_col       = 0;
    t->input_len     = 0;
    t->input_cursor  = 0;
    t->history_count = 0;
    t->history_pos   = -1;
    t->cursor_blink  = 0;
    t->redraw_needed = 1;
    for (int i = 0; i <= TERM_INPUT_MAX; i++) t->input[i] = 0;
    for (int i = 0; i < TERM_HISTORY; i++)
        for (int j = 0; j < TERM_HIST_LEN; j++)
            t->history[i][j] = 0;
}

/* ===== terminal_newline (FIX DRV-1) ===== */
void terminal_newline(terminal_t *t)
{
    t->cur_col = 0;
    t->total_lines++;

    if (t->total_lines >= TERM_BUF_ROWS) {
        for (int i = 0; i < TERM_BUF_ROWS - 1; i++) {
            for (int j = 0; j < TERM_BUF_COLS; j++) {
                t->lines[i].text[j]  = t->lines[i+1].text[j];
                t->lines[i].color[j] = t->lines[i+1].color[j];
            }
            t->lines[i].len = t->lines[i+1].len;
        }
        int last = TERM_BUF_ROWS - 1;
        for (int j = 0; j < TERM_BUF_COLS; j++) {
            t->lines[last].text[j]  = ' ';
            t->lines[last].color[j] = TERM_COL_DEFAULT;
        }
        t->lines[last].len = 0;
        t->total_lines = TERM_BUF_ROWS;
    }
    t->redraw_needed = 1;
}

void terminal_putchar(terminal_t *t, char c, unsigned char color)
{
    if (c == '\n') { terminal_newline(t); return; }
    if (c == '\r') { t->cur_col = 0; return; }
    if (c == '\b') {
        if (t->cur_col > 0) {
            t->cur_col--;
            int row = t->total_lines - 1;
            t->lines[row].text[t->cur_col]  = ' ';
            t->lines[row].color[t->cur_col] = TERM_COL_DEFAULT;
            if (t->lines[row].len > t->cur_col)
                t->lines[row].len = t->cur_col;
        }
        t->redraw_needed = 1; return;
    }
    if (c == '\t') {
        int nxt = (t->cur_col + 4) & ~3;
        while (t->cur_col < nxt && t->cur_col < TERM_BUF_COLS - 1)
            terminal_putchar(t, ' ', color);
        return;
    }
    int row = t->total_lines - 1;
    if (t->cur_col >= TERM_BUF_COLS - 1) {
        terminal_newline(t); row = t->total_lines - 1;
    }
    t->lines[row].text[t->cur_col]  = c;
    t->lines[row].color[t->cur_col] = color;
    t->cur_col++;
    if (t->cur_col > t->lines[row].len) t->lines[row].len = t->cur_col;
    t->redraw_needed = 1;
}

void terminal_puts(terminal_t *t, const char *s, unsigned char color)
{
    while (*s) terminal_putchar(t, *s++, color);
}

void terminal_clear(terminal_t *t)
{
    for (int i = 0; i < TERM_BUF_ROWS; i++) {
        for (int j = 0; j < TERM_BUF_COLS; j++) {
            t->lines[i].text[j]  = ' ';
            t->lines[i].color[j] = TERM_COL_DEFAULT;
        }
        t->lines[i].len = 0;
    }
    t->total_lines = 1; t->scroll_offset = 0; t->cur_col = 0;
    t->redraw_needed = 1;
}

/* ===== terminal_scroll (NEW — wheel support) =====
 * delta_lines > 0 scrolls back into history (like PGUP),
 * delta_lines < 0 scrolls toward the live prompt (like PGDN). */
void terminal_scroll(terminal_t *t, int delta_lines)
{
    t->scroll_offset += delta_lines;
    int mx = t->total_lines - 1;
    if (mx < 0) mx = 0;
    if (t->scroll_offset > mx) t->scroll_offset = mx;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    t->redraw_needed = 1;
}

/* gui_build_prompt (DAY43): "IDE3/docs/ > " — same shape as kmain.c's
 * text_shell_prompt(), shared by the input row and the echoed command. */
static void gui_build_prompt(char *prompt)
{
    int n = 0;
    const char *mn = (vfs_root && vfs_root->fs) ? vfs_mount_name_of(vfs_root->fs) : (const char *)0;
    const char *cwd = shell_get_cwd();
    const char *seg = mn ? mn : "LumaOS";
    while (*seg && n < 12) prompt[n++] = *seg++;
    prompt[n++] = '/';
    if (cwd[0]) {
        int k = 0;
        while (cwd[k] && n < 30) prompt[n++] = cwd[k++];
        prompt[n++] = '/';
    }
    prompt[n++] = ' '; prompt[n++] = '>'; prompt[n++] = ' ';
    prompt[n] = 0;
}

/* ===== terminal_render =====
 *
 * DAY43 PRE-2 (②): "GUI 模式的 bash 能不能不让输入命令的在最下面，让它像
 * ubuntu bash / powershell / 这个系统的非 GUI 模式一样呢"
 *
 * BEFORE: the scrollback was drawn bottom-anchored and the prompt + input
 * line lived in a SEPARATE strip permanently glued to the bottom of the
 * window (separator line + input row), so with 3 lines of output the
 * prompt still sat ~30 rows below them.
 *
 * NOW: the input line is just the LAST LINE of the terminal, exactly as in
 * a real shell and in text_shell_run():
 *
 *     LumaOS 5.0
 *     IDE3/ > ls
 *     kday38.txt   0 bytes
 *     IDE3/ > _            <- directly under the last output line
 *
 * How it is built (nothing new is stored, it is a render-time layout):
 *
 *   - lines[0 .. total_lines-1] is the history, as before. The last entry
 *     lines[total_lines-1] is the "current" line; when a command finished
 *     (cur_col == 0 and it is empty) it is exactly where the next prompt
 *     goes. If a command left a half line behind (len > 0) the live line
 *     starts on the row after it.
 *   - The live line = prompt + t->input. It is laid out in the same
 *     visible_cols grid as the history, WRAPPING when it is longer than a
 *     row (up to 118 typed chars + prompt > 92 columns), so it occupies
 *     live_rows rows.
 *   - virtual_rows = base + live_rows (base = rows above the live line).
 *     The window shows the last visible_rows of it, shifted up by
 *     scroll_offset (wheel / PgUp). Until the screen is full that means
 *     "start at the top and grow downward" (the new behaviour); once full
 *     it scrolls, keeping the prompt on the last row (like bash).
 *   - The cursor is a 2 px bar at (prompt_len + input_cursor) in that grid,
 *     so LEFT/RIGHT/HOME/END and mid-line insertion still show up right.
 *
 * The old separator line / reserved 14 px strip are gone: the text area
 * now uses the whole client rectangle. */
void terminal_render(terminal_t *t, int wx, int wy, int ww, int wh)
{
    int cx = wx + 2;
    int cy = wy + TITLEBAR_H + 1;
    int cw = ww - 6;
    int ch = wh - TITLEBAR_H - 4;
    if (cw <= 0 || ch <= 0) return;

    gui_fill_body(g_render_win, cx, cy, cw, ch, COL_TERM_BG);   /* PRE-3: opacity-aware */

    int visible_rows = ch / 9;
    int visible_cols = cw / 8;
    if (visible_rows < 1 || visible_cols < 2) return;

    /* ---- live line (prompt + input) layout ---- */
    char prompt[40];
    gui_build_prompt(prompt);
    int pl       = (int)strlen(prompt);
    int live_len = pl + t->input_len;
    int live_rows = live_len / visible_cols + 1;     /* +1: room for the cursor */

    int cur_idx  = pl + t->input_cursor;
    int cur_row  = cur_idx / visible_cols;           /* cursor wraps with the text */
    int cur_col  = cur_idx % visible_cols;
    if (cur_row >= live_rows) live_rows = cur_row + 1;

    int base = t->total_lines - 1;                   /* history lines above the live line */
    if (t->lines[base].len > 0) base++;              /* unfinished output line: prompt goes below it */

    /* DAY43 PRE-3 (③): a history line longer than the window is WRAPPED
     * over several screen rows (it used to be cut at visible_cols, which
     * hid the tail of a long `echo`). hist_rows = screen rows the history
     * needs at the CURRENT window width; recomputed every frame, so it
     * stays right while the window is resized. */
    int hist_rows = 0;
    for (int v = 0; v < base; v++) {
        int len = t->lines[v].len;
        hist_rows += (len <= visible_cols) ? 1 : (len + visible_cols - 1) / visible_cols;
    }
    int vrows = hist_rows + live_rows;

    /* clamp the scroll position to what actually exists */
    int max_sc = vrows - visible_rows;
    if (max_sc < 0) max_sc = 0;
    if (t->scroll_offset > max_sc) t->scroll_offset = max_sc;

    int start = vrows - visible_rows - t->scroll_offset;
    if (start < 0) start = 0;

    int screen_row = 0;
    int r = 0;                                       /* virtual row of the line being walked */

    /* ---- history (wrapped) ---- */
    for (int v = 0; v < base && screen_row < visible_rows; v++) {
        int len = t->lines[v].len;
        int n = (len <= visible_cols) ? 1 : (len + visible_cols - 1) / visible_cols;
        if (r + n <= start) { r += n; continue; }    /* wholly above the window */
        for (int k = 0; k < n && screen_row < visible_rows; k++) {
            if (r + k < start) continue;
            int from = k * visible_cols;
            int to   = from + visible_cols;
            if (to > len) to = len;
            int dy = cy + screen_row * 9;
            for (int col = from; col < to; col++) {
                char c = t->lines[v].text[col];
                if (c >= 32 && c <= 126)
                    vga_draw_char(cx + (col - from) * 8, dy, c, t->lines[v].color[col]);
            }
            screen_row++;
        }
        r += n;
    }

    /* ---- live line (wrapped) ---- */
    for (int lr = 0; lr < live_rows && screen_row < visible_rows; lr++) {
        if (hist_rows + lr < start) continue;
        int dy   = cy + screen_row * 9;
        int from = lr * visible_cols;
        int to   = from + visible_cols;
        if (to > live_len) to = live_len;
        for (int k = from; k < to; k++) {
            char c; unsigned char colr;
            if (k < pl) { c = prompt[k];        colr = TERM_COL_PROMPT; }
            else        { c = t->input[k - pl]; colr = COL_TEXT_PRIMARY; }
            if (c >= 32 && c <= 126)
                vga_draw_char(cx + (k - from) * 8, dy, c, colr);
        }
        /* blinking cursor bar on the row it falls on */
        if (lr == cur_row && t->cursor_blink < 15 && t->scroll_offset == 0)
            vga_draw_rect(cx + cur_col * 8, dy, 2, 8, COL_TERM_CUR);
        screen_row++;
    }

    t->cursor_blink = (t->cursor_blink + 1) % 30;

    /* ---- scrollbar (over the whole virtual buffer incl. the live line) ---- */
    if (vrows > visible_rows) {
        int sbx = wx + ww - 4;
        vga_draw_rect(sbx, cy, 3, ch, COL_SCROLLBAR);
        int th = (visible_rows * ch) / vrows;
        if (th < 3) th = 3;
        int ty = cy + (start * ch) / vrows;
        if (ty + th > cy + ch) ty = cy + ch - th;
        vga_draw_rect(sbx, ty, 3, th, COL_SCROLLTHM);
    }

    t->redraw_needed = 0;
}

/* ===== terminal_handle_key ===== */
static void history_push(terminal_t *t, const char *cmd)
{
    if (!cmd[0]) return;
    int slot = t->history_count % TERM_HISTORY;
    int ci = 0;
    while (cmd[ci] && ci < TERM_HIST_LEN-1) { t->history[slot][ci]=cmd[ci]; ci++; }
    t->history[slot][ci] = 0;
    t->history_count++;
}

static void history_load(terminal_t *t)
{
    if (t->history_pos < 0) {
        t->input[0]=0; t->input_len=0; t->input_cursor=0;
        t->redraw_needed=1; return;
    }
    int idx = (t->history_count - 1 - t->history_pos) % TERM_HISTORY;
    if (idx < 0) idx += TERM_HISTORY;
    int ci = 0;
    while (t->history[idx][ci] && ci < TERM_INPUT_MAX)
        { t->input[ci]=t->history[idx][ci]; ci++; }
    t->input[ci]=0; t->input_len=ci; t->input_cursor=ci;
    t->redraw_needed=1;
}

void terminal_handle_key(terminal_t *t, unsigned char key)
{
    /* Any recognised key always forces a redraw at the end of this
     * function (see bottom) so a keypress can never be silently lost
     * even if a future edit forgets to set redraw_needed in a branch. */
    if (key == KEY_UP) {
        if (t->history_count > 0 && t->history_pos < t->history_count-1)
            { t->history_pos++; history_load(t); }
        goto done;
    }
    if (key == KEY_DOWN) {
        if      (t->history_pos > 0) { t->history_pos--; history_load(t); }
        else if (t->history_pos == 0){ t->history_pos=-1; history_load(t); }
        goto done;
    }
    if (key == KEY_LEFT) {
        if (t->input_cursor > 0) t->input_cursor--;
        goto done;
    }
    if (key == KEY_RIGHT) {
        if (t->input_cursor < t->input_len) t->input_cursor++;
        goto done;
    }
    if (key == KEY_PGUP) {
        terminal_scroll(t, 5);
        goto done;
    }
    if (key == KEY_PGDN) {
        terminal_scroll(t, -5);
        goto done;
    }
    if (key == KEY_HOME){ t->input_cursor=0; goto done; }
    if (key == KEY_END) { t->input_cursor=t->input_len; goto done; }
    if (key == KEY_DEL) {
        t->scroll_offset = 0;                 /* PRE-2: editing snaps back to the prompt */
        if (t->input_cursor < t->input_len) {
            for (int i=t->input_cursor; i<t->input_len-1; i++)
                t->input[i]=t->input[i+1];
            t->input_len--; t->input[t->input_len]=0;
        }
        goto done;
    }
    /* Ctrl+L = clear */
    if (key == KEY_CTRL_L) { terminal_clear(t); goto done; }
    /* Ctrl+U = kill line */
    if (key == KEY_CTRL_U) {
        t->input[0]=0; t->input_len=0; t->input_cursor=0;
        goto done;
    }
    /* Ctrl+A = beginning of line */
    if (key == KEY_CTRL_A) { t->input_cursor=0; goto done; }
    /* Ctrl+E = end of line */
    if (key == KEY_CTRL_E) { t->input_cursor=t->input_len; goto done; }

    if (key == '\n' || key == '\r') {
        { char pr[40]; gui_build_prompt(pr); terminal_puts(t, pr, TERM_COL_PROMPT); }
        terminal_puts(t, t->input, TERM_COL_CMD);
        terminal_newline(t);
        history_push(t, t->input);
        /* PRE-4: run a COPY and empty the input line first. A long-running
         * command (the `write` window) keeps the GUI repainting from inside
         * run_command; with the input still in place the live prompt row
         * showed the command a second time under its own echo. */
        char cmdcopy[TERM_INPUT_MAX + 1];
        { int n = 0; while (t->input[n] && n < TERM_INPUT_MAX) { cmdcopy[n] = t->input[n]; n++; } cmdcopy[n] = 0; }
        for (int i=0; i<=TERM_INPUT_MAX; i++) t->input[i]=0;
        t->input_len=0; t->input_cursor=0;
        terminal_run_command(t, cmdcopy);
        /* DAY43 PRE-2: the prompt now follows the output on the next row,
         * so a command that ended without '\n' must not leave the prompt
         * glued to its last word. */
        if (t->cur_col != 0) terminal_newline(t);
        for (int i=0; i<=TERM_INPUT_MAX; i++) t->input[i]=0;
        t->input_len=0; t->input_cursor=0;
        t->history_pos=-1; t->scroll_offset=0;
        goto done;
    }
    if (key == '\b' || key == KEY_BS) {
        t->scroll_offset = 0;
        if (t->input_cursor > 0) {
            for (int i=t->input_cursor-1; i<t->input_len-1; i++)
                t->input[i]=t->input[i+1];
            t->input_len--; t->input_cursor--;
            t->input[t->input_len]=0;
        }
        goto done;
    }
    if (key >= 32 && key <= 126 && t->input_len < TERM_INPUT_MAX) {
        t->scroll_offset = 0;
        for (int i=t->input_len; i>t->input_cursor; i--)
            t->input[i]=t->input[i-1];
        t->input[t->input_cursor]=(char)key;
        t->input_len++; t->input_cursor++;
        t->input[t->input_len]=0;
    }

done:
    t->redraw_needed = 1;
}

/* ===== gui_init ===== */
void gui_init(void)
{
    gui_set_palette();

    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].flags  = 0;
        windows[i].title[0] = 0;
        win_maximized[i]  = 0;
        win_alive[i]      = 0;
    }
    win_alive[WIN_TERMINAL] = win_alive[WIN_INFO] = win_alive[WIN_SETTINGS] = 1;

    /* Z-ORDER: initial order back→front: Settings, Info, Terminal.
     * Only the 3 fixed apps are live at start (PRE-3: extra terminals are
     * appended to z_order when created). */
    z_order[0] = WIN_SETTINGS;
    z_order[1] = WIN_INFO;
    z_order[2] = WIN_TERMINAL;
    z_order_count = 3;

    /* Window 0: Terminal — covers most of desktop */
    gui_window_init(WIN_TERMINAL,
                    DOCK_W + 4, MENUBAR_H + 4,
                    SCREEN_W - DOCK_W - 8,
                    SCREEN_H - MENUBAR_H - 8,
                    "Terminal", WIN_HAS_TERM);
    terminal_init(&g_terminal);
    terminal_puts(&g_terminal, "LumaOS 5.0\n", TERM_COL_INFO);
    terminal_puts(&g_terminal, "Type 'help' for commands.\n", TERM_COL_DEFAULT);
    terminal_newline(&g_terminal);

    /* Window 1: Info — initialised by window2, starts hidden */
    gui_window2_init();

    /* Window 2: Settings — starts hidden */
    gui_window2_settings_init();

    /* Ids GUI_EXTRA_FIRST.. are the dynamic Terminal slots (PRE-3): empty
     * until gui_window_new_terminal() fills them. */

    /* Dock items (PRE-4: 4 icons; the 4th is the "+" New Terminal button,
     * win_id = -1, with a separator above it). Labels double as tooltips. */
    static const char *dock_labels[DOCK_APPS] = { "Terminal", "Info", "Settings", "New Terminal" };
    static const int   dock_wins[DOCK_APPS]   = { WIN_TERMINAL, WIN_INFO, WIN_SETTINGS, -1 };
    for (int i = 0; i < DOCK_APPS; i++) {
        dock_items[i].app_id = i;                         /* app_id == ICON_* kind */
        dock_items[i].x      = (DOCK_W - DOCK_ICON_SZ) / 2;
        dock_items[i].y      = DOCK_ICON_Y0 + i * DOCK_ICON_STEP + (i == DOCK_APP_NEW ? 10 : 0);
        dock_items[i].win_id = dock_wins[i];
        int k = 0;
        while (dock_labels[i][k] && k < 15) { dock_items[i].label[k] = dock_labels[i][k]; k++; }
        dock_items[i].label[k] = 0;
    }

    /* Desktop icons: same four apps, stacked down the left edge. `color`
     * carries the ICON_* kind (see gui_draw_desktop_icon). */
    static const char *desk_labels[4] = { "Terminal", "Info", "Settings", "New Term" };
    static const int   desk_wins[4]   = { WIN_TERMINAL, WIN_INFO, WIN_SETTINGS, DESK_ACTION_NEWTERM };
    desk_icon_count = 0;
    for (int i = 0; i < 4; i++) {
        desk_icon_t *ic = &desk_icons[desk_icon_count++];
        ic->x       = DESK_X + 20;
        ic->y       = DESK_Y + 14 + i * (DESK_ICON_SZ + DESK_ICON_GAP + 14);
        ic->win_id  = desk_wins[i];
        ic->color   = (unsigned char)i;
        ic->visible = 1;
        int k = 0;
        while (desk_labels[i][k] && k < 15) { ic->label[k] = desk_labels[i][k]; k++; }
        ic->label[k] = 0;
    }

    /* Terminal has first focus */
    gui_window_focus(WIN_TERMINAL);

    prev_mx=mouse_x; prev_my=mouse_y; prev_ml=0; prev_mr=0;
    drag_win=-1; resize_win=-1; popup_menu=0;

    shell_init();

    g_gui_mode = 1;   /* DAY43: from here on, screen output must go through the GUI */

    /* gui_init() does not loop itself — kmain's fallback loop calls
     * gui_refresh() repeatedly. This is intentional (keeps gui_init
     * a plain setup function), but is documented here explicitly so
     * it's never mistaken for a bug: if kmain's loop is ever removed,
     * a `for(;;) gui_refresh();` must be added at the end of this
     * function instead. */
}

/* ===== gui_draw_frame (DAY43: factored out of gui_refresh) ===== */

/* ---- modal window state (the `write` editor / its Y/N dialog) ----
 * A modal is a real window as far as the user can tell: title bar with
 * [_][ ][x], drag by the title, maximize/restore, minimize to a tab at the
 * bottom-left of the desktop. It is not in windows[]/z_order[] because its
 * owner (cmd_write) is blocked inside ONE shell_exec() call and drives it
 * through gui_modal_pump(); everything about it lives in this block. */
static int   modal_on = 0;
static int   modal_x, modal_y, modal_w, modal_h;
static int   modal_max = 0, modal_min = 0, modal_close_req = 0;
static int   modal_sx, modal_sy, modal_sw, modal_sh;      /* geometry to restore */
static int   modal_drag = 0, modal_dox = 0, modal_doy = 0;
static char  modal_title[48];
static void (*g_modal_draw)(int, int, int, int) = (void (*)(int,int,int,int))0;

#define MODAL_CHIP_H 16
static void modal_chip_rect(int *x, int *y, int *w, int *h)
{
    int n = (int)strlen(modal_title);
    if (n > 30) n = 30;
    *w = n * 8 + 28; *h = MODAL_CHIP_H;
    *x = DESK_X + 8; *y = SCREEN_H - MODAL_CHIP_H - 6;
}

void gui_modal_open(const char *title, int w, int h,
                    void (*draw)(int cx, int cy, int cw, int ch))
{
    if (w > DESK_W) w = DESK_W;
    if (h > DESK_H) h = DESK_H;
    modal_w = w; modal_h = h;
    modal_x = DESK_X + (DESK_W - w) / 2;
    modal_y = DESK_Y + (DESK_H - h) / 2;
    modal_max = modal_min = modal_close_req = modal_drag = 0;
    int i = 0;
    while (title[i] && i < 47) { modal_title[i] = title[i]; i++; }
    modal_title[i] = 0;
    g_modal_draw = draw;
    modal_on = 1;
    g_terminal.redraw_needed = 1;
}

void gui_modal_close(void)
{
    modal_on = 0; modal_max = modal_min = modal_close_req = modal_drag = 0;
    g_modal_draw = (void (*)(int,int,int,int))0;
    g_terminal.redraw_needed = 1;
}

int gui_modal_active(void)    { return modal_on; }
int gui_modal_minimized(void) { return modal_on && modal_min; }
int gui_modal_take_close(void){ int r = modal_close_req; modal_close_req = 0; return r; }

static void modal_toggle_max(void)
{
    if (!modal_max) {
        modal_sx = modal_x; modal_sy = modal_y; modal_sw = modal_w; modal_sh = modal_h;
        modal_x = DESK_X; modal_y = DESK_Y; modal_w = DESK_W; modal_h = DESK_H;
        modal_max = 1;
    } else {
        modal_x = modal_sx; modal_y = modal_sy; modal_w = modal_sw; modal_h = modal_sh;
        modal_max = 0;
    }
}

/* Returns 1 when the modal consumed the mouse event. While it is
 * minimized only its tab is claimed, the rest of the desktop stays usable. */
static int modal_mouse(int x, int y, int left, int click)
{
    if (!modal_on) return 0;

    if (modal_drag) {
        if (left) {
            int nx = x - modal_dox, ny = y - modal_doy;
            if (nx < DESK_X) nx = DESK_X;
            if (ny < DESK_Y) ny = DESK_Y;
            if (nx + modal_w > SCREEN_W)  nx = SCREEN_W  - modal_w;
            if (ny + modal_h > SCREEN_H)  ny = SCREEN_H  - modal_h;
            modal_x = nx; modal_y = ny;
            g_terminal.redraw_needed = 1;
            return 1;
        }
        modal_drag = 0;
    }

    if (modal_min) {
        int cx, cy, cw, ch; modal_chip_rect(&cx, &cy, &cw, &ch);
        if (click && x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
            modal_min = 0;
            g_terminal.redraw_needed = 1;
            return 1;
        }
        return 0;
    }

    if (click && x >= modal_x && x < modal_x + modal_w &&
                 y >= modal_y && y < modal_y + modal_h) {
        int b = chrome_button_at(x, y, modal_x, modal_y, modal_w);
        if (b == 1)      modal_close_req = 1;
        else if (b == 2) modal_toggle_max();
        else if (b == 3) modal_min = 1;
        else if (y < modal_y + TITLEBAR_H) {
            if (modal_max) {                       /* drag a maximized window: restore under the cursor */
                int rw = modal_sw, rh = modal_sh;
                int rx = x - rw / 2;
                if (rx < DESK_X) rx = DESK_X;
                if (rx + rw > SCREEN_W) rx = SCREEN_W - rw;
                modal_x = rx; modal_y = y - TITLEBAR_H / 2;
                if (modal_y < DESK_Y) modal_y = DESK_Y;
                modal_w = rw; modal_h = rh; modal_max = 0;
            }
            modal_drag = 1; modal_dox = x - modal_x; modal_doy = y - modal_y;
        }
        g_terminal.redraw_needed = 1;
    }
    return 1;       /* modal: swallow everything else (clicks outside, moves) */
}

static void modal_draw(void)
{
    if (!modal_on) return;
    if (modal_min) {
        int cx, cy, cw, ch; modal_chip_rect(&cx, &cy, &cw, &ch);
        int hot = (mouse_x >= cx && mouse_x < cx + cw && mouse_y >= cy && mouse_y < cy + ch);
        gui_draw_shadow(cx, cy, cw, ch, 70);
        vga_draw_rect(cx, cy, cw, ch, hot ? COL_MENU_HILITE : COL_POPUP_BG);
        vga_draw_hline(cx, cy, cw, COL_DGRAY);
        vga_draw_hline(cx, cy + ch - 1, cw, COL_DGRAY);
        vga_draw_vline(cx, cy, ch, COL_DGRAY);
        vga_draw_vline(cx + cw - 1, cy, ch, COL_DGRAY);
        vga_draw_rect(cx + 5, cy + 5, 6, 6, COL_BTN_MIN);            /* "minimized" marker */
        char buf[34]; int n = 0;
        while (modal_title[n] && n < 30) { buf[n] = modal_title[n]; n++; }
        buf[n] = 0;
        vga_draw_string(cx + 18, cy + 4, buf, hot ? COL_MENU_HILITE_TEXT : COL_MENUTEXT);
        return;
    }
    gui_draw_chrome(modal_x, modal_y, modal_w, modal_h, modal_title, 1, modal_max, 0);
    if (g_modal_draw)
        g_modal_draw(modal_x + 2, modal_y + TITLEBAR_H + 1,
                     modal_w - 4, modal_h - TITLEBAR_H - 3);
}

static void gui_draw_frame(int mx, int my)
{
    gui_draw_desktop();
    gui_draw_dock();

    /* Draw windows in Z-ORDER: index 0 = back, last = front.
     * Only the live z_order_count entries are ever drawn. */
    for (int i = 0; i < z_order_count; i++) {
        int wid = z_order[i];
        if (wid >= 0 && wid < MAX_WINDOWS)
            gui_window_draw(wid);
    }

    gui2_draw_overlay();
    gui_draw_menubar();
    modal_draw();                          /* DAY43: modal window / its tab on top */
    gui_draw_cursor(mx, my);
    vga_flip();

    /* PRE-3: the frame is up to date — clear EVERY terminal's dirty flag.
     * (terminal_render() only clears the one it draws; with the main
     * Terminal closed, or extra terminals alive, stale flags would force a
     * repaint every single frame.) */
    g_terminal.redraw_needed = 0;
    for (int i = 0; i < MAX_WINDOWS - GUI_EXTRA_FIRST; i++)
        g_extra_term[i].redraw_needed = 0;
}

/* One poll of the mouse: dispatch to gui_handle_mouse() when anything changed.
 * Shared by gui_refresh() and gui_modal_pump() so the modal window can be
 * dragged while `write` is blocked waiting for a key. */
static void gui_poll_mouse(void)
{
    int mx=mouse_x, my=mouse_y, ml=mouse_left_button, mr=mouse_right_button;
    if (mx!=prev_mx || my!=prev_my || ml!=prev_ml || mr!=prev_mr) {
        gui_handle_mouse(mx, my, ml, mr);
        prev_mx=mx; prev_my=my; prev_ml=ml; prev_mr=mr;
    }
}

/* ===== gui_refresh ===== */
void gui_refresh(void)
{
    unsigned char key = keyboard_read();
    if (key) gui_handle_key(key);

    gui_poll_mouse();
    int mx = mouse_x, my = mouse_y;

    /* Scroll wheel: poll accumulated delta and dispatch once per frame */
    int wheel = mouse_consume_wheel();
    if (wheel != 0) {
        gui_handle_wheel(mx, my, wheel);
    }

    gui2_tick();

    static int frame=0; frame++;
    int dirty = g_terminal.redraw_needed;
    for (int i = 0; i < MAX_WINDOWS - GUI_EXTRA_FIRST; i++)   /* output of an extra terminal */
        if (win_alive[GUI_EXTRA_FIRST + i] && g_extra_term[i].redraw_needed) dirty = 1;
    if (dirty || modal_on || (frame % 15 == 0)) {
        gui_draw_frame(mx, my);
    }
}

/* One GUI-loop iteration WITHOUT key dispatch: the modal owner reads the
 * keyboard itself, but the mouse keeps working (modal window drag / max /
 * min, the desktop while the modal is minimized), the tick code keeps
 * running, and the frame (desktop + windows + modal) is repainted. */
void gui_modal_pump(void)
{
    gui_poll_mouse();
    gui2_tick();
    gui_draw_frame(mouse_x, mouse_y);
}

/* ===== gui_handle_key ===== */
void gui_handle_key(unsigned char key)
{
    if (popup_menu) {
        popup_menu=0;
        g_terminal.redraw_needed=1;
    }

    /* Dispatch to focused window. active_window is the single source
     * of truth for keyboard focus (kept in sync with z_order top by
     * gui_window_focus()), so this routing can never point at a
     * window that isn't actually the visible top window. */
    terminal_t *act_t = (active_window >= 0) ? gui_terminal_of(active_window) : (terminal_t *)0;
    if (act_t)
        terminal_handle_key(act_t, key);        /* PRE-3: main OR extra terminal */
    else if (active_window == WIN_INFO)
        gui_window2_key(key);
    else if (active_window == WIN_SETTINGS)
        gui_window2_settings_key(key);

    /* Defensive: guarantee a redraw happens even if a handler above
     * (now or in the future) forgets to set the flag itself. */
    g_terminal.redraw_needed = 1;
}

/* ===== gui_handle_wheel (NEW) =====
 * Routes a scroll-wheel notch count to whichever window is under the
 * cursor. Positive delta = scroll back into history / up; negative =
 * scroll toward the newest content / down (matches PGUP/PGDN mapping
 * already used by terminal_handle_key and window2's list scrolling). */
void gui_handle_wheel(int x, int y, int wheel_delta)
{
    if (wheel_delta == 0) return;
    /* Normalise raw hardware notches into +-1 "lines" worth of scroll
     * per call so fast spins don't overshoot wildly; callers that want
     * to scroll multiple lines per notch can multiply inside their own
     * scroll function. Sign convention: hardware reports negative for
     * "scroll up/away" on most IntelliMouse-compatible devices, which
     * we want to map to *scrolling back into history* (positive lines
     * for terminal_scroll / window2 scroll helpers). */
    int lines = (wheel_delta < 0) ? 3 : -3;

    /* Ignore wheel events over the menu bar / dock — nothing scrolls there */
    if (y < MENUBAR_H || x < DOCK_W) return;

    /* Find topmost visible, non-minimized window under the cursor */
    for (int zi = z_order_count - 1; zi >= 0; zi--) {
        int i = z_order[zi];
        if (i < 0 || i >= MAX_WINDOWS) continue;
        if (!(windows[i].flags & WIN_VISIBLE)) continue;
        if (  windows[i].flags & WIN_MINIMIZED) continue;
        gui_window_t *w = &windows[i];
        if (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h) {
            terminal_t *wt = gui_terminal_of(i);
            if (wt && (w->flags & WIN_HAS_TERM)) {
                terminal_scroll(wt, lines);
            } else if (i == WIN_INFO) {
                gui_window2_scroll(lines < 0 ? 1 : -1);
            } else if (i == WIN_SETTINGS) {
                gui_window2_settings_scroll(lines < 0 ? 1 : -1);
            }
            g_terminal.redraw_needed = 1;
            return;
        }
    }
}

/* ===== gui_menu_open_window =====
 * Shared "open, un-minimize, or just focus" helper for every fixed UI
 * element that targets a window id (dock icon, desktop icon, menu item). */
static void gui_menu_open_window(int wid)
{
    if (wid < 0 || wid >= MAX_WINDOWS) return;
    if (!(windows[wid].flags & WIN_VISIBLE)) {
        windows[wid].flags |= WIN_VISIBLE;
        windows[wid].flags &= (unsigned char)~WIN_MINIMIZED;
        gui_window_focus(wid);
    } else if (windows[wid].flags & WIN_MINIMIZED) {
        windows[wid].flags &= (unsigned char)~WIN_MINIMIZED;
        gui_window_focus(wid);
    } else {
        gui_window_focus(wid);
    }
    g_terminal.redraw_needed = 1;
}

/* ===== window operations shared by the title-bar buttons and the App menu ===== */
static void gui_close_window(int i)
{
    if (i < 0 || i >= MAX_WINDOWS) return;
    if (i >= GUI_EXTRA_FIRST) { gui_window_destroy(i); return; }   /* PRE-3: dynamic window */
    windows[i].flags &= (unsigned char)~WIN_VISIBLE;
    if (resize_win == i) resize_win = -1;
    if (drag_win   == i) drag_win   = -1;
    if (active_window == i) gui_focus_topmost();
    g_terminal.redraw_needed = 1;
}

static void gui_minimize_window(int i)
{
    if (i < 0 || i >= MAX_WINDOWS) return;
    windows[i].flags |= WIN_MINIMIZED;
    if (resize_win == i) resize_win = -1;
    if (drag_win   == i) drag_win   = -1;
    if (active_window == i) gui_focus_topmost();
    g_terminal.redraw_needed = 1;
}

static void gui_toggle_maximize(int i)
{
    if (i < 0 || i >= MAX_WINDOWS) return;
    gui_window_t *w = &windows[i];
    if (!win_maximized[i]) {
        win_saved_x[i]=w->x; win_saved_y[i]=w->y;
        win_saved_w[i]=w->w; win_saved_h[i]=w->h;
        w->x = DESK_X; w->y = DESK_Y;
        w->w = DESK_W; w->h = DESK_H;
        win_maximized[i] = 1;
    } else {
        w->x=win_saved_x[i]; w->y=win_saved_y[i];
        w->w=win_saved_w[i]; w->h=win_saved_h[i];
        win_maximized[i]=0;
    }
    if (resize_win == i) resize_win = -1;      /* the two interactions never race */
    gui_window_focus(i);
    g_terminal.redraw_needed = 1;
}

/* New Terminal: top-bar button, App/Programs menu, dock "+" icon, desktop icon.
 * Reports a full window table in the main terminal. */
static void gui_do_new_terminal(void)
{
    if (gui_window_new_terminal() < 0) {
        terminal_t *t = gui_terminal_of(WIN_TERMINAL);
        if (t) terminal_puts(t, "term: window limit reached\n", TERM_COL_ERROR);
    }
    g_terminal.redraw_needed = 1;
}

static void gui_menu_action(int act)
{
    switch (act) {
    case A_ABOUT:     gui_menu_open_window(WIN_INFO);     break;
    case A_SETTINGS:
    case A_OPEN_SET:  gui_menu_open_window(WIN_SETTINGS); break;
    case A_OPEN_TERM: gui_menu_open_window(WIN_TERMINAL); break;
    case A_OPEN_INFO: gui_menu_open_window(WIN_INFO);     break;
    case A_SHUTDOWN:  sys_shutdown(); break;
    case A_REBOOT:    sys_reboot();   break;
    case A_NEWTERM:   gui_do_new_terminal(); break;
    case A_CLOSEWIN:  if (active_window >= 0) gui_close_window(active_window);    break;
    case A_MINWIN:    if (active_window >= 0) gui_minimize_window(active_window); break;
    case A_ZOOMWIN:   if (active_window >= 0) gui_toggle_maximize(active_window); break;
    case A_HELP:
    case A_VERSION:
        /* surface the Shell's own output: open the Terminal, run the command */
        gui_menu_open_window(WIN_TERMINAL);
        terminal_run_command(&g_terminal, act == A_HELP ? "help" : "version");
        break;
    default: break;
    }
}

/* ===== gui_handle_mouse ===== */
void gui_handle_mouse(int x, int y, int left, int right)
{
    (void)right;
    int click = (left && !prev_ml);

    /* ---- DAY43 PRE-4: the modal window (write editor) gets first pick ---- */
    if (modal_mouse(x, y, left, click)) return;

    /* hover feedback: repaint when the dock hover target changes, or while a
     * menu is open (its rows highlight under the cursor) */
    {
        static int last_hover = -2;
        int hd = dock_hover_item(x, y);
        if (hd != last_hover) { last_hover = hd; g_terminal.redraw_needed = 1; }
        if (popup_menu) g_terminal.redraw_needed = 1;
        if (y < MENUBAR_H) g_terminal.redraw_needed = 1;   /* New Terminal button hover */
    }

    /* ---- Active resize: highest priority while in progress (④ NEW) ---- */
    if (resize_win >= 0) {
        if (left) {
            gui_window_t *w = &windows[resize_win];
            int nw = x - resize_off_x - w->x;
            int nh = y - resize_off_y - w->y;
            if (nw < MIN_WIN_W) nw = MIN_WIN_W;
            if (nh < MIN_WIN_H) nh = MIN_WIN_H;
            if (w->x + nw > SCREEN_W) nw = SCREEN_W - w->x;
            if (w->y + nh > SCREEN_H) nh = SCREEN_H - w->y;
            w->w = nw; w->h = nh;
            g_terminal.redraw_needed = 1;
            return;
        } else {
            resize_win = -1;
        }
    }

    /* ---- Active drag: move window ---- */
    if (drag_win >= 0) {
        if (left) {
            int nx = x - drag_off_x;
            int ny = y - drag_off_y;
            if (nx < DOCK_W)    nx = DOCK_W;
            if (ny < MENUBAR_H) ny = MENUBAR_H;
            if (nx + windows[drag_win].w > SCREEN_W)
                nx = SCREEN_W - windows[drag_win].w;
            if (ny + windows[drag_win].h > SCREEN_H)
                ny = SCREEN_H - windows[drag_win].h;
            windows[drag_win].x = nx;
            windows[drag_win].y = ny;
            g_terminal.redraw_needed = 1;
            return;
        } else {
            drag_win = -1;
        }
    }

    /* ---- MenuBar ---- */
    if (y < MENUBAR_H) {
        menubar_app_name();
        if (!click) {
            /* macOS behaviour: with a menu open, hovering another title switches to it */
            if (popup_menu >= MENU_LOGO && popup_menu <= MENU_HELP) {
                int m = menubar_menu_at(x);
                if (m && m != popup_menu) popup_menu = m;
            }
            return;
        }
        if (x >= POWER_X) {
            popup_menu = (popup_menu == MENU_POWER) ? 0 : MENU_POWER;
        } else if (x >= NEWBTN_X && x < NEWBTN_X + NEWTERM_BTN_W) {
            popup_menu = 0;
            gui_do_new_terminal();
        } else {
            int m = menubar_menu_at(x);
            popup_menu = (m && m != popup_menu) ? m : 0;
        }
        g_terminal.redraw_needed = 1;
        return;
    }

    /* ---- Open popup menu: pick an item, or close on any other click ---- */
    if (popup_menu && click) {
        int item = popup_item_at(popup_menu, x, y);
        int m = popup_menu;
        popup_menu = 0;
        g_terminal.redraw_needed = 1;
        if (item >= 0) {
            int n; const mitem_t *it = menu_items(m, &n);
            gui_menu_action(it[item].act);
        }
        return;
    }

    /* ---- Dock ---- */
    if (x < DOCK_W) {
        if (!click) return;
        popup_menu = 0;
        for (int i = 0; i < DOCK_APPS; i++) {
            dock_item_t *d = &dock_items[i];
            if (y >= d->y && y < d->y + DOCK_ICON_SZ) {
                int wid = d->win_id;
                if (wid < 0) {                         /* "+" : new terminal */
                    gui_do_new_terminal();
                    break;
                }
                if (!(windows[wid].flags & WIN_VISIBLE)) {
                    windows[wid].flags |= WIN_VISIBLE;
                    windows[wid].flags &= (unsigned char)~WIN_MINIMIZED;
                    gui_window_focus(wid);
                } else if (windows[wid].flags & WIN_MINIMIZED) {
                    windows[wid].flags &= (unsigned char)~WIN_MINIMIZED;
                    gui_window_focus(wid);
                } else if (wid == active_window) {
                    windows[wid].flags |= WIN_MINIMIZED;
                } else {
                    gui_window_focus(wid);
                }
                g_terminal.redraw_needed=1;
                break;
            }
        }
        return;
    }

    /* ---- Window button / resize-grip hit-test (walk z_order top→bottom) ---- */
    if (click) {
        for (int zi = z_order_count-1; zi >= 0; zi--) {
            int i = z_order[zi];
            if (i < 0 || i >= MAX_WINDOWS) continue;
            if (!(windows[i].flags & WIN_VISIBLE)) continue;
            if (  windows[i].flags & WIN_MINIMIZED) continue;
            gui_window_t *w = &windows[i];

            /* Must be within the window's own rect at all to claim this click */
            if (x < w->x || x >= w->x+w->w || y < w->y || y >= w->y+w->h)
                continue;

            /* Resize grip (bottom-right corner), only if not maximized */
            if (!win_maximized[i]) {
                int gx = w->x + w->w - RESIZE_HANDLE_SZ;
                int gy = w->y + w->h - RESIZE_HANDLE_SZ;
                if (x >= gx && x < w->x+w->w && y >= gy && y < w->y+w->h) {
                    gui_window_focus(i);
                    resize_win   = i;
                    resize_off_x = x - (w->x + w->w);
                    resize_off_y = y - (w->y + w->h);
                    return;
                }
            }

            /* Button row: gated on x falling inside the three buttons
             * themselves, otherwise it would swallow title-bar drags. */
            int bx_right = w->x + w->w - 2;
            int mnbx = bx_right - WIN_BTN_W - 1 - WIN_BTN_W - 1 - WIN_BTN_W;
            if (y >= w->y+2 && y < w->y+2+WIN_BTN_H &&
                x >= mnbx  && x < bx_right) {
                int b = chrome_button_at(x, y, w->x, w->y, w->w);
                if (b == 1) { gui_close_window(i);    return; }
                if (b == 2) { gui_toggle_maximize(i); return; }
                if (b == 3) { gui_minimize_window(i); return; }
                /* a 1px gap between buttons: still claim focus/raise */
                gui_window_focus(i);
                return;
            }

            /* Titlebar: focus + start drag. (PRE-3 ①: tested BEFORE the
             * Settings controls so Settings can be dragged too.) */
            if (y < w->y+TITLEBAR_H) {
                gui_window_focus(i);
                if (win_maximized[i]) {
                    /* dragging a maximized window restores it first,
                     * centred under the cursor (Windows behaviour) */
                    int rw = win_saved_w[i], rh = win_saved_h[i];
                    int rx = x - rw / 2;
                    if (rx < DOCK_W) rx = DOCK_W;
                    if (rx + rw > SCREEN_W) rx = SCREEN_W - rw;
                    w->x = rx; w->y = y - TITLEBAR_H / 2;
                    if (w->y < MENUBAR_H) w->y = MENUBAR_H;
                    w->w = rw; w->h = rh;
                    win_maximized[i] = 0;
                }
                drag_win   = i;
                drag_off_x = x - w->x;
                drag_off_y = y - w->y;
                return;
            }

            /* Settings panel: let window2 try to handle interactive rows */
            if (i == WIN_SETTINGS) {
                gui_window_focus(i);
                if (gui_window2_settings_click(x, y)) {
                    g_terminal.redraw_needed = 1;
                    return;
                }
                return;
            }

            /* Body click: just focus/raise */
            gui_window_focus(i);
            return;
        }

        /* The click missed every window: bare desktop -> desktop icons. */
        if (g_show_desktop_icons) {
            for (int di = 0; di < desk_icon_count; di++) {
                desk_icon_t *ic = &desk_icons[di];
                if (!ic->visible) continue;
                if (x >= ic->x - 8 && x < ic->x + DESK_ICON_SZ + 8 &&
                    y >= ic->y && y < ic->y + DESK_ICON_SZ + 18) {
                    if (ic->win_id == DESK_ACTION_NEWTERM) { gui_do_new_terminal(); return; }
                    if (ic->win_id < 0) return;
                    gui_menu_open_window(ic->win_id);
                    return;
                }
            }
        }
    }
}

void gui_update_mouse(int mx, int my) { (void)mx; (void)my; }
