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
 * cross-validated byte-for-byte in this project's own comments).
 *
 * WHY BYTES ARE PRINTED ONE AT A TIME VIA terminal_putchar(), NOT
 * terminal_puts(): file content is arbitrary binary data, not a C
 * string — it can (and, for a real KERNEL.BIN-shaped file, will)
 * contain embedded 0x00 bytes. terminal_puts() walks its argument
 * until a NUL, so handing it a raw file buffer would silently
 * truncate output at the first embedded zero. terminal_putchar() has
 * no such assumption — every byte read is rendered as one glyph
 * (whatever glyph the VGA font table has at that byte value;
 * genuinely unprintable values just look like garbage on screen,
 * which is an explicitly-accepted outcome here), except the handful
 * terminal_putchar() itself special-cases as control characters (\n,
 * \r, \b, \t — see window1.c's own doc comment on terminal_putchar())
 * which move the cursor instead of rendering a glyph; a real binary
 * file can contain those bytes too, and letting them do their normal
 * cursor thing rather than crashing or being filtered out is
 * consistent with "just show me the raw bytes" being the point of
 * this command.
 *
 * DAY20 (Week 3, Day 20 of the 5.0 filesystem roadmap):
 *   "增加文件偏移指针 file->pos，支持多次读取。连续调用两次 read，第
 *    二次能接着上次位置读。"
 *
 *   `file->pos` itself, and fat_read()'s respecting of it, were both
 *   already in place since Day 19 (see vfs.h's `struct file` doc
 *   comment and fat_vfs.h's fat_read() doc comment — Day 19 wrote
 *   fat_read_chain() to already skip whole clusters up to file_pos
 *   "so Day 20 is a caller-side change only"). Day 19's cmd_cat,
 *   though, only ever made ONE fs->read() call, capped at a fixed
 *   preview size, specifically because Day 19's own acceptance text
 *   only asked for "开头部分" (the beginning part) — so through Day 19,
 *   file->pos only ever went from 0 to (at most) that one cap in this
 *   command, never exercising a SECOND fs->read() call at all, let
 *   alone one whose starting file->pos lands past the file's first
 *   cluster.
 *
 *   This is exactly the caller-side change Day 19's fat_vfs.h doc
 *   comment predicted: cmd_cat now loops, calling node.fs->read()
 *   repeatedly with a fixed-size chunk buffer until it returns 0 (EOF,
 *   per vfs_read_fn's "0 == EOF" contract — see vfs.h) or a negative
 *   value (read error), printing each chunk as it arrives instead of
 *   one capped chunk. Every call after the first starts at whatever
 *   file->pos the PREVIOUS call left it at (fat_read() advances it
 *   internally — see fat_vfs.c's fat_read()), so for any file bigger
 *   than CAT_CHUNK_BYTES this now genuinely exercises "连续调用...
 *   第二次能接着上次位置读" through the real shell command, not just
 *   kmain.c's boot-time self-test (see kmain.c's DAY20 section for the
 *   deeper cluster-boundary-crossing version of this same check).
 *
 * WHY CAT_CHUNK_BYTES IS 256, NOT ONE size-of-file READ: this project
 * has no kernel heap yet (see fat12_vfs.h's DAY13 design notes), so
 * any buffer here is either a bounded stack/static array or nothing —
 * a single "read the whole file in one call" would need a buffer as
 * large as the largest file this project might ever open, an
 * unbounded requirement a fixed-size buffer can't satisfy. Chunking
 * decouples the buffer size from the file size entirely: ANY file
 * length works with the same small 256-byte static buffer, at the
 * cost of more (but not larger) fs->read() calls — which is precisely
 * the behavior Day 20 exists to prove works. 256 is deliberately NOT a
 * multiple of this project's own 512-byte cluster size (see
 * fat_vfs.c's DAY19 comment on FAT_READ_MAX_CLUSTER_BYTES for why
 * every fixture here uses 512-byte clusters) — a 256-byte chunk against
 * a 512-byte cluster means most chunk boundaries do NOT line up with
 * cluster boundaries, so this loop's repeated fs->read() calls
 * regularly land file->pos mid-cluster, not just exactly at cluster 0
 * of some later cluster. That is a strictly harder case for
 * fat_read_chain()'s "skip whole clusters, then start copying at
 * offset_in_cluster" logic (kernel/fs/fat/fat_vfs.c) than an aligned
 * chunk size would be, so exercising it here through the real shell
 * command adds coverage kmain.c's own cluster-aligned-vs-not mix (see
 * kmain.c's DAY20 section) is also deliberately designed around.
 *
 * WHY NO CAP ON TOTAL BYTES PRINTED ANYMORE: with the loop above, the
 * old CAT_PREVIEW_BYTES ceiling has no job left to do — a bounded
 * chunk buffer plus "loop until EOF" already handles files of any
 * size without needing a second, separate "don't read past N total
 * bytes" limit. The terminal_t scrollback (TERM_BUF_ROWS=400 lines,
 * gui.h) is large enough for every fixture this project mounts today
 * (BIGFILE.BIN's 10240 bytes is well under 400 * TERM_BUF_COLS=120 =
 * 48000 character-cells of scrollback) — a real "huge file" truncation
 * safeguard is future work for whenever a file that large actually
 * exists to open.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../shell.h"

#define CAT_CHUNK_BYTES  256

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

    /* DAY20 payoff: loop fs->read() until EOF (0) or error (<0),
     * instead of Day 19's single capped call. `file.pos` (advanced
     * internally by fat_read() on every call — see fat_vfs.c) is what
     * makes each successive call pick up exactly where the previous
     * one left off; this loop is the "caller-side change only" Day
     * 19's fat_vfs.h doc comment predicted. */
    static uint8_t buf[CAT_CHUNK_BYTES];
    uint32_t total_read = 0;
    int had_error = 0;

    for (;;) {
        int n = node.fs->read(&file, buf, CAT_CHUNK_BYTES);
        if (n < 0) {
            had_error = 1;
            break;
        }
        if (n == 0)
            break;   /* EOF, per vfs_read_fn's "0 == EOF" contract */

        /* Print exactly what was read, byte for byte — see this
         * file's header comment for why terminal_putchar() (not
         * terminal_puts()) is used: file content is not a
         * NUL-terminated C string. */
        for (int i = 0; i < n; i++)
            terminal_putchar(t, (char)buf[i], TERM_COL_DEFAULT);

        total_read += (uint32_t)n;
    }
    terminal_newline(t);

    if (had_error) {
        terminal_puts(t, "cat: read error after ", TERM_COL_ERROR);
        {
            char numbuf[16];
            sprintf(numbuf, "%u", total_read);
            terminal_puts(t, numbuf, TERM_COL_ERROR);
        }
        terminal_puts(t, " bytes\n", TERM_COL_ERROR);
        return;
    }

    {
        char numbuf[16];
        sprintf(numbuf, "%u", total_read);
        terminal_puts(t, "-- read ", TERM_COL_INFO);
        terminal_puts(t, numbuf, TERM_COL_INFO);
        if (total_read < node.size)
            terminal_puts(t, " bytes (truncated preview) --", TERM_COL_INFO);
        else
            terminal_puts(t, " bytes (whole file) --", TERM_COL_INFO);
    }
    terminal_newline(t);
}