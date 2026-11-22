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

void cmd_help    (terminal_t *t, int argc, char **argv);
void cmd_clear   (terminal_t *t, int argc, char **argv);
void cmd_type    (terminal_t *t, int argc, char **argv);
void cmd_version (terminal_t *t, int argc, char **argv);
void cmd_echo    (terminal_t *t, int argc, char **argv);
void cmd_color   (terminal_t *t, int argc, char **argv);
void cmd_shutdown(terminal_t *t, int argc, char **argv);
void cmd_reboot  (terminal_t *t, int argc, char **argv);
void cmd_ls      (terminal_t *t, int argc, char **argv);  /* DAY14 */

#endif