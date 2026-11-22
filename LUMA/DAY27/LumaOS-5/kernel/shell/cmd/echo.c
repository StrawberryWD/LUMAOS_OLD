/* kernel/shell/cmd/echo.c */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"
void cmd_echo(terminal_t *t, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        terminal_puts(t, argv[i], TERM_COL_DEFAULT);
        if (i < argc-1) terminal_putchar(t, ' ', TERM_COL_DEFAULT);
    }
    terminal_newline(t);
}
