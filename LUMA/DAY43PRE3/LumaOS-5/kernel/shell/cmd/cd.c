/* kernel/shell/cmd/cd.c
 * LumaOS 5.0 - Shell command: cd
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "支持 cd 命令：切换当前工作目录（在挂载点之间）。"
 *
 *   At that point this project had no subdirectories, so the only thing
 *   "当前工作目录" could mean was WHICH MOUNTED VOLUME (IDE0/IDE1/IDE2)
 *   a bare filename defaults onto — which is exactly what vfs_root
 *   already IS (kmain.c's prompt and shell_resolve_path() both key off
 *   it), so `cd IDE0` was just "reassign vfs_root".
 *
 * DAY42 (Week 6, Day 42 of the 5.0 filesystem roadmap):
 *   "多级目录与路径解析。" LMFS now has real subdirectories
 *   (lmfs_mkdir(), kernel/fs/lmfs/lmfs.c), so "当前工作目录" gets its
 *   second coordinate: the directory INSIDE the volume. vfs_root is
 *   still the volume; the new part is the cwd string kept in shell.c
 *   (shell_get_cwd()/shell_set_cwd()). cd now accepts everything
 *   shell_resolve_path() does:
 *
 *     cd IDE3            switch volume, land in its root   (as before)
 *     cd IDE1/           same, trailing '/' accepted       (as before)
 *     cd docs            into a subdirectory of the current directory
 *     cd docs/notes      several levels at once
 *     cd ..   cd ../x    up / sideways ('..' stops at the volume root)
 *     cd /               volume root of the current volume
 *     cd IDE3/docs       switch volume AND directory in one go
 *
 *   Transcript (matching the Day 25 format, with the new longer path):
 *
 *     IDE3/ > cd docs
 *      - Changed directory to IDE3/docs/
 *     IDE3/docs/ > cd ..
 *      - Changed directory to IDE3/
 *
 *   A failed cd (no such directory, target is a file, FAT volume asked
 *   to enter a subdirectory) leaves vfs_root and the cwd untouched —
 *   the same "don't apply a change that didn't fully succeed" rule as
 *   always.
 *
 *   FAT volumes still have no subdirectories (see mkdir.c's header), so
 *   `cd` on a FAT volume only ever accepts the volume root itself.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/lmfs/lmfs_vfs.h"
#include "../../lib/string.h"
#include "../shell.h"

void cmd_cd(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: cd <path>  "
                          "(e.g. cd IDE0, cd docs, cd .., cd /)\n",
                       TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "cd: bad path '", TERM_COL_ERROR);
        terminal_puts(t, argv[1], TERM_COL_ERROR);
        terminal_puts(t, "' (no current filesystem, or path too long)\n",
                       TERM_COL_ERROR);
        return;
    }

    filesystem_t *fs = (filesystem_t *)0;
    const char *sub = "";
    if (shell_split_path(path_buf, &fs, &sub) != 0 || !fs || !fs->root) {
        terminal_puts(t, "cd: no such mounted volume in '", TERM_COL_ERROR);
        terminal_puts(t, path_buf, TERM_COL_ERROR);
        terminal_puts(t, "'\n", TERM_COL_ERROR);
        return;
    }

    if (sub[0] != '\0') {
        /* A subdirectory: only LMFS has them, and the target must
         * really exist and really be a directory. lmfs_vfs_open_path()
         * resolves the whole multi-level path (lmfs_resolve_path()) and
         * hands back a node whose .type says file-or-directory. */
        if (!fs->name || strcmp(fs->name, "lmfs") != 0) {
            terminal_puts(t, "cd: '", TERM_COL_ERROR);
            terminal_puts(t, path_buf, TERM_COL_ERROR);
            terminal_puts(t, "': no such directory (this volume has no "
                              "subdirectories)\n", TERM_COL_ERROR);
            return;
        }
        vfs_node_t node;
        if (lmfs_vfs_open_path(path_buf, &node) != 0) {
            terminal_puts(t, "cd: '", TERM_COL_ERROR);
            terminal_puts(t, path_buf, TERM_COL_ERROR);
            terminal_puts(t, "': no such directory\n", TERM_COL_ERROR);
            return;
        }
        if (node.type != VFS_TYPE_DIR) {
            terminal_puts(t, "cd: '", TERM_COL_ERROR);
            terminal_puts(t, path_buf, TERM_COL_ERROR);
            terminal_puts(t, "': not a directory\n", TERM_COL_ERROR);
            return;
        }
        /* cwd string is capped at SHELL_CWD_MAX: check BEFORE touching
         * vfs_root so a too-deep path fails atomically. */
        if (strlen(sub) >= 64) {
            terminal_puts(t, "cd: path too long\n", TERM_COL_ERROR);
            return;
        }
    }

    /* Commit: volume first (shell_set_cwd() remembers which volume the
     * string belongs to), then the directory inside it. */
    vfs_root = fs->root;
    shell_set_cwd(sub);

    terminal_puts(t, " - Changed directory to ", TERM_COL_SUCCESS);
    terminal_puts(t, path_buf, TERM_COL_SUCCESS);
    if (sub[0] != '\0')
        terminal_putchar(t, '/', TERM_COL_SUCCESS);   /* root path already
                                                         ends in '/' */
    terminal_newline(t);
}
