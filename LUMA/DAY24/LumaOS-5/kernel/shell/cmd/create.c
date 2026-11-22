/* kernel/shell/cmd/create.c
 * LumaOS 5.0 - Shell command: create
 *
 * DAY24 (Week 4, Day 24 of the 5.0 filesystem roadmap):
 *   "实现 fat_create_file(name)：在根目录区找一个空条目（首字节 0x00
 *    或 0xE5），填入文件名、起始簇 0、大小 0。在 Shell 中输入 create
 *    test.txt，重新 ls 能看到它。"
 *
 *   This is the Shell-visible exerciser of Day 24's real deliverable,
 *   fat_create_file() (kernel/fs/fat/fat_vfs.c) — the exact same
 *   relationship cmd_ls (Day 14) has to vfs_readdir(), and cmd_show
 *   (Day 19/21) has to fat_open()/fat_read(): the FS-layer function is
 *   where the actual work happens, this file only wraps it as an
 *   interactive command and renders the result through terminal_puts()
 *   instead of the boot-time console's printf().
 *
 * PATH SYNTAX, MATCHING cmd_show's OWN CONVENTION EXACTLY:
 *   "<mount-slot>/<filename>", e.g. `create IDE0/TEST.TXT` — see
 *   fat_vfs.h's doc comment on fat_create_file() for the full
 *   rationale (same mandatory-mount-slot parsing fat_open() already
 *   established Day 18, now shared by every path-taking command). The
 *   roadmap's own acceptance text writes the example as a bare
 *   `create test.txt` (no mount-slot) — but this project has had a
 *   numbered mount table (IDE0/IDE1/IDE2, all three simultaneously
 *   mounted since Day 22/23's own self-tests) since Day 18-FOLLOWUP,
 *   and `show`/`fat_open()` have required an explicit mount-slot ever
 *   since, so `create` follows that same established convention rather
 *   than reintroducing an implicit "whichever volume mounted last"
 *   fallback `show` deliberately does NOT have. See this file's
 *   DAY24-FOLLOWUP note below for the one place this command's
 *   argument handling goes further than the roadmap's literal text for
 *   exactly this reason.
 *
 * DAY24-FOLLOWUP (this file's own small addition, not asked for by
 * 计划.md's Day 24 text itself): if the user really does type a bare
 * filename with no mount-slot segment at all (matching the roadmap's
 * own literal `create test.txt` example), this command defaults it
 * onto vfs_root's own mount slot (via vfs_mount_name_of(vfs_root->fs),
 * the same lookup kmain.c's shell-prompt code already uses to display
 * "IDE2/ >") rather than flatly rejecting it as a malformed path. This
 * is purely a Shell-side convenience — fat_create_file() itself still
 * requires the full "<mount-slot>/<filename>" syntax unchanged, this
 * command just fills in the missing segment before calling it, so
 * `show`'s own stricter behavior (no such fallback) is untouched.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../shell.h"

void cmd_create(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: create <mount-slot>/<filename>  "
                          "(e.g. create IDE0/TEST.TXT)\n", TERM_COL_ERROR);
        return;
    }

    const char *arg = argv[1];

    /* DAY24-FOLLOWUP: no '/' at all in the typed argument — default
     * onto vfs_root's own mount slot, per this file's header comment
     * above, rather than letting fat_create_file() reject it outright.
     * A path buffer of MOUNT-SLOT ("IDE" + up to 4 digits, well under
     * 8 bytes per vfs_mount_entry_t.name) + '/' + up to a 12-byte 8.3
     * filename comfortably fits SHELL_ARG_LEN (40, see shell.h) twice
     * over. */
    static char path_buf[64];
    const char *path = arg;

    if (strchr(arg, '/') == (char *)0) {
        const char *slot = vfs_root ? vfs_mount_name_of(vfs_root->fs) : (const char *)0;
        if (!slot) {
            terminal_puts(t, "create: no mount-slot given and no "
                              "current filesystem mounted\n", TERM_COL_ERROR);
            return;
        }

        uint32_t i = 0;
        while (slot[i] != '\0' && i < sizeof(path_buf) - 2) {
            path_buf[i] = slot[i];
            i++;
        }
        path_buf[i++] = '/';
        uint32_t j = 0;
        while (arg[j] != '\0' && i < sizeof(path_buf) - 1) {
            path_buf[i++] = arg[j++];
        }
        path_buf[i] = '\0';
        path = path_buf;
    }

    /* DAY24: fat_create_file() (kernel/fs/fat/fat_vfs.c) does the real
     * work — scans the target volume's root directory for a reusable
     * free slot (DIR_Name[0] == 0x00 or 0xE5), rejects a duplicate name
     * via its own internal fat_open() check, and writes a new entry
     * with start cluster 0 / size 0 if the coast is clear. See that
     * function's own doc comment (fat_vfs.h) for exactly which
     * fat_error_t code each failure case returns. */
    int rc = fat_create_file(path);
    if (rc != FAT_OK) {
        terminal_puts(t, "create: cannot create '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' (", TERM_COL_ERROR);
        terminal_puts(t, fat_strerror(rc), TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
        return;
    }

    terminal_puts(t, "created ", TERM_COL_SUCCESS);
    terminal_puts(t, path, TERM_COL_SUCCESS);
    terminal_newline(t);
}