/* kernel/shell/cmd/ls.c
 * LumaOS 5.0 - Shell command: ls
 *
 * DAY14 (Week 2, Day 14 of the 5.0 filesystem roadmap):
 *   "整合 Shell：新增命令 cmd_ls（调用 vfs_readdir 列出文件）。在终端
 *    输入 ls，能看到 FAT 镜像里的所有文件。"
 *
 *   This is the payoff of Day 8's vfs.h shapes, Day 13's vfs_mount()/
 *   fat12_vfs_mount() wiring, and every FAT12 Day before it: `ls` is
 *   the FIRST piece of shell-visible, user-typed code in this project
 *   that walks the filesystem exclusively through the generic VFS
 *   layer (vfs_root->fs->readdir()) — never fat12_readdir() or any
 *   other backend-specific call by name. kmain.c's own DAY13 test
 *   section already proved this exact call sequence works before this
 *   file existed; cmd_ls only wraps it as an interactive command and
 *   renders the result through terminal_puts() instead of printf().
 *
 * WHY THIS DOESN'T CALL fat12_readdir() DIRECTLY:
 *   The whole point of Day 8/13's abstraction is that shell commands
 *   should not need to know which filesystem is mounted. Once FAT16/
 *   32 (Week 3) or LMFS (Month 2) exist, `ls` keeps working unchanged
 *   as long as whatever is mounted fills in fs->readdir correctly —
 *   see vfs.h's own file header comment ("Shell commands ... are
 *   meant to walk struct vfs_node trees and call through a struct
 *   filesystem's function pointers — never a driver-specific function
 *   by name").
 *
 * WHAT "ls" ACTUALLY LISTS TODAY:
 *   vfs_root is whatever kmain()'s own vfs_mount("IDE0") call resolved
 *   at boot (see kernel/fs/vfs/vfs.c's header comment) — on this
 *   project's stock disk image that is the Day 11 FAT12 test volume
 *   embedded at LBA 5000 (tests/fat12-with-file.img), so `ls` is
 *   expected to print BIGFILE.BIN and SMALL.BIN. It is NOT
 *   mbr.bin/loader.bin/kernel.bin — those are raw, fixed-LBA files
 *   dd'd straight onto the disk outside any filesystem, unrelated to
 *   whatever FAT12 volume gets mounted (see 计划.md's Day 12 recap
 *   note, reproduced in kmain.c/vfs.c's comments, for the full
 *   reasoning on why that's expected and correct, not a bug).
 *
 * WHY A NULL vfs_root (OR A READDIR FAILURE) DOESN'T CRASH:
 *   vfs_mount("IDE0") can fail (kmain.c's own DAY13 section already
 *   handles that case by testing its return value before touching
 *   vfs_root) — if the user somehow reaches a shell prompt with no
 *   filesystem mounted, `ls` prints a clear error through the SAME
 *   terminal_t the rest of the shell already uses instead of
 *   dereferencing a NULL vfs_root or a NULL vfs_root->fs->readdir.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../shell.h"

void cmd_ls(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!vfs_root) {
        terminal_puts(t, "ls: no filesystem mounted (vfs_root is NULL)\n",
                      TERM_COL_ERROR);
        return;
    }
    if (!vfs_root->fs || !vfs_root->fs->readdir) {
        terminal_puts(t, "ls: mounted filesystem has no readdir op\n",
                      TERM_COL_ERROR);
        return;
    }

    vfs_node_t child;
    uint32_t   idx  = 0;
    int        any  = 0;

    while (vfs_root->fs->readdir(vfs_root, idx, &child) == 0) {
        /* Directories get a trailing "/" for display only — the name
         * is stored slash-free (see vfs.h's vfs_node_t.name comment),
         * this is purely cosmetic, same rule kmain.c's DAY13 section
         * already applies when printing vfs_root itself. */
        terminal_puts(t, child.name, TERM_COL_CMD);
        if (child.type == VFS_TYPE_DIR)
            terminal_puts(t, "/", TERM_COL_CMD);

        /* Pad out to a fixed column so the size lines up, same spirit
         * as cmd_help.c's name/description padding. */
        {
            int len = (int)strlen(child.name) + (child.type == VFS_TYPE_DIR ? 1 : 0);
            int pad = 16 - len;
            for (int s = 0; s < pad; s++)
                terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        }

        if (child.type != VFS_TYPE_DIR) {
            char numbuf[16];
            sprintf(numbuf, "%u", child.size);
            terminal_puts(t, numbuf, TERM_COL_DEFAULT);
            terminal_puts(t, " bytes", TERM_COL_DEFAULT);
        }
        terminal_newline(t);

        any = 1;
        idx++;
    }

    if (!any) {
        terminal_puts(t, "(empty)\n", TERM_COL_INFO);
    }
}