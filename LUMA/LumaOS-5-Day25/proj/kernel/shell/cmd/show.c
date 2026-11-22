/* kernel/shell/cmd/show.c
 * LumaOS 5.0 - Shell command: show
 *
 * DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘"):
 *   "添加错误处理（文件不存在、簇链断裂）。关闭文件功能（fat_close）。
 *    编写测试用例：更改 cat 命令名称为 show，功能不变，一个根目录下的
 *    文本文件（用 Python 提前写入）。"
 *
 *   This file IS Day 19's kernel/shell/cmd/cat.c, renamed per this
 *   day's own acceptance text ("更改cat命令名称为show，功能不变" — "rename
 *   the cat command to show, behavior unchanged"). Every DAY19/DAY20
 *   design decision documented in the old cat.c header comment still
 *   applies unchanged (why "<mount-slot>/<filename>", why
 *   terminal_putchar() not terminal_puts(), why CAT_CHUNK_BYTES=256 and
 *   deliberately not cluster-aligned, why no total-bytes cap) — none of
 *   that reasoning is re-derived here, only re-applied under the new
 *   name (CAT_CHUNK_BYTES -> SHOW_CHUNK_BYTES, cmd_cat -> cmd_show).
 *   See kernel/shell/shell.c's registration line for the "cat" -> "show"
 *   command-table change and build.sh for the "cat.c" -> "show.c" build
 *   rename.
 *
 *   What's actually NEW this day, on top of the rename:
 *
 *   1. FRIENDLIER ERROR DIAGNOSTICS ("文件不存在、簇链断裂"): fat_open()
 *      and fs->read() (-> fat_read()) now return one of fat_error_t's
 *      specific negative codes instead of a flat -1 (see fat_vfs.h's
 *      DAY21 doc comments) — this file now reports WHICH failure
 *      happened via fat_strerror(), rather than a single generic
 *      "not found" message that used to cover every failure case
 *      (including ones that were never actually "not found", like a
 *      broken cluster chain discovered mid-read). Compare this to Day
 *      19/20's cat.c, which only ever printed "(not found)" for
 *      fat_open() failing and a bare "cat: read error after N bytes"
 *      for fs->read() failing, with no way to tell a broken chain from
 *      a real disk I/O failure from bad arguments.
 *
 *   2. fat_close(): this command now calls `node.fs->close(&file)`
 *      once it's done reading, mirroring the open()/read()/close()
 *      shape a real OS shell's `cat`/`show`/`type` would use. Nothing
 *      is actually FREED by this (see vfs_close_fn's doc comment,
 *      kernel/fs/vfs/vfs.h, for why — no kernel heap yet), but it
 *      invalidates `file` (`file.node` becomes NULL) so this function
 *      can't accidentally keep reading from it after the point where a
 *      real "closed" file should refuse further reads — and it
 *      exercises the new `fs->close` op through the one real caller
 *      that has a `file_t` to close, the same way this command has
 *      always been the real exerciser of `fs->open`/`fs->read`.
 * --------------------------------------------------------------------- */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../shell.h"

#define SHOW_CHUNK_BYTES  256

void cmd_show(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: show <mount-slot>/<filename>  "
                          "(e.g. show IDE0/BIGFILE.BIN)\n", TERM_COL_ERROR);
        return;
    }

    const char *path = argv[1];

    /* DAY21: fat_open() now returns a specific fat_error_t code
     * (FAT_ERR_NOT_FOUND / FAT_ERR_INVALID_ARGS) rather than a flat
     * -1 — see fat_vfs.h's doc comment on fat_open() for exactly which
     * path-shape/lookup failure produces which code. fat_strerror()
     * turns whichever one came back into a short human-readable tail
     * for the error line, replacing Day 19/20's hardcoded
     * "(not found)" (which used to be printed even for a malformed
     * path, not just a genuinely missing file). */
    vfs_node_t node;
    int open_err = fat_open(path, &node);
    if (open_err != FAT_OK) {
        terminal_puts(t, "show: cannot open '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' (", TERM_COL_ERROR);
        terminal_puts(t, fat_strerror(open_err), TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
        return;
    }

    if (node.type != VFS_TYPE_FILE) {
        terminal_puts(t, "show: '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' is a directory\n", TERM_COL_ERROR);
        return;
    }

    if (!node.fs || !node.fs->open) {
        terminal_puts(t, "show: mounted filesystem has no open op\n",
                      TERM_COL_ERROR);
        return;
    }

    /* fs->open() (fat12_vfs_open()/fat16_vfs_open()/fat32_vfs_open(),
     * all three identical — see fat12_vfs.c's DAY13 comment) resets
     * file.pos to 0 and refuses a non-file node, mirroring the same
     * check just above; calling it anyway (rather than hand-building
     * a file_t here) keeps `show` going through the same open/read/
     * close triple every other VFS consumer would use. */
    file_t file;
    if (node.fs->open(&node, &file) != 0) {
        terminal_puts(t, "show: open failed for '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "'\n", TERM_COL_ERROR);
        return;
    }

    terminal_puts(t, "-- ", TERM_COL_INFO);
    terminal_puts(t, path, TERM_COL_INFO);
    {
        char numbuf[16];
        sprintf(numbuf, "%u", node.size);
        terminal_puts(t, " (", TERM_COL_INFO);
        terminal_puts(t, numbuf, TERM_COL_INFO);
        terminal_puts(t, " bytes) --", TERM_COL_INFO);
    }
    terminal_newline(t);

    /* DAY24-FOLLOWUP: a 0-byte file (start_cluster == 0 — exactly what
     * fat_create_file() now produces, see fat_common.h's doc comment
     * on fat_create_entry_fixed_root(): "起始簇 0...大小 0") has no
     * data cluster to chain-walk at all — fs->read() (-> fat_read())
     * correctly refuses that as FAT_ERR_INVALID_ARGS rather than
     * misreading cluster 0 as if it were a real data cluster (see
     * fat_vfs.h's own doc comment on fat_read(): "the starting cluster
     * is 0 (a 0-length file has nothing to chain-walk... a 0-byte file
     * is spec-legal)"). Before Day 24, this project had no way to
     * PRODUCE a 0-byte file at all (every fixture file always had real
     * content), so this case was spec-legal but unreachable — `show`
     * never needed to special-case it. Day 24 changes that: `show` on
     * a freshly `create`d file must print a legitimate empty-file
     * result, not fs->read()'s own (still-correct) invalid-args error.
     * This check is placed here, in `show` itself, rather than
     * loosening fat_read()'s own contract, since "nothing to read" is
     * a `show`-level EOF-shaped outcome, not a change to what
     * fat_read() is allowed to assume about a nonzero start_cluster. */
    if (node.size == 0) {
        terminal_puts(t, "-- read 0 bytes (whole file) --\n", TERM_COL_INFO);
        if (node.fs->close)
            node.fs->close(&file);
        return;
    }

    if (!node.fs->read) {
        terminal_puts(t, "show: mounted filesystem has no read op\n",
                      TERM_COL_ERROR);
        return;
    }

    /* Loop fs->read() until EOF (0) or error (<0) — see the old
     * cat.c's DAY20 comment (reproduced in spirit in this file's own
     * header comment above) for why chunked, non-cluster-aligned reads
     * are used rather than one size-of-file call. DAY21: `read_err`
     * now captures the SPECIFIC negative fat_error_t code fs->read()
     * returned, instead of Day 19/20's bare `had_error` flag, so the
     * failure message below can say WHY the read stopped. */
    static uint8_t buf[SHOW_CHUNK_BYTES];
    uint32_t total_read = 0;
    int read_err = FAT_OK;

    for (;;) {
        int n = node.fs->read(&file, buf, SHOW_CHUNK_BYTES);
        if (n < 0) {
            read_err = n;
            break;
        }
        if (n == 0)
            break;   /* EOF, per vfs_read_fn's "0 == EOF" contract */

        /* Print exactly what was read, byte for byte — see this
         * file's header comment (and the old cat.c's DAY19 comment it
         * carries forward) for why terminal_putchar() (not
         * terminal_puts()) is used: file content is not a
         * NUL-terminated C string. */
        for (int i = 0; i < n; i++)
            terminal_putchar(t, (char)buf[i], TERM_COL_DEFAULT);

        total_read += (uint32_t)n;
    }
    terminal_newline(t);

    if (read_err != FAT_OK) {
        /* DAY21: name the specific failure (e.g. "cluster chain
         * broken" vs "I/O error") instead of Day 19/20's generic
         * "read error after N bytes" — this is the direct payoff of
         * fat_read_chain() (kernel/fs/fat/fat_vfs.c) now telling
         * FAT_ERR_IO and FAT_ERR_CHAIN_BROKEN apart. */
        terminal_puts(t, "show: read error after ", TERM_COL_ERROR);
        {
            char numbuf[16];
            sprintf(numbuf, "%u", total_read);
            terminal_puts(t, numbuf, TERM_COL_ERROR);
        }
        terminal_puts(t, " bytes (", TERM_COL_ERROR);
        terminal_puts(t, fat_strerror(read_err), TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
    } else {
        char numbuf[16];
        sprintf(numbuf, "%u", total_read);
        terminal_puts(t, "-- read ", TERM_COL_INFO);
        terminal_puts(t, numbuf, TERM_COL_INFO);
        if (total_read < node.size)
            terminal_puts(t, " bytes (truncated preview) --", TERM_COL_INFO);
        else
            terminal_puts(t, " bytes (whole file) --", TERM_COL_INFO);
        terminal_newline(t);
    }

    /* DAY21 fat_close(): whether the loop above ended in EOF or an
     * error, `file` is still a valid open handle at this point (fs->
     * open() succeeded, see above) and this is the one place `show`
     * is done with it — close it on EVERY exit path from here on
     * (success or read error alike), not just the success path, so a
     * `file_t` this command opened never leaks out "open" regardless
     * of how reading it went. Checked for NULL first, same discipline
     * as every other `node.fs->X` call above: `close` is a legal-to-
     * be-NULL op per vfs_close_fn's own doc comment (kernel/fs/vfs/
     * vfs.h), even though every FAT variant this project has today
     * always sets it (see fat12_vfs.c/fat16_vfs.c/fat32_vfs.c's DAY21
     * `.close =` lines). */
    if (node.fs->close)
        node.fs->close(&file);
}