/* kernel/shell/cmd/cd.c
 * LumaOS 5.0 - Shell command: cd
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "支持 cd 命令：切换当前工作目录（在挂载点之间）。"
 *
 *   This project has no subdirectories yet — every fat_open()/
 *   fat_create_file()/fat_write() path is still exactly one
 *   "<mount-slot>/<filename>" segment (真正的多级目录 is Day 42's job,
 *   still far ahead of today's roadmap position — see fat_open()'s own
 *   doc comment in fat_vfs.h, "path doesn't have a second '/'" for the
 *   current hard limit). So there is no directory TREE for `cd` to
 *   descend into today — the only thing "当前工作目录" can mean at
 *   this point in the roadmap is WHICH MOUNTED VOLUME (IDE0/IDE1/IDE2)
 *   a bare, mount-slot-less filename defaults onto, which is exactly
 *   what vfs_root already IS: kmain.c's text_shell_prompt() already
 *   derives its "IDE0/ >"-style prompt from vfs_mount_name_of(vfs_root
 *   ->fs) (see that function's own DAY14 comment predicting `cd` would
 *   be "a one-line change" for exactly this reason), and shell.c's
 *   shell_resolve_path() (DAY25) already defaults a bare filename onto
 *   that same vfs_root. So `cd IDE0` here just needs to reassign
 *   vfs_root itself to point at IDE0's own filesystem_t/root node —
 *   every other DAY25 piece (`write test.txt`, `read test.txt`, and
 *   the Shell prompt) already keys off vfs_root and needs no changes
 *   of its own to "see" a cd.
 *
 * ARGUMENT SYNTAX: `cd IDE0` or `cd IDE1/` (a trailing '/' is accepted
 * and stripped — this project's own namespace convention (计划.md's
 * file-tree diagram, and vfs_node_t's own doc comment on why stored
 * names are slash-free) always DISPLAYS a mount/directory with a
 * trailing '/', so a user copying that displayed form back in as an
 * argument, e.g. after seeing "IDE1/ >" printed, should not be
 * rejected for including the very slash this project's own prompt
 * just showed them).
 *
 * Returns/prints exactly the transcript this day's own acceptance
 * text specifies:
 *
 *   IDE2/ >cd IDE0
 *    - Changed directory to IDE0/
 *   IDE0/ >cd IDE1/
 *    - Changed directory to IDE1/
 *   IDE1/ >
 *
 * An unknown mount name (no such slot registered — vfs_lookup_mount()
 * returns NULL, kernel/fs/vfs/vfs.c) leaves vfs_root untouched and
 * prints an error instead, the same "don't apply a change that didn't
 * fully succeed" discipline fat_create_file()/fat_write() already
 * follow for their own failure paths.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../shell.h"

void cmd_cd(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: cd <mount-slot>  (e.g. cd IDE0)\n",
                       TERM_COL_ERROR);
        return;
    }

    /* Strip a single trailing '/', per this file's header comment
     * above — "cd IDE1/" and "cd IDE1" are the same request. A small
     * fixed buffer is enough: mount-slot names are "IDE" + a handful
     * of digits (vfs_mount_name_of()'s own doc comment), nowhere near
     * this size. */
    static char slot_buf[16];
    uint32_t i = 0;
    while (argv[1][i] != '\0' && i < sizeof(slot_buf) - 1) {
        slot_buf[i] = argv[1][i];
        i++;
    }
    slot_buf[i] = '\0';
    if (i > 0 && slot_buf[i - 1] == '/')
        slot_buf[i - 1] = '\0';

    filesystem_t *fs = vfs_lookup_mount(slot_buf);
    if (!fs || !fs->root) {
        terminal_puts(t, "cd: no such mounted volume '", TERM_COL_ERROR);
        terminal_puts(t, slot_buf, TERM_COL_ERROR);
        terminal_puts(t, "'\n", TERM_COL_ERROR);
        return;
    }

    vfs_root = fs->root;

    terminal_puts(t, " - Changed directory to ", TERM_COL_SUCCESS);
    terminal_puts(t, slot_buf, TERM_COL_SUCCESS);
    terminal_puts(t, "/\n", TERM_COL_SUCCESS);
}
