/* kernel/shell/shell.c - LumaOS 5.0 Shell Core */
#include "../core/kernel.h"
#include "../gui/gui.h"
#include "../fs/vfs/vfs.h"   /* DAY25: vfs_root, vfs_mount_name_of() —
                                shell_resolve_path() below */
#include "shell.h"

shell_cmd_t cmd_table[SHELL_MAX_CMDS];
int         cmd_count = 0;

void shell_register(const char *name, const char *desc, shell_cmd_fn_t fn) {
    if (cmd_count >= SHELL_MAX_CMDS) return;
    cmd_table[cmd_count].name        = name;
    cmd_table[cmd_count].description = desc;
    cmd_table[cmd_count].fn          = fn;
    cmd_count++;
}

void shell_init(void) {
    cmd_count = 0;
    shell_register("help",     "Show available commands",     cmd_help    );
    shell_register("clear",    "Clear terminal screen",       cmd_clear   );
    shell_register("version",  "Show OS version",             cmd_version );
    shell_register("type",     "Show quick reference",        cmd_type    );
    shell_register("echo",     "Echo text to terminal",       cmd_echo    );
    shell_register("color",    "Change text color (0-15)",    cmd_color   );
    shell_register("shutdown", "Shutdown the system",         cmd_shutdown);
    shell_register("reboot",   "Reboot the system",           cmd_reboot  );
    /* DAY14: "整合 Shell：新增命令 cmd_ls（调用 vfs_readdir 列出文件）。"
     * See kernel/shell/cmd/ls.c's header comment for what vfs_root
     * actually resolves to and why this goes through fs->readdir()
     * instead of calling any FAT12-specific function by name. */
    shell_register("ls",       "List files (via VFS)",        cmd_ls      );
    /* DAY19: "实现 cmd_cat /KERNEL.BIN，能在终端打印出内核二进制的开头
     * 部分。" this goes through fat_open()+fs->read() (fat_read(), also
     * new this day), never a FAT12/16/32-specific function by name,
     * same discipline `ls` already established.
     *
     * DAY21: "更改cat命令名称为show，功能不变" — renamed cat -> show.
     *
     * DAY27: "删除那个show只保留read" — `show` is removed outright.
     * kernel/shell/cmd/read.c (DAY25) had already grown into a full
     * superset of what `show` did (see read.c's own header comment),
     * so this project no longer needs two commands doing the same job;
     * `read` (registered below, alongside `write`/`cd`) is the one
     * that remains. */
    /* DAY24: "实现 fat_create_file(name)：在根目录区找一个空条目...
     * 在 Shell 中输入 create test.txt，重新 ls 能看到它。" See
     * kernel/shell/cmd/create.c's header comment for the full
     * "<mount-slot>/<filename>" path convention (matching `read`'s own
     * since Day 18-FOLLOWUP/Day 25) and the small bare-filename
     * fallback this command adds on top of fat_create_file()'s own
     * stricter syntax. */
    shell_register("create",   "Create an empty file (via VFS)",    cmd_create);
    /* DAY25: "实现 fat_write(file, buf, size)... write test.txt... 支持
     * cd 命令... 支持 show test.txt 而不是 show IDE2/text.txt." See each
     * command's own .c file header comment for the full contract:
     * cmd_write.c (interactive multi-line editor, Ctrl+S to save,
     * overwrite-confirmation prompt — DAY27 update: skipped entirely
     * for an already-empty file), cmd_read.c (this day's `read` —
     * `show`'s own logic plus the bare-filename fallback `create`
     * established), cmd_cd.c (switches vfs_root between mounted
     * volumes — this project has no subdirectory tree yet, see that
     * file's own header comment for why "cd between mounts" is exactly
     * what "当前工作目录" can mean at this point in the roadmap). */
    shell_register("write",    "Write a file's contents (via VFS)", cmd_write );
    shell_register("read",     "Print a file's contents (via VFS)", cmd_read  );
    shell_register("cd",       "Change current mounted volume",     cmd_cd    );
    /* DAY26: "实现 fat_delete_file(name)：找到目录项，标记为已删除
     * （首字节设为 0xE5），释放其占用的簇链。rm test.txt 后 ls 消失，
     * 且空闲簇数增加。" See kernel/shell/cmd/rm.c's header comment for
     * the full "<mount-slot>/<filename>" path convention (same as
     * every other file-taking command since Day 18/24/25) and
     * kernel/fs/fat/fat_vfs.h's doc comment on fat_delete_file() for
     * the real cluster-chain-freeing + directory-entry-deletion work
     * this command exercises. */
    shell_register("rm",       "Delete a file (via VFS)",           cmd_rm    );
}

static char arg_buf[SHELL_MAX_ARGS][SHELL_ARG_LEN];
static char *argv_p[SHELL_MAX_ARGS];

static int parse(const char *line, char **argv) {
    int argc = 0, i = 0;
    while (line[i]==' '||line[i]=='\t') i++;
    while (line[i] && argc < SHELL_MAX_ARGS) {
        int ai = 0;
        while (line[i] && line[i]!=' ' && line[i]!='\t' && ai < SHELL_ARG_LEN-1)
            arg_buf[argc][ai++] = line[i++];
        arg_buf[argc][ai] = 0;
        /* === MODIFIED SECTION START ===
         * STAGE4 FIX: "argv[argc++] = arg_buf[argc-1];" reads and
         * writes argc in the same unsequenced expression (flagged by
         * -Wsequence-point). Every mainstream compiler happened to
         * evaluate it left-to-right, but that's not guaranteed by the
         * C standard, so under more aggressive optimization this could
         * theoretically break argument parsing. Split into two
         * statements with an explicit sequence point between them. */
        argv[argc] = arg_buf[argc];
        argc++;
        /* === MODIFIED SECTION END === */
        while (line[i]==' '||line[i]=='\t') i++;
    }
    return argc;
}

void shell_exec(terminal_t *t, const char *cmdline) {
    int blank = 1;
    for (int i = 0; cmdline[i]; i++)
        if (cmdline[i]!=' '&&cmdline[i]!='\t') { blank=0; break; }
    if (blank) return;
    int argc = parse(cmdline, argv_p);
    if (!argc) return;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(argv_p[0], cmd_table[i].name)==0) {
            cmd_table[i].fn(t, argc, argv_p);
            return;
        }
    }
    terminal_puts(t, "Unknown: ", TERM_COL_ERROR);
    terminal_puts(t, argv_p[0],  TERM_COL_ERROR);
    terminal_puts(t, "\nType 'help'\n", TERM_COL_DEFAULT);
}

/* Called by terminal when user presses Enter */
void terminal_run_command(terminal_t *t, const char *cmd) {
    shell_exec(t, cmd);
}

/* ===================================================================
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "支持 cd 命令：切换当前工作目录（在挂载点之间）。支持 show test.txt
 *    而不是 show IDE2/text.txt。"
 *
 * See shell.h's doc comment above this function's declaration for the
 * full contract and the "why a shared helper, not a third/fourth copy"
 * rationale. Implementation mirrors cmd_create's own DAY24-FOLLOWUP
 * loop (kernel/shell/cmd/create.c) exactly, just parameterized over an
 * output buffer/size instead of that command's own private static
 * path_buf[64].
 * =================================================================== */
int shell_resolve_path(const char *arg, char *out_buf, uint32_t out_buf_size)
{
    if (!arg || !out_buf || out_buf_size == 0)
        return -1;

    if (strchr(arg, '/') != (char *)0) {
        /* Already fully-qualified — copy unchanged (bounds-checked). */
        uint32_t i = 0;
        while (arg[i] != '\0' && i < out_buf_size - 1) {
            out_buf[i] = arg[i];
            i++;
        }
        if (arg[i] != '\0')
            return -1;   /* didn't fit — refuse rather than truncate */
        out_buf[i] = '\0';
        return 0;
    }

    /* No '/' at all — default onto vfs_root's own current mount slot,
     * the same fallback cmd_create's DAY24-FOLLOWUP note established,
     * now shared. */
    const char *slot = vfs_root ? vfs_mount_name_of(vfs_root->fs) : (const char *)0;
    if (!slot)
        return -1;   /* no current filesystem mounted */

    uint32_t i = 0;
    while (slot[i] != '\0') {
        if (i >= out_buf_size - 1)
            return -1;   /* didn't fit */
        out_buf[i] = slot[i];
        i++;
    }
    if (i >= out_buf_size - 1)
        return -1;
    out_buf[i++] = '/';

    uint32_t j = 0;
    while (arg[j] != '\0') {
        if (i >= out_buf_size - 1)
            return -1;   /* didn't fit */
        out_buf[i++] = arg[j++];
    }
    out_buf[i] = '\0';
    return 0;
}