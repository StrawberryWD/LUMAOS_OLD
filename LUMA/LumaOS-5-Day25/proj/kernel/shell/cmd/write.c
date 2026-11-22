/* kernel/shell/cmd/write.c
 * LumaOS 5.0 - Shell command: write
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "实现 fat_write(file, buf, size)：处理簇链扩展（如果当前簇链不够，
 *    追加新簇）。在 Shell 中输入 write test.txt，写入内容，show 能看到。"
 *
 * This is the Shell-visible exerciser of Day 25's real deliverable,
 * fat_write() (kernel/fs/fat/fat_vfs.c) — the same relationship
 * cmd_create.c (Day 24) has to fat_create_file(). Unlike every other
 * cmd_*.c in this project, `write` cannot just compute its output and
 * hand it to terminal_puts() for shell_exec()'s caller to flush
 * afterwards (text_shell_flush_new_lines(), kernel/core/kmain.c, only
 * runs once shell_exec() has already RETURNED) — an interactive
 * multi-line text editor needs the user to see each keystroke as it
 * happens, so this file talks to the real screen directly through
 * console_getc-style primitives (console_putc_color()/console_set_
 * cursor()/console_get_x()/console_get_y(), kernel/core/console.c) and
 * reads the keyboard itself via keyboard_getchar() (blocking), the
 * exact same pair of primitives kmain.c's own text_shell_run() already
 * uses for its single-line command prompt (see that function's own
 * comment for why this is safe: today's kmain() runs a pure text-mode
 * loop with no GUI event loop competing for the same keyboard queue —
 * see this file's own DAY25 design-decision note below for the fuller
 * version of that argument).
 *
 * WORKFLOW (per this project owner's own worked examples):
 *
 *   write test.txt                      (file doesn't exist yet)
 *     -> creates it (fat_create_file()) exactly like typing `create`
 *        first, then drops straight into the editor.
 *
 *   write test.txt                      (file already exists)
 *     ! TEST.TXT ALREADY EXISTS
 *     ! DO YOU WANT TO COVER THE FILE?
 *      - Y/N -                          (Y -> editor; N -> cancelled)
 *
 *   Editor: multi-line, Enter inserts a new numbered line ("1.", "2.",
 *   ...), arrow keys move the cursor (Up/Down between lines, Left/
 *   Right within a line), Backspace deletes a character or, at column
 *   0, merges the current line into the previous one. Ctrl+S ends the
 *   editor and persists the WHOLE buffer via fat_write() — see that
 *   function's own doc comment (fat_vfs.h) for why this is a full
 *   overwrite-from-byte-0, not an append, and for the "old chain
 *   tail/whole old chain left orphaned" leak this project's roadmap
 *   accepts until Day 27's truncate/free-chain work exists.
 *
 * WHY DIRECT console_* CALLS, NOT terminal_puts() (design decision,
 * expanded): terminal_puts() only appends to a terminal_t's in-memory
 * line buffer (kernel/gui/gui.h's own struct); nothing renders it to
 * the actual screen until the CALLER of shell_exec() flushes it —
 * text_shell_flush_new_lines() in text-mode, or window1.c's redraw
 * path in GUI mode — and both of those only run AFTER shell_exec()
 * returns. An editor that needs to redraw on every keystroke while
 * still INSIDE this one shell_exec() call (it hasn't returned yet —
 * the whole editing session is one call to cmd_write()) therefore
 * cannot go through `t` at all for its live rendering; it must own the
 * real screen directly, the same way kmain.c's text_shell_run() already
 * does for its own single-line input editor. `t` (the terminal_t this
 * function still receives, same signature as every other shell_cmd_fn_t)
 * is used only for the handful of lines printed BEFORE and AFTER the
 * interactive session (the usage error, the overwrite prompt, the
 * final "wrote N bytes" summary) — exactly where the existing
 * terminal_puts()-then-flush-after-return contract still applies
 * normally, same as `create`/`show`/`read`.
 *
 * SCOPE LIMITS (deliberate, matching this file's own worked examples
 * and this project's current single-line-per-directory-entry FAT
 * layer): line count capped at WRITE_MAX_LINES (20) and each line
 * capped at WRITE_MAX_LINE_LEN (72) visible characters — generous
 * headroom for every acceptance scenario (a handful of short lines),
 * chosen mainly to guarantee the editor's own on-screen redraw never
 * needs to scroll the real VGA console mid-session (see the "why a
 * fixed budget, not the full 25-row screen" note by WRITE_MAX_LINES'
 * own definition below for the full reasoning) — this project has no
 * text-wrapping/pagination concept anywhere yet, so keeping the
 * editor's own footprint comfortably inside one screen is simpler than
 * teaching it to scroll.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../drivers/keyboard.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../../lib/string.h"
#include "../shell.h"

/* WRITE_MAX_LINES / WRITE_MAX_LINE_LEN — see this file's header
 * comment's "SCOPE LIMITS" paragraph. WRITE_MAX_LINES=20 is chosen so
 * the editor's own UI (a 2-line header showing the filename plus a
 * blank separator) plus 20 numbered content lines plus a short footer
 * hint line comfortably fits inside VGA_ROWS=25 (kernel/core/
 * console.c) without this function ever needing to call
 * console.c's own scroll() — this function redraws by walking
 * `start_y` forward a fixed number of rows every time, and a redraw
 * that overran the 25-row screen would silently scroll the console
 * out from under that fixed math, desyncing every row this function
 * thinks it owns. WRITE_MAX_LINE_LEN=72 leaves room for a
 * "NN. " 4-character line-number prefix (NN up to 20 fits in 2 digits)
 * within VGA_COLS=80 with a few columns of margin. */
#define WRITE_MAX_LINES     20
#define WRITE_MAX_LINE_LEN  72

/* write_read_y_n(t)
 *
 * Blocking single-keypress Y/N prompt for the "file already exists"
 * confirmation. Deliberately NOT part of the line-editor's own
 * keystroke loop below (it happens BEFORE the editor starts, and only
 * needs one keypress, not a redrawing multi-line UI) — kept as its own
 * tiny helper so cmd_write() itself reads as "ask, maybe bail, then
 * edit" rather than interleaving this one-shot decision into the
 * editor's own state machine. Returns 1 for Y/y, 0 for N/n/anything
 * else (matches this project's own worked example, which only shows
 * Y and N as the two documented answers — an unrecognized keypress is
 * treated as the safer of the two, "don't overwrite", rather than
 * silently proceeding to destroy the file's old content). */
static int write_read_y_n(void)
{
    for (;;) {
        char c = keyboard_getchar();
        if (c == 'Y' || c == 'y')
            return 1;
        if (c == 'N' || c == 'n')
            return 0;
        /* Anything else (including Enter) — keep waiting rather than
         * silently defaulting, so a stray keypress can't accidentally
         * pick either answer for the user. */
    }
}

/* write_redraw(filename, lines, line_lens, num_lines, cur_row, cur_col,
 *               start_y)
 *
 * Redraws the WHOLE editor from `start_y` down, then repositions the
 * hardware cursor to (cur_col + 4, start_y + 2 + cur_row) — the "+4"
 * accounts for the "NN. " line-number prefix (see WRITE_MAX_LINE_LEN's
 * own comment above), the "+2" for this function's own 2-line header
 * (filename + a blank separator line) before line "1." begins.
 *
 * A full redraw on every keystroke (rather than an incremental patch
 * of just the changed cell) is the same "simpler and provably correct
 * over clever and fragile" trade-off this project has favored
 * elsewhere (e.g. fat_write()'s own "always allocate a fresh cluster
 * rather than reuse an old chain's tail" simplification, fat_vfs.h) —
 * WRITE_MAX_LINES/WRITE_MAX_LINE_LEN's own small fixed bound keeps a
 * full redraw cheap enough (at most 20 lines * 72 columns) that the
 * flicker/cost of redrawing unchanged lines is not a real concern at
 * this project's current scale. */
static void write_redraw(const char *filename,
                          char lines[][WRITE_MAX_LINE_LEN + 1],
                          const int *line_lens, int num_lines,
                          int cur_row, int cur_col, int start_y)
{
    int y = start_y;

    console_set_cursor(0, y);
    console_write_color("-- editing ", TERM_COL_INFO);
    console_write_color(filename, TERM_COL_INFO);
    console_write_color(" (Ctrl+S to save) --                                  ",
                         TERM_COL_INFO);
    y++;
    console_set_cursor(0, y);
    console_write_color("                                                                                ",
                         TERM_COL_DEFAULT);
    y++;

    for (int i = 0; i < WRITE_MAX_LINES; i++) {
        console_set_cursor(0, y + i);
        if (i < num_lines) {
            char prefix[8];
            sprintf(prefix, "%d. ", i + 1);
            console_write_color(prefix, TERM_COL_PROMPT);
            for (int j = 0; j < line_lens[i]; j++)
                console_putc_color(lines[i][j], COLOR_DEFAULT);
        }
        /* Blank out the rest of this row so a shortened line (e.g.
         * after Backspace deleted trailing characters, or a merged-
         * away line further down shifting everything up) doesn't leave
         * stale characters from the previous redraw sitting past the
         * new, shorter content. */
        int printed = (i < num_lines) ? (4 + line_lens[i]) : 0;
        for (int col = printed; col < 80; col++)
            console_putc_color(' ', COLOR_DEFAULT);
    }

    console_set_cursor(4 + cur_col, start_y + 2 + cur_row);
}

void cmd_write(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: write <filename>  (e.g. write test.txt, "
                          "or write IDE0/TEST.TXT)\n", TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "write: no mount-slot given and no "
                          "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }
    const char *path = path_buf;

    /* ---- Does it already exist? fat_open() succeeding is this
     * project's own definition of "exists" (same check fat_create_
     * file() itself performs internally to reject a duplicate name,
     * fat_vfs.c's DAY24 doc comment). ---- */
    vfs_node_t existing;
    int open_err = fat_open(path, &existing);

    if (open_err == FAT_OK) {
        if (existing.type != VFS_TYPE_FILE) {
            terminal_puts(t, "write: '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' is a directory\n", TERM_COL_ERROR);
            return;
        }

        /* Per this project owner's own worked example: uppercase the
         * displayed filename portion (matching the 8.3 short-name
         * casing fat_encode_short_name() itself stores on disk — see
         * fat_common.c's DAY24 comment — so "TEST.TXT ALREADY EXISTS"
         * reads the same way a real DOS/FAT tool's own uppercase
         * directory listing would) inside the confirmation prompt. */
        terminal_puts(t, "! ", TERM_COL_ERROR);
        {
            /* Print just the filename portion (after the last '/'),
             * uppercased, matching the worked example's "TEST.TXT
             * ALREADY EXISTS" (not "IDE0/TEST.TXT ALREADY EXISTS"). */
            const char *slash = strchr(path, '/');
            const char *fname = slash ? slash + 1 : path;
            char upname[SHELL_ARG_LEN];
            uint32_t k = 0;
            while (fname[k] != '\0' && k < sizeof(upname) - 1) {
                char c = fname[k];
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
                upname[k] = c;
                k++;
            }
            upname[k] = '\0';
            terminal_puts(t, upname, TERM_COL_ERROR);
        }
        terminal_puts(t, " ALREADY EXISTS\n", TERM_COL_ERROR);
        terminal_puts(t, "! DO YOU WANT TO COVER THE FILE?\n", TERM_COL_ERROR);
        terminal_puts(t, " - Y/N - ", TERM_COL_PROMPT);

        /* Flush what's been queued so far to the real screen before
         * blocking on a keypress — see this file's own header comment
         * ("WHY DIRECT console_* CALLS") for why terminal_puts() alone
         * would otherwise leave this prompt invisible until AFTER the
         * keypress it's asking for. text_shell_flush_new_lines() itself
         * is kmain.c-local (static) and not reachable from here, so
         * this function does the equivalent flush itself: walk
         * g_text_shell would require reaching into kmain.c's own
         * static state, which this file has no access to and should
         * not reach into — instead, print this prompt a SECOND time,
         * this time through the same direct console_* path the editor
         * itself uses, then let the terminal_puts() calls above simply
         * be redundantly re-flushed (harmlessly re-printed) once
         * shell_exec() eventually returns. */
        console_write_color("! ", COLOR_ERROR);
        {
            const char *slash = strchr(path, '/');
            const char *fname = slash ? slash + 1 : path;
            char upname[SHELL_ARG_LEN];
            uint32_t k = 0;
            while (fname[k] != '\0' && k < sizeof(upname) - 1) {
                char c = fname[k];
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
                upname[k] = c;
                k++;
            }
            upname[k] = '\0';
            console_write_color(upname, COLOR_ERROR);
        }
        console_write_color(" ALREADY EXISTS\n", COLOR_ERROR);
        console_write_color("! DO YOU WANT TO COVER THE FILE?\n", COLOR_ERROR);
        console_write_color(" - Y/N - ", COLOR_PROMPT);

        int yes = write_read_y_n();
        console_putc('\n');

        if (!yes) {
            terminal_puts(t, "Cancelled.\n", TERM_COL_INFO);
            console_write_color("Cancelled.\n", COLOR_INFO);
            return;
        }
    } else if (open_err == FAT_ERR_NOT_FOUND) {
        /* Doesn't exist yet — create it first, exactly like typing
         * `create` before `write`, per this file's own header comment
         * ("creates it... exactly like typing `create` first"). */
        int rc = fat_create_file(path);
        if (rc != FAT_OK) {
            terminal_puts(t, "write: cannot create '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' (", TERM_COL_ERROR);
            terminal_puts(t, fat_strerror(rc), TERM_COL_ERROR);
            terminal_puts(t, ")\n", TERM_COL_ERROR);
            return;
        }
    } else {
        terminal_puts(t, "write: cannot open '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' (", TERM_COL_ERROR);
        terminal_puts(t, fat_strerror(open_err), TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
        return;
    }

    /* ---- Interactive multi-line editor ---- */
    static char lines[WRITE_MAX_LINES][WRITE_MAX_LINE_LEN + 1];
    static int  line_lens[WRITE_MAX_LINES];
    int num_lines = 1;
    int cur_row = 0, cur_col = 0;

    for (int i = 0; i < WRITE_MAX_LINES; i++)
        line_lens[i] = 0;

    /* Record where the editor begins on the real screen — every
     * redraw is relative to this fixed row, per write_redraw()'s own
     * comment on why this function never lets the console scroll out
     * from under that assumption. */
    int start_y = console_get_y();
    if (start_y != 0)
        console_putc('\n');   /* start the editor on its own fresh line */
    start_y = console_get_y();

    write_redraw(path, lines, line_lens, num_lines, cur_row, cur_col, start_y);

    for (;;) {
        char c = keyboard_getchar();

        if (c == (char)KEY_CTRL_S) {
            break;   /* save & exit */
        } else if (c == (char)KEY_UP) {
            if (cur_row > 0) {
                cur_row--;
                if (cur_col > line_lens[cur_row])
                    cur_col = line_lens[cur_row];
            }
        } else if (c == (char)KEY_DOWN) {
            if (cur_row < num_lines - 1) {
                cur_row++;
                if (cur_col > line_lens[cur_row])
                    cur_col = line_lens[cur_row];
            }
        } else if (c == (char)KEY_LEFT) {
            if (cur_col > 0)
                cur_col--;
        } else if (c == (char)KEY_RIGHT) {
            if (cur_col < line_lens[cur_row])
                cur_col++;
        } else if (c == '\r' || c == '\n' || c == (char)KEY_ENTER) {
            /* Insert a new line after cur_row, splitting the current
             * line's content at cur_col — same "everything after the
             * cursor moves to the new line" behavior any text editor's
             * Enter key has. Only if there's still room under
             * WRITE_MAX_LINES; silently ignored otherwise (this
             * project's own SCOPE LIMITS, see this file's header
             * comment — no error message for hitting the cap, matching
             * how terminal_t's own TERM_INPUT_MAX simply stops
             * accepting more characters rather than erroring, kernel/
             * gui/window1.c's terminal_handle_key()). */
            if (num_lines < WRITE_MAX_LINES) {
                for (int i = num_lines; i > cur_row + 1; i--) {
                    line_lens[i] = line_lens[i - 1];
                    memcpy(lines[i], lines[i - 1], (uint32_t)line_lens[i]);
                }
                int tail_len = line_lens[cur_row] - cur_col;
                if (tail_len < 0) tail_len = 0;
                memcpy(lines[cur_row + 1], &lines[cur_row][cur_col], (uint32_t)tail_len);
                line_lens[cur_row + 1] = tail_len;
                line_lens[cur_row] = cur_col;
                num_lines++;
                cur_row++;
                cur_col = 0;
            }
        } else if (c == '\b' || c == (char)KEY_BS) {
            if (cur_col > 0) {
                memmove(&lines[cur_row][cur_col - 1], &lines[cur_row][cur_col],
                         (uint32_t)(line_lens[cur_row] - cur_col));
                line_lens[cur_row]--;
                cur_col--;
            } else if (cur_row > 0) {
                /* Merge current line into the end of the previous one,
                 * then remove this line and shift every later line up
                 * — same "backspace at column 0 joins lines" behavior
                 * this file's own header comment documents. */
                int prev_len = line_lens[cur_row - 1];
                int this_len = line_lens[cur_row];
                int room = WRITE_MAX_LINE_LEN - prev_len;
                int take = (this_len < room) ? this_len : room;

                memcpy(&lines[cur_row - 1][prev_len], lines[cur_row], (uint32_t)take);
                line_lens[cur_row - 1] = prev_len + take;

                for (int i = cur_row; i < num_lines - 1; i++) {
                    line_lens[i] = line_lens[i + 1];
                    memcpy(lines[i], lines[i + 1], (uint32_t)line_lens[i]);
                }
                num_lines--;
                cur_row--;
                cur_col = prev_len;
            }
        } else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
            if (line_lens[cur_row] < WRITE_MAX_LINE_LEN) {
                memmove(&lines[cur_row][cur_col + 1], &lines[cur_row][cur_col],
                         (uint32_t)(line_lens[cur_row] - cur_col));
                lines[cur_row][cur_col] = c;
                line_lens[cur_row]++;
                cur_col++;
            }
        }
        /* Any other key (function keys, Home/End/PgUp/PgDn, unhandled
         * Ctrl combos, ...) is silently ignored — same "not every key
         * does something" tolerance terminal_handle_key() itself
         * already has for keys it doesn't recognize. */

        write_redraw(path, lines, line_lens, num_lines, cur_row, cur_col, start_y);
    }

    /* ---- Flatten the multi-line buffer into one '\n'-joined byte
     * string and persist it via fat_write() — see that function's own
     * doc comment (fat_vfs.h) for the full overwrite-from-0 contract
     * this relies on. ---- */
    static uint8_t content[WRITE_MAX_LINES * (WRITE_MAX_LINE_LEN + 1)];
    uint32_t total = 0;
    for (int i = 0; i < num_lines; i++) {
        memcpy(&content[total], lines[i], (uint32_t)line_lens[i]);
        total += (uint32_t)line_lens[i];
        if (i < num_lines - 1)
            content[total++] = '\n';
    }

    /* Move the real cursor past the editor's own on-screen area before
     * printing the final result, so this doesn't overwrite the last
     * content row. WRITE_MAX_LINES rows of content + 2 header rows. */
    console_set_cursor(0, start_y + 2 + WRITE_MAX_LINES);

    int written = fat_write(path, content, total);
    if (written < 0) {
        terminal_puts(t, "write: cannot write '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' (", TERM_COL_ERROR);
        terminal_puts(t, fat_strerror(written), TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
        console_write_color("write: failed to save\n", COLOR_ERROR);
        return;
    }

    terminal_puts(t, "wrote ", TERM_COL_SUCCESS);
    {
        char numbuf[16];
        sprintf(numbuf, "%d", written);
        terminal_puts(t, numbuf, TERM_COL_SUCCESS);
    }
    terminal_puts(t, " bytes to ", TERM_COL_SUCCESS);
    terminal_puts(t, path, TERM_COL_SUCCESS);
    terminal_newline(t);

    console_write_color("wrote ", COLOR_SUCCESS);
    {
        char numbuf[16];
        sprintf(numbuf, "%d", written);
        console_write_color(numbuf, COLOR_SUCCESS);
    }
    console_write_color(" bytes to ", COLOR_SUCCESS);
    console_write_color(path, COLOR_SUCCESS);
    console_putc('\n');
}
