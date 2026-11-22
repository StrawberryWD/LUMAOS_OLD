/* kernel/shell/shell.h - LumaOS 5.0 Shell Header */
#ifndef LUMA_SHELL_H
#define LUMA_SHELL_H

#include "../gui/gui.h"

#define SHELL_MAX_CMDS 16
#define SHELL_MAX_ARGS  8
#define SHELL_ARG_LEN  40

typedef void (*shell_cmd_fn_t)(terminal_t *t, int argc, char **argv);

typedef struct {
    const char     *name;
    const char     *description;
    shell_cmd_fn_t  fn;
} shell_cmd_t;

extern shell_cmd_t cmd_table[];
extern int         cmd_count;

void shell_init(void);
void shell_exec(terminal_t *t, const char *cmdline);
void shell_register(const char *name, const char *desc, shell_cmd_fn_t fn);

/* shell_resolve_path(arg, out_buf, out_buf_size)
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "支持 cd 命令：切换当前工作目录（在挂载点之间）。支持 show test.txt
 *    而不是 show IDE2/text.txt。"
 *
 *   cmd_create's own DAY24-FOLLOWUP note (kernel/shell/cmd/create.c)
 *   first added the "no '/' in the typed argument -> default onto
 *   vfs_root's own mount slot" fallback, one command at a time. Day 25
 *   needs the SAME fallback in `write` and `read` (see this project's
 *   own Day 25 acceptance text: "支持 show test.txt" — and `read` is
 *   this day's replacement name for that same lookup, per the Shell
 *   commands list below) — a third and fourth copy of the identical
 *   loop would be exactly the kind of duplication this header's own
 *   shell_cmd_fn_t abstraction already exists to avoid at the command-
 *   dispatch layer; this function is that same avoidance applied one
 *   layer lower, to path parsing. cmd_create is expected to switch to
 *   calling this function too (rather than keeping its own private
 *   copy of the loop) — see create.c's own updated header comment.
 *
 *   If `arg` contains a '/', it is copied into `out_buf` UNCHANGED
 *   (already fully-qualified — e.g. "IDE0/TEST.TXT" — nothing to
 *   default). If `arg` has NO '/' at all, this prepends "<slot>/" where
 *   <slot> is vfs_mount_name_of(vfs_root->fs) — the CURRENT working
 *   mount, exactly the one Day 25's own `cd` command (cmd_cd, kernel/
 *   shell/cmd/cd.c) changes by reassigning vfs_root — so a bare
 *   `write test.txt` always resolves against whichever mount `cd` most
 *   recently switched to, the same "vfs_root IS the cwd" design
 *   kmain.c's text_shell_prompt() already relies on to print "IDE0/ >"
 *   after a `cd IDE0` (see that function's own DAY14/DAY25 comments).
 *
 * Returns 0 on success (`out_buf` now holds a NUL-terminated path).
 * Returns -1 if `arg`/`out_buf` is NULL, `arg` has no '/' AND no
 * volume is currently mounted (vfs_root is NULL or vfs_mount_name_of()
 * finds no matching slot for it — should not happen once Day 22/23's
 * self-test mounts have run, but checked rather than assumed), or
 * `out_buf_size` is too small to hold the resulting "<slot>/<arg>"
 * (truncation is refused outright rather than silently handing a
 * caller a cut-off path that might resolve to the WRONG file).
 */
int shell_resolve_path(const char *arg, char *out_buf, uint32_t out_buf_size);

void cmd_help    (terminal_t *t, int argc, char **argv);
void cmd_clear   (terminal_t *t, int argc, char **argv);
void cmd_type    (terminal_t *t, int argc, char **argv);
void cmd_version (terminal_t *t, int argc, char **argv);
void cmd_echo    (terminal_t *t, int argc, char **argv);
void cmd_color   (terminal_t *t, int argc, char **argv);
void cmd_shutdown(terminal_t *t, int argc, char **argv);
void cmd_reboot  (terminal_t *t, int argc, char **argv);
void cmd_ls      (terminal_t *t, int argc, char **argv);  /* DAY14 */
void cmd_show    (terminal_t *t, int argc, char **argv);  /* DAY19, renamed from cmd_cat on DAY21 */
void cmd_create  (terminal_t *t, int argc, char **argv);  /* DAY24 */
void cmd_write   (terminal_t *t, int argc, char **argv);  /* DAY25 */
void cmd_read    (terminal_t *t, int argc, char **argv);  /* DAY25 */
void cmd_cd      (terminal_t *t, int argc, char **argv);  /* DAY25 */
void cmd_rm      (terminal_t *t, int argc, char **argv);  /* DAY26 */

#endif