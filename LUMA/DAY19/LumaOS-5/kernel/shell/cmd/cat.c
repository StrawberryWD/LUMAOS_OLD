/* kernel/shell/cmd/cat.c
 * LumaOS 5.0 - Shell command: cat
 *
 * DAY19 (Week 3, Day 19 of the 5.0 filesystem roadmap):
 *   "实现 fat_read(file, buf, size)：根据起始簇，依次读取簇链，将数据
 *    拷贝到 buf。实现 cmd_cat /KERNEL.BIN，能在终端打印出内核二进制的
 *    开头部分（乱码也行，证明读到了数据）。"
 *
 *   This is the shell-visible payoff of Day 18's fat_open() (path ->
 *   vfs_node_t, starting cluster in node.fs_private) and this same
 *   day's fat_read() (kernel/fs/fat/fat_vfs.c: cluster-chain walk,
 *   copies real file bytes into a caller buffer) — the same "cmd_X
 *   wraps the VFS layer, never a backend-specific function by name"
 *   shape cmd_ls.c (Day 14) already established for readdir.
 *
 * NOTE ON "/KERNEL.BIN": the roadmap's own example path assumes a
 * single global root and a file literally named KERNEL.BIN — neither
 * holds in this project (see fat_vfs.h's DAY18 doc comment and
 * fat12.h's DAY12 note for the same point already made about
 * fat12_readdir()'s roadmap text): this project's Unix-less namespace
 * needs an explicit "<mount-slot>/<filename>" path (fat_open()'s own
 * contract), and no FAT volume this project mounts contains a file
 * named KERNEL.BIN — kernel.bin is dd'd onto a raw disk-absolute LBA
 * outside any filesystem, not stored as a directory entry. `cat` is
 * therefore written to accept ANY "<mount-slot>/<filename>" argument
 * (e.g. "IDE0/BIGFILE.BIN", the Day 11/18 fixture already
 * cross-validated byte-for-byte in this project's own comments) —
 * proving the read path works end-to-end is Day 19's actual
 * acceptance criterion ("能在终端打印出内核二进制的开头部分（乱码也行，
 * 证明读到了数据）" — garbled output is explicitly fine, the point is
 * proving real bytes were read), not that the literal string
 * "KERNEL.BIN" resolves.
 *
 * WHY BYTES ARE PRINTED ONE AT A TIME VIA terminal_putchar(), NOT
 * terminal_puts(): file content is arbitrary binary data, not a C
 * string — it can (and, for a real KERNEL.BIN-shaped file, will)
 * contain embedded 0x00 bytes. terminal_puts() walks its argument
 * until a NUL, so handing it a raw file buffer would silently
 * truncate output at the first embedded zero instead of printing
 * "the beginning part, garbled or not" as the roadmap asks.
 * terminal_putchar() has no such assumption — every byte read is
 * rendered as one glyph (whatever glyph the VGA font table has at
 * that byte value; genuinely unprintable values just look like
 * garbage on screen, which is the explicitly-accepted outcome here),
 * except the handful terminal_putchar() itself special-cases as
 * control characters (\n, \r, \b, \t — see window1.c's own doc
 * comment on terminal_putchar()) which move the cursor instead of
 * rendering a glyph; a real binary file can contain those bytes too,
 * and letting them do their normal cursor thing rather than crashing
 * or being filtered out is consistent with "just show me the raw
 * bytes" being the whole point of this command today.
 *
 * WHY ONLY THE FIRST CAT_PREVIEW_BYTES BYTES: the roadmap's own
 * acceptance text asks for "开头部分" (the beginning part) specifically,
 * not the whole file — printing all of, say, BIGFILE.BIN's 10240
 * bytes one glyph at a time would scroll far past what a single
 * terminal_t screen (TERM_BUF_ROWS=400 lines, gui.h) usefully shows
 * for a smoke test, and isn't what Day 19 asks to prove. 512 bytes
 * (one cluster on every fixture this project mounts today — see
 * fat_vfs.c's DAY19 comment on FAT_READ_MAX_CLUSTER_BYTES for the
 * per-fixture cluster size) is enough to prove multi-byte reads work
 * and to fit on screen at TERM_BUF_COLS=120 (about 5 visual rows'
 * worth of raw bytes, plus the header/footer lines this command
 * prints around them) without needing scrollback. A real, unbounded
 * `cat` (reading in a loop until fat_read() reports EOF/0) is future
 * work once a command actually needs the WHOLE file — Day 20's
 * "支持多次读取" is exactly what a follow-up, no-cap version of this
 * loop would build on (call fat_read() again; file->pos already
 * carries forward correctly, see fat_vfs.h's fat_read() doc comment).
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../shell.h"

#define CAT_PREVIEW_BYTES  512

void cmd_cat(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: cat <mount-slot>/<filename>  "
                          "(e.g. cat IDE0/BIGFILE.BIN)\n", TERM_COL_ERROR);
        return;
    }

    const char *path = argv[1];

    /* Day 18's path resolver: "<mount-slot>/<filename>" -> a
     * vfs_node_t with fs_private == starting cluster, size == byte
     * length — see fat_vfs.h's fat_open() doc comment for the exact
     * failure cases this -1 covers (unknown mount slot, not found,
     * subdirectory path, malformed path). */
    vfs_node_t node;
    if (fat_open(path, &node) != 0) {
        terminal_puts(t, "cat: cannot open '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' (not found)\n", TERM_COL_ERROR);
        return;
    }

    if (node.type != VFS_TYPE_FILE) {
        terminal_puts(t, "cat: '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' is a directory\n", TERM_COL_ERROR);
        return;
    }

    if (!node.fs || !node.fs->open) {
        terminal_puts(t, "cat: mounted filesystem has no open op\n",
                      TERM_COL_ERROR);
        return;
    }

    /* fs->open() (fat12_vfs_open()/fat16_vfs_open()/fat32_vfs_open(),
     * all three identical — see fat12_vfs.c's DAY13 comment) resets
     * file.pos to 0 and refuses a non-file node, mirroring the same
     * check just above; calling it anyway (rather than hand-building
     * a file_t here) keeps `cat` going through the same open/read
     * pair every other VFS consumer would use. */
    file_t file;
    if (node.fs->open(&node, &file) != 0) {
        terminal_puts(t, "cat: open failed for '", TERM_COL_ERROR);
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

    if (!node.fs->read) {
        terminal_puts(t, "cat: mounted filesystem has no read op\n",
                      TERM_COL_ERROR);
        return;
    }

    /* DAY19 payoff: fs->read() now reaches fat_vfs.c's real fat_read()
     * (fat12_vfs_read()/fat16_vfs_read()/fat32_vfs_read() all forward
     * to it) instead of the Day13/16/17 "always returns 0" stub — this
     * is the first call in this codebase that reads genuine file
     * CONTENT off disk through the VFS layer, not just directory
     * metadata (Day 14's `ls`) or a starting cluster number (Day 18's
     * `fat_open`). */
    static uint8_t buf[CAT_PREVIEW_BYTES];
    uint32_t want = CAT_PREVIEW_BYTES;
    if (want > node.size)
        want = node.size;

    int n = node.fs->read(&file, buf, want);
    if (n < 0) {
        terminal_puts(t, "cat: read error\n", TERM_COL_ERROR);
        return;
    }

    /* Print exactly what was read, byte for byte — see this file's
     * header comment for why terminal_putchar() (not terminal_puts())
     * is used: file content is not a NUL-terminated C string. */
    for (int i = 0; i < n; i++)
        terminal_putchar(t, (char)buf[i], TERM_COL_DEFAULT);
    terminal_newline(t);

    {
        char numbuf[16];
        sprintf(numbuf, "%d", n);
        terminal_puts(t, "-- read ", TERM_COL_INFO);
        terminal_puts(t, numbuf, TERM_COL_INFO);
        if ((uint32_t)n < node.size)
            terminal_puts(t, " bytes (truncated preview) --", TERM_COL_INFO);
        else
            terminal_puts(t, " bytes (whole file) --", TERM_COL_INFO);
    }
    terminal_newline(t);
}