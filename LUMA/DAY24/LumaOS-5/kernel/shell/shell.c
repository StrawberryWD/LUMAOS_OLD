/* kernel/shell/shell.c - LumaOS 5.0 Shell Core */
#include "../core/kernel.h"
#include "../gui/gui.h"
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
     * 部分。" See kernel/shell/cmd/show.c's header comment for the full
     * "why <mount-slot>/<filename>, why byte-at-a-time, why capped at
     * 512 bytes" rationale — this goes through fat_open()+fs->read()
     * (fat_read(), also new this day), never a FAT12/16/32-specific
     * function by name, same discipline `ls` already established.
     *
     * DAY21: "更改cat命令名称为show，功能不变" — the command name (and
     * the function it dispatches to) is now "show"/cmd_show, not
     * "cat"/cmd_cat. Behavior is otherwise unchanged (see show.c's own
     * DAY21 header comment for exactly what DID change this day:
     * friendlier fat_error_t-based diagnostics and a new fat_close()
     * call, both independent of the rename itself). */
    shell_register("show",     "Print a file's contents (via VFS)", cmd_show);
    /* DAY24: "实现 fat_create_file(name)：在根目录区找一个空条目...
     * 在 Shell 中输入 create test.txt，重新 ls 能看到它。" See
     * kernel/shell/cmd/create.c's header comment for the full
     * "<mount-slot>/<filename>" path convention (matching `show`'s own
     * since Day 18-FOLLOWUP) and the small bare-filename fallback this
     * command adds on top of fat_create_file()'s own stricter syntax. */
    shell_register("create",   "Create an empty file (via VFS)",    cmd_create);
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