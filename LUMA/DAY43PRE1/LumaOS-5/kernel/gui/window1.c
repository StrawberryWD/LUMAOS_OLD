/* kernel/gui/window1.c - LumaOS 4.1 GUI Core
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
static void (*g_modal_draw)(void) = (void (*)(void))0;
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
static int z_order_count = 3; /* number of live entries at the front of z_order[] */

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

/* ---- popup state ---- */
static int popup_battery = 0;
static int popup_menu    = 0;

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

void gui_set_palette(void)
{
    if (g_theme_light) gui_set_palette_light();
    else                gui_set_palette_dark();
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

/* ===== MenuBar ===== */
void gui_draw_menubar(void)
{
    vga_draw_rect(0, 0, SCREEN_W, MENUBAR_H, COL_MENUBAR);
    vga_draw_hline(0, MENUBAR_H - 1, SCREEN_W, COL_DGRAY);

    vga_draw_string(4, 3, "LumaOS 5.0", COL_MENUTEXT);

    vga_draw_string( 90, 3, "Programs", COL_MENUTEXT);
    vga_draw_string(162, 3, "Settings", COL_MENUTEXT);
    vga_draw_string(234, 3, "Help",     COL_MENUTEXT);

    /* Battery / power button — right side */
    vga_draw_string(SCREEN_W - 80, 3, "[Power]", COL_MENUTEXT);

    if (popup_battery) {
        int px = SCREEN_W - 80, py = MENUBAR_H;
        vga_draw_rect(px, py, 70, 28, COL_POPUP_BG);
        vga_draw_hline(px, py,    70, COL_DGRAY);
        vga_draw_vline(px, py,    28, COL_DGRAY);
        vga_draw_vline(px+69, py, 28, COL_DGRAY);
        vga_draw_hline(px, py+27, 70, COL_DGRAY);
        vga_draw_string(px + 4, py + 4,  "Shutdown", COL_LRED);
        vga_draw_string(px + 4, py + 15, "Reboot",   COL_YELLOW);
    }

    if (popup_menu) {
        int mx2 = (popup_menu == 1) ?  86 :
                  (popup_menu == 2) ? 158 : 230;
        int my2 = MENUBAR_H;
        vga_draw_rect(mx2, my2, 80, 28, COL_POPUP_BG);
        vga_draw_hline(mx2,    my2,    80, COL_DGRAY);
        vga_draw_vline(mx2,    my2,    28, COL_DGRAY);
        vga_draw_vline(mx2+79, my2,    28, COL_DGRAY);
        vga_draw_hline(mx2, my2+27,    80, COL_DGRAY);
        if (popup_menu == 1) {
            vga_draw_string(mx2+4, my2+4,  "Terminal", COL_MENUTEXT);
            vga_draw_string(mx2+4, my2+15, "Info",     COL_MENUTEXT);
        } else if (popup_menu == 2) {
            vga_draw_string(mx2+4, my2+4,  "Settings", COL_MENUTEXT);
            vga_draw_string(mx2+4, my2+15, "About",    COL_MENUTEXT);
        } else {
            vga_draw_string(mx2+4, my2+4,  "Help",    COL_MENUTEXT);
            vga_draw_string(mx2+4, my2+15, "Version", COL_MENUTEXT);
        }
    }
}

/* ===== Dock ===== */
void gui_draw_dock(void)
{
    vga_draw_rect(0, MENUBAR_H, DOCK_W, SCREEN_H - MENUBAR_H, COL_DOCK_BG);
    vga_draw_vline(DOCK_W - 1, MENUBAR_H, SCREEN_H - MENUBAR_H, COL_DGRAY);

    for (int i = 0; i < DOCK_APPS; i++) {
        dock_item_t *d = &dock_items[i];
        int wid = d->win_id;
        int open     = (wid >= 0 && (windows[wid].flags & WIN_VISIBLE));
        int minimized= (wid >= 0 && (windows[wid].flags & WIN_MINIMIZED));

        unsigned char ic = (i == 0) ? (unsigned char)COL_DOCK_ICON0
                         : (i == 1) ? (unsigned char)COL_DOCK_ICON1
                                    : (unsigned char)COL_SETTINGS_BG;
        vga_draw_rect(d->x, d->y, DOCK_ICON_SZ, DOCK_ICON_SZ, ic);

        unsigned char bc = (wid == active_window && open && !minimized)
                         ? (unsigned char)COL_WHITE
                         : (unsigned char)COL_DGRAY;
        vga_draw_hline(d->x,               d->y,                DOCK_ICON_SZ, bc);
        vga_draw_hline(d->x,               d->y+DOCK_ICON_SZ-1, DOCK_ICON_SZ, bc);
        vga_draw_vline(d->x,               d->y,  DOCK_ICON_SZ, bc);
        vga_draw_vline(d->x+DOCK_ICON_SZ-1,d->y,  DOCK_ICON_SZ, bc);

        vga_draw_string(d->x + 2, d->y + 13, d->label, COL_MENUTEXT);

        if (open && !minimized)
            vga_draw_rect(d->x + 14, d->y + DOCK_ICON_SZ + 2, 4, 2, COL_MENUTEXT);
    }
}

/* ===== Desktop ===== */
void gui_draw_desktop(void)
{
    vga_draw_rect(DOCK_W, MENUBAR_H,
                  SCREEN_W - DOCK_W, SCREEN_H - MENUBAR_H,
                  COL_DESKTOP);

    /* === MODIFIED SECTION START ===
     * STAGE4 FIX: desktop icons were fully wired (desk_icons[],
     * desk_icon_count, the Settings "Show Desktop Icons" toggle) but
     * nothing ever rendered them — this loop was missing entirely.
     * Respects g_show_desktop_icons (Settings toggle) and each icon's
     * own `visible` flag. */
    if (g_show_desktop_icons) {
        for (int i = 0; i < desk_icon_count; i++) {
            if (desk_icons[i].visible)
                gui_draw_desktop_icon(&desk_icons[i]);
        }
    }
    /* === MODIFIED SECTION END === */
}

/* === MODIFIED SECTION START ===
 * STAGE4 (new function): draws a single desktop icon — a coloured
 * square (same bordered style as dock icons) with its label centred
 * underneath, wrapped/clipped to the icon's own width so long labels
 * can't bleed into neighbouring icons. */
void gui_draw_desktop_icon(const desk_icon_t *icon)
{
    if (!icon) return;

    vga_draw_rect(icon->x, icon->y, DESK_ICON_SZ, DESK_ICON_SZ, icon->color);

    unsigned char bc = (unsigned char)COL_DGRAY;
    vga_draw_hline(icon->x,                  icon->y,                 DESK_ICON_SZ, bc);
    vga_draw_hline(icon->x,                  icon->y+DESK_ICON_SZ-1,  DESK_ICON_SZ, bc);
    vga_draw_vline(icon->x,                  icon->y,                 DESK_ICON_SZ, bc);
    vga_draw_vline(icon->x+DESK_ICON_SZ-1,   icon->y,                 DESK_ICON_SZ, bc);

    /* Label background strip (uses the colour Stage3 already reserved
     * for this: COL_DESKTOP_ICON_BG) so text stays legible regardless
     * of desktop background colour/theme. */
    int label_y = icon->y + DESK_ICON_SZ + 2;
    vga_draw_rect(icon->x - 2, label_y - 1, DESK_ICON_SZ + 4, 10, COL_DESKTOP_ICON_BG);
    vga_draw_string_clipped(icon->x, label_y, (char *)icon->label,
                             COL_MENUTEXT, DESK_ICON_SZ);

    /* Highlight the icon if its target window is currently open+active,
     * mirroring the dock's "active" indicator dot. */
    if (icon->win_id >= 0 && icon->win_id == active_window &&
        (windows[icon->win_id].flags & WIN_VISIBLE) &&
        !(windows[icon->win_id].flags & WIN_MINIMIZED)) {
        vga_draw_rect(icon->x + DESK_ICON_SZ/2 - 2, icon->y + DESK_ICON_SZ + 12, 4, 2, COL_MENUTEXT);
    }
}
/* === MODIFIED SECTION END === */

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

/* ===== Window draw ===== */
void gui_window_draw(int id)
{
    if (id < 0 || id >= MAX_WINDOWS) return;
    gui_window_t *w = &windows[id];
    if (!(w->flags & WIN_VISIBLE)) return;
    if (  w->flags & WIN_MINIMIZED) return;

    int ax = w->x, ay = w->y, aw = w->w, ah = w->h;
    int active = (id == active_window);

    /* Drop shadow (only for active to further distinguish) */
    if (active)
        vga_draw_rect(ax+3, ay+3, aw, ah, COL_SHADOW);
    else
        vga_draw_rect(ax+2, ay+2, aw, ah, COL_SHADOW);

    /* Window body */
    vga_draw_rect(ax, ay, aw, ah, COL_WIN_BODY);

    /* Border */
    unsigned char bc = active ? (unsigned char)COL_WIN_BORDER
                              : (unsigned char)COL_DGRAY;
    vga_draw_hline(ax,      ay,      aw, bc);
    vga_draw_hline(ax,      ay+ah-1, aw, bc);
    vga_draw_vline(ax,      ay,      ah, bc);
    vga_draw_vline(ax+aw-1, ay,      ah, bc);

    /* Title bar */
    unsigned char tc = active ? (unsigned char)COL_WIN_TITLE
                              : (unsigned char)COL_WIN_INACT;
    vga_draw_rect(ax+1, ay+1, aw-2, TITLEBAR_H-2, tc);

    /* Title text */
    unsigned char txt_col = active ? (unsigned char)COL_TEXT_PRIMARY
                                   : (unsigned char)COL_TEXT_SECONDARY;
    vga_draw_string(ax+4, ay+3, w->title, txt_col);

    /* ---- Window buttons (top-right) ---- */
    int bx = ax + aw - 2;

    /* Close [x] */
    bx -= WIN_BTN_W;
    int clx = bx, cly = ay + 2;
    unsigned char btn_cl = active ? (unsigned char)COL_BTN_CLOSE
                                  : (unsigned char)COL_DGRAY;
    vga_draw_rect(clx, cly, WIN_BTN_W, WIN_BTN_H, btn_cl);
    if (active) {
        vga_put_pixel(clx+2,cly+2,COL_WHITE); vga_put_pixel(clx+6,cly+2,COL_WHITE);
        vga_put_pixel(clx+3,cly+3,COL_WHITE); vga_put_pixel(clx+5,cly+3,COL_WHITE);
        vga_put_pixel(clx+4,cly+4,COL_WHITE);
        vga_put_pixel(clx+3,cly+5,COL_WHITE); vga_put_pixel(clx+5,cly+5,COL_WHITE);
        vga_put_pixel(clx+2,cly+6,COL_WHITE); vga_put_pixel(clx+6,cly+6,COL_WHITE);
    }

    /* Maximize [ ] */
    bx -= WIN_BTN_W + 1;
    int mxbx = bx;
    unsigned char btn_mx = active ? (unsigned char)COL_BTN_MAX
                                  : (unsigned char)COL_DGRAY;
    if (win_maximized[id] && active) {
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

    /* Minimize [_] */
    bx -= WIN_BTN_W + 1;
    int mnbx = bx;
    unsigned char btn_mn = active ? (unsigned char)COL_BTN_MIN
                                  : (unsigned char)COL_DGRAY;
    vga_draw_rect(mnbx, cly, WIN_BTN_W, WIN_BTN_H, btn_mn);
    if (active)
        vga_draw_hline(mnbx+2, cly+WIN_BTN_H-3, WIN_BTN_W-4, COL_WHITE);

    /* ---- Resize grip (④ NEW) — only when visible & not maximized ---- */
    if (!win_maximized[id]) {
        int gx = ax + aw - RESIZE_HANDLE_SZ;
        int gy = ay + ah - RESIZE_HANDLE_SZ;
        vga_draw_rect(gx, gy, RESIZE_HANDLE_SZ, RESIZE_HANDLE_SZ, COL_RESIZE_HANDLE);
        /* diagonal grip lines for affordance */
        for (int i = 1; i < RESIZE_HANDLE_SZ; i += 2)
            vga_put_pixel(gx + i, gy + RESIZE_HANDLE_SZ - 1 - i, COL_WIN_BORDER);
    }

    /* Terminal content area */
    if (w->flags & WIN_HAS_TERM)
        terminal_render(&g_terminal, ax, ay, aw, ah);

    /* Settings content drawn by window2 */
    if (id == WIN_SETTINGS)
        gui_window2_settings_draw();

    /* Info content drawn by window2 -- drawn here (inside the
     * z-order-respecting per-window pass) rather than as a separate
     * global overlay, so it can never paint on top of another window
     * stacked above it. */
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

/* ===== terminal_render ===== */
void terminal_render(terminal_t *t, int wx, int wy, int ww, int wh)
{
    int cx = wx + 2;
    int cy = wy + TITLEBAR_H + 1;
    int cw = ww - 6;
    int ch = wh - TITLEBAR_H - 14;
    if (cw <= 0 || ch <= 0) return;

    vga_draw_rect(cx, cy, cw, ch, COL_TERM_BG);

    int visible_rows = ch / 9;
    int visible_cols = cw / 8;

    int start = t->total_lines - visible_rows - t->scroll_offset;
    if (start < 0) start = 0;

    for (int row = 0; row < visible_rows; row++) {
        int li = start + row;
        if (li >= t->total_lines) break;
        int dy = cy + row * 9;
        int cols = t->lines[li].len;
        if (cols > visible_cols) cols = visible_cols;
        for (int col = 0; col < cols; col++) {
            char c = t->lines[li].text[col];
            if (c >= 32 && c <= 126)
                vga_draw_char(cx + col*8, dy, c, t->lines[li].color[col]);
        }
    }

    int sep_y = wy + wh - 13;
    vga_draw_hline(cx, sep_y, cw, COL_WIN_BORDER);

    int iy = wy + wh - 11;
    vga_draw_rect(cx, iy, cw, 9, COL_TERM_BG);
    /* DAY43: prompt shows volume + cwd like the text shell ("IDE3/docs/ > "),
     * not a bare "$ ". Truncated to 24 chars so the input keeps room. */
    char prompt[40];
    gui_build_prompt(prompt);
    vga_draw_string(cx, iy, prompt, COL_TERM_CUR);
    int pw = (int)strlen(prompt) * 8;
    int maxvis = (cw - pw - 8) / 8;
    if (maxvis < 0) maxvis = 0;
    for (int i = 0; i < t->input_len && i < maxvis; i++) {
        char ic[2] = { t->input[i], 0 };
        vga_draw_string(cx + pw + i*8, iy, ic, COL_TEXT_PRIMARY);
    }

    t->cursor_blink = (t->cursor_blink + 1) % 30;
    if (t->cursor_blink < 15) {
        int cur_x = cx + pw + t->input_cursor * 8;
        if (t->input_cursor <= maxvis)
            vga_draw_rect(cur_x, iy, 2, 8, COL_TERM_CUR);
    }

    if (t->total_lines > visible_rows) {
        int sbx = wx + ww - 4;
        vga_draw_rect(sbx, cy, 3, ch, COL_SCROLLBAR);
        int th = (visible_rows * ch) / t->total_lines;
        if (th < 3) th = 3;
        int top_row = t->total_lines - visible_rows - t->scroll_offset;
        if (top_row < 0) top_row = 0;
        int ty = cy + (top_row * ch) / t->total_lines;
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
        terminal_run_command(t, t->input);
        for (int i=0; i<=TERM_INPUT_MAX; i++) t->input[i]=0;
        t->input_len=0; t->input_cursor=0;
        t->history_pos=-1; t->scroll_offset=0;
        goto done;
    }
    if (key == '\b' || key == KEY_BS) {
        if (t->input_cursor > 0) {
            for (int i=t->input_cursor-1; i<t->input_len-1; i++)
                t->input[i]=t->input[i+1];
            t->input_len--; t->input_cursor--;
            t->input[t->input_len]=0;
        }
        goto done;
    }
    if (key >= 32 && key <= 126 && t->input_len < TERM_INPUT_MAX) {
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
    }

    /* Z-ORDER: initial order back→front: Settings, Info, Terminal.
     * Only 3 live windows participate; slot 3 is spare/unused. */
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

    /* Window 3: spare — keep hidden, never enters z_order */
    windows[3].flags = 0;

    /* Dock items */
    dock_items[DOCK_APP_TERM].app_id = DOCK_APP_TERM;
    dock_items[DOCK_APP_TERM].x      = (DOCK_W - DOCK_ICON_SZ) / 2;
    dock_items[DOCK_APP_TERM].y      = MENUBAR_H + 6;
    dock_items[DOCK_APP_TERM].win_id = WIN_TERMINAL;
    dock_items[DOCK_APP_TERM].label[0]='T';
    dock_items[DOCK_APP_TERM].label[1]='M';
    dock_items[DOCK_APP_TERM].label[2]='R';
    dock_items[DOCK_APP_TERM].label[3]=0;

    dock_items[DOCK_APP_INFO].app_id = DOCK_APP_INFO;
    dock_items[DOCK_APP_INFO].x      = (DOCK_W - DOCK_ICON_SZ) / 2;
    dock_items[DOCK_APP_INFO].y      = MENUBAR_H + 6 + DOCK_ICON_SZ + 8;
    dock_items[DOCK_APP_INFO].win_id = WIN_INFO;
    dock_items[DOCK_APP_INFO].label[0]='I';
    dock_items[DOCK_APP_INFO].label[1]='N';
    dock_items[DOCK_APP_INFO].label[2]='F';
    dock_items[DOCK_APP_INFO].label[3]=0;

    dock_items[DOCK_APP_SET].app_id = DOCK_APP_SET;
    dock_items[DOCK_APP_SET].x      = (DOCK_W - DOCK_ICON_SZ) / 2;
    dock_items[DOCK_APP_SET].y      = MENUBAR_H + 6 + (DOCK_ICON_SZ + 8) * 2;
    dock_items[DOCK_APP_SET].win_id = WIN_SETTINGS;
    dock_items[DOCK_APP_SET].label[0]='S';
    dock_items[DOCK_APP_SET].label[1]='E';
    dock_items[DOCK_APP_SET].label[2]='T';
    dock_items[DOCK_APP_SET].label[3]=0;

    /* === MODIFIED SECTION START ===
     * STAGE4 FIX: populate desk_icons[] so gui_draw_desktop() has
     * something to draw. Mirrors the dock's three apps, stacked
     * vertically near the top-left of the desktop area so they never
     * overlap the Terminal window's default position. Uses the same
     * colour indices as the dock icons for visual consistency. */
    desk_icon_count = 0;

    desk_icons[desk_icon_count].x       = DESK_X + 16;
    desk_icons[desk_icon_count].y       = DESK_Y + 16;
    desk_icons[desk_icon_count].win_id  = WIN_TERMINAL;
    desk_icons[desk_icon_count].color   = (unsigned char)COL_DOCK_ICON0;
    desk_icons[desk_icon_count].visible = 1;
    desk_icons[desk_icon_count].label[0]='T'; desk_icons[desk_icon_count].label[1]='e';
    desk_icons[desk_icon_count].label[2]='r'; desk_icons[desk_icon_count].label[3]='m';
    desk_icons[desk_icon_count].label[4]=0;
    desk_icon_count++;

    desk_icons[desk_icon_count].x       = DESK_X + 16;
    desk_icons[desk_icon_count].y       = DESK_Y + 16 + DESK_ICON_SZ + DESK_ICON_GAP;
    desk_icons[desk_icon_count].win_id  = WIN_INFO;
    desk_icons[desk_icon_count].color   = (unsigned char)COL_DOCK_ICON1;
    desk_icons[desk_icon_count].visible = 1;
    desk_icons[desk_icon_count].label[0]='I'; desk_icons[desk_icon_count].label[1]='n';
    desk_icons[desk_icon_count].label[2]='f'; desk_icons[desk_icon_count].label[3]='o';
    desk_icons[desk_icon_count].label[4]=0;
    desk_icon_count++;

    desk_icons[desk_icon_count].x       = DESK_X + 16;
    desk_icons[desk_icon_count].y       = DESK_Y + 16 + (DESK_ICON_SZ + DESK_ICON_GAP) * 2;
    desk_icons[desk_icon_count].win_id  = WIN_SETTINGS;
    desk_icons[desk_icon_count].color   = (unsigned char)COL_SETTINGS_BG;
    desk_icons[desk_icon_count].visible = 1;
    desk_icons[desk_icon_count].label[0]='S'; desk_icons[desk_icon_count].label[1]='e';
    desk_icons[desk_icon_count].label[2]='t'; desk_icons[desk_icon_count].label[3]='t';
    desk_icons[desk_icon_count].label[4]='i'; desk_icons[desk_icon_count].label[5]='n';
    desk_icons[desk_icon_count].label[6]='g'; desk_icons[desk_icon_count].label[7]='s';
    desk_icons[desk_icon_count].label[8]=0;
    desk_icon_count++;
    /* === MODIFIED SECTION END === */

    /* Terminal has first focus */
    gui_window_focus(WIN_TERMINAL);

    prev_mx=mouse_x; prev_my=mouse_y; prev_ml=0; prev_mr=0;
    drag_win=-1; resize_win=-1; popup_battery=0; popup_menu=0;

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
    if (g_modal_draw) g_modal_draw();      /* DAY43: modal panel on top */
    gui_draw_cursor(mx, my);
    vga_flip();
}

/* ===== gui_refresh ===== */
void gui_refresh(void)
{
    unsigned char key = keyboard_read();
    if (key) gui_handle_key(key);

    int mx=mouse_x, my=mouse_y, ml=mouse_left_button, mr=mouse_right_button;
    if (mx!=prev_mx || my!=prev_my || ml!=prev_ml || mr!=prev_mr) {
        gui_handle_mouse(mx, my, ml, mr);
        prev_mx=mx; prev_my=my; prev_ml=ml; prev_mr=mr;
    }

    /* Scroll wheel: poll accumulated delta and dispatch once per frame */
    int wheel = mouse_consume_wheel();
    if (wheel != 0) {
        gui_handle_wheel(mx, my, wheel);
    }

    gui2_tick();

    static int frame=0; frame++;
    if (g_terminal.redraw_needed || g_modal_draw || (frame % 15 == 0)) {
        gui_draw_frame(mx, my);
    }
}

/* ===== gui_modal_set_draw / gui_modal_pump (DAY43) ===== */
void gui_modal_set_draw(void (*fn)(void))
{
    g_modal_draw = fn;
    g_terminal.redraw_needed = 1;
}

/* One GUI-loop iteration WITHOUT key dispatch: the modal owner reads the
 * keyboard itself, but the mouse cursor keeps moving, the clock/tick
 * code keeps running, and the frame (desktop + windows + overlay) is
 * repainted and flipped. */
void gui_modal_pump(void)
{
    gui2_tick();
    gui_draw_frame(mouse_x, mouse_y);
}

/* ===== gui_handle_key ===== */
void gui_handle_key(unsigned char key)
{
    if (popup_battery || popup_menu) {
        popup_battery=0; popup_menu=0;
        g_terminal.redraw_needed=1;
    }

    /* Dispatch to focused window. active_window is the single source
     * of truth for keyboard focus (kept in sync with z_order top by
     * gui_window_focus()), so this routing can never point at a
     * window that isn't actually the visible top window. */
    if (active_window == WIN_TERMINAL && (windows[WIN_TERMINAL].flags & WIN_HAS_TERM))
        terminal_handle_key(&g_terminal, key);
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
            if (i == WIN_TERMINAL && (w->flags & WIN_HAS_TERM)) {
                terminal_scroll(&g_terminal, lines);
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
 * Shared "open, un-minimize, or just focus" helper for anything that
 * targets a window id by clicking a fixed UI element (dock icon,
 * desktop icon, or -- new -- a MenuBar dropdown item). Kept as a single
 * implementation so all of these entry points always agree on what
 * "open" means for a hidden/minimized/already-visible window. */
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

/* ===== gui_handle_mouse ===== */
void gui_handle_mouse(int x, int y, int left, int right)
{
    (void)right;

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

    int click = (left && !prev_ml);

    /* ---- MenuBar ---- */
    if (y < MENUBAR_H) {
        if (!click) return;
        if (x >= SCREEN_W - 80) {
            popup_battery = !popup_battery;
            popup_menu    = 0;
            g_terminal.redraw_needed = 1;
            return;
        }
        if (x >= 86  && x < 158) { popup_menu=(popup_menu==1)?0:1; popup_battery=0; }
        else if (x >= 158 && x < 230) { popup_menu=(popup_menu==2)?0:2; popup_battery=0; }
        else if (x >= 230 && x < 290) { popup_menu=(popup_menu==3)?0:3; popup_battery=0; }
        else { popup_menu=0; popup_battery=0; }
        g_terminal.redraw_needed=1;
        return;
    }

    /* ---- Battery popup: power actions ---- */
    if (popup_battery && click) {
        int px = SCREEN_W - 80, py = MENUBAR_H;
        if (x >= px && x < px+70) {
            if (y >= py && y < py+13)     { sys_shutdown(); return; }
            if (y >= py+13 && y < py+28)  { sys_reboot();   return; }
        }
        popup_battery=0; g_terminal.redraw_needed=1; return;
    }

    /* ---- Menu popup actions -----------------------------------------
     * The dropdown itself opened correctly (see the MenuBar block
     * above), but until now any click while it was open just closed it
     * again -- the two item rows drawn inside the popup (see
     * gui_draw_menubar's popup_menu rendering) had no hit-test at all,
     * so "Programs -> Terminal/Info", "Settings -> Settings/About" and
     * "Help -> Help/Version" were unclickable. This now hit-tests the
     * same (mx2,my2,80,28) rect / two 13px-tall rows used for drawing
     * and dispatches each item to the window (or shell command) it
     * represents. */
    if (popup_menu && click) {
        int mx2 = (popup_menu == 1) ?  86 :
                  (popup_menu == 2) ? 158 : 230;
        int my2 = MENUBAR_H;
        if (x >= mx2 && x < mx2+80 && y >= my2 && y < my2+28) {
            int item = (y < my2+13) ? 0 : 1;   /* 0 = top row, 1 = bottom row */
            if (popup_menu == 1) {
                /* Programs: Terminal / Info */
                gui_menu_open_window(item == 0 ? WIN_TERMINAL : WIN_INFO);
            } else if (popup_menu == 2) {
                /* Settings: Settings / About (About = Info window's
                 * content, which already holds the About/version text) */
                gui_menu_open_window(item == 0 ? WIN_SETTINGS : WIN_INFO);
            } else {
                /* Help: Help / Version -- surface the Shell's own help
                 * output by opening the Terminal and running the
                 * matching built-in command. */
                gui_menu_open_window(WIN_TERMINAL);
                terminal_run_command(&g_terminal, item == 0 ? "help" : "version");
            }
        }
        popup_menu=0; popup_battery=0;
        g_terminal.redraw_needed=1;
        return;
    }

    /* ---- Dock ---- */
    if (x < DOCK_W) {
        if (!click) return;
        popup_battery=0; popup_menu=0;
        for (int i = 0; i < DOCK_APPS; i++) {
            dock_item_t *d = &dock_items[i];
            if (y >= d->y && y < d->y + DOCK_ICON_SZ) {
                int wid = d->win_id;
                if (wid < 0) break;
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

            /* Button row — the y-band spans nearly the whole titlebar
             * height, so this must ALSO be gated on x falling within the
             * horizontal span of the three buttons themselves (mnbx..
             * bx_right). Previously only y was checked, so any click
             * anywhere along the titlebar (including over the title
             * text, where users normally grab to drag) that happened to
             * land in that y-band was swallowed here and returned
             * before ever reaching the titlebar-drag check below —
             * leaving only a 1-2px sliver at the very top/bottom edge
             * of the titlebar able to start a drag. That is why a drag
             * could appear to "work" once (by chance grabbing that
             * sliver) and then fail on every subsequent attempt. */
            int bx_right = w->x + w->w - 2;
            int clx  = bx_right - WIN_BTN_W;              /* Close */
            int mxbx = clx  - WIN_BTN_W - 1;               /* Maximize */
            int mnbx = mxbx - WIN_BTN_W - 1;               /* Minimize (leftmost) */
            if (y >= w->y+2 && y < w->y+2+WIN_BTN_H &&
                x >= mnbx  && x < bx_right) {

                /* Close */
                if (x >= clx && x < clx+WIN_BTN_W) {
                    windows[i].flags &= (unsigned char)~WIN_VISIBLE;
                    if (active_window == i) gui_window_focus(WIN_TERMINAL);
                    g_terminal.redraw_needed=1; return;
                }
                /* Maximize */
                if (x >= mxbx && x < mxbx+WIN_BTN_W) {
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
                    /* Maximizing/restoring cancels any in-flight resize
                     * on this window so the two interactions never race. */
                    if (resize_win == i) resize_win = -1;
                    gui_window_focus(i);
                    g_terminal.redraw_needed=1; return;
                }
                /* Minimize */
                if (x >= mnbx && x < mnbx+WIN_BTN_W) {
                    windows[i].flags |= WIN_MINIMIZED;
                    if (resize_win == i) resize_win = -1;
                    if (drag_win   == i) drag_win   = -1;
                    if (active_window == i) gui_window_focus(WIN_TERMINAL);
                    g_terminal.redraw_needed=1; return;
                }
                /* Click was within the button cluster's horizontal span
                 * but missed all three (a 1px gap) — still claim
                 * focus/raise, then stop (topmost wins). */
                gui_window_focus(i);
                return;
            }

            /* Settings panel: let window2 try to handle interactive
             * rows (theme toggle, checkboxes, etc.) before falling
             * through to generic focus-only handling. */
            if (i == WIN_SETTINGS) {
                gui_window_focus(i);
                if (gui_window2_settings_click(x, y)) {
                    g_terminal.redraw_needed = 1;
                    return;
                }
                return;
            }

            /* Titlebar: focus + start drag */
            if (y < w->y+TITLEBAR_H) {
                gui_window_focus(i);
                drag_win   = i;
                drag_off_x = x - w->x;
                drag_off_y = y - w->y;
                return;
            }

            /* Body click: just focus/raise */
            gui_window_focus(i);
            if (popup_battery || popup_menu) {
                popup_battery=0; popup_menu=0;
                g_terminal.redraw_needed=1;
            }
            return;
        }

        /* === MODIFIED SECTION START ===
         * STAGE4 FIX: the click missed every window rect above, so it
         * landed on bare desktop — check desktop icons. Click-to-open
         * (single click) is used rather than double-click since there
         * is no existing double-click/timing state to build on, and
         * this matches the dock's existing single-click-to-open UX. */
        if (g_show_desktop_icons) {
            for (int di = 0; di < desk_icon_count; di++) {
                desk_icon_t *ic = &desk_icons[di];
                if (!ic->visible) continue;
                int label_h = 12; /* icon square + label strip, from gui_draw_desktop_icon */
                if (x >= ic->x && x < ic->x + DESK_ICON_SZ &&
                    y >= ic->y && y < ic->y + DESK_ICON_SZ + label_h) {
                    int wid = ic->win_id;
                    if (wid < 0) return;
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
                    return;
                }
            }
        }
        /* === MODIFIED SECTION END === */
    }
}

void gui_update_mouse(int mx, int my) { (void)mx; (void)my; }