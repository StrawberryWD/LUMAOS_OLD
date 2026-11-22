/* kernel/shell/cmd/run.c
 * LumaOS 5.0 — DAY48 (Week 7, Day 48): "Shell 集成 `run`。"
 *   验收："`run hello.sw` 成功。"
 *
 * 用法:  run <program.sw>     例如  run IDE3/hello.sw    （LMFS）
 *                                   run IDE0/HELLO.SW    （FAT）
 *                                   cd IDE3  然后  run hello.sw
 *
 * 这是 Day 44~47 的“正式出口”：用户敲一个命令，系统按 Unix shell 的习惯运行程序——
 *   - 程序成功（退出码 0）：除了程序自己的输出，什么都不多说；
 *   - 程序以非 0 退出：多一行红色的 "run: <path> exited with code N"；
 *   - 程序最后一次输出没以换行结尾：run 补一个换行，让下一个提示符另起一行；
 *   - 出错：一行说明（原因太长就另起一行），并且明确哪一步出的错——文件有问题
 *     (没动任何东西)、内存有问题 (程序没跑)、栈溢出 (程序跑完了，事后才发现)。
 *
 * 与调试命令 elfrun 的区别：elfrun 是给内核开发者看的（打印入口地址、`-v` 栈报告、
 * 每一步的标题，对任何 ELF 都运行）；run 是给用户用的（安静、只运行 `.sw`）。
 * 运行本身都走同一个 elf_exec()，没有第二套实现。
 *
 * 只运行 .sw：计划.md 规定 LumaOS 的应用执行文件统一用 `.sw`；run 在碰磁盘之前先检查
 * 扩展名（不区分大小写：FAT 的 HELLO.SW 与 LMFS 的 hello.sw 都行），“把一个文本文件
 * 当程序运行”这种明显的误操作在第一步就被挡住，不去读它。注意这只是“防手滑”，不是
 * 安全边界——内容是不是合法 ELF 仍由 elf_load() 验证。
 *
 * 危险提示：今天程序仍与内核同在 Ring 0（Day 54/55 才有硬件隔离），死循环的程序会让
 * 系统停住，所以只运行自己信任的 .sw。每行 < 80 列。
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../exec/elf.h"
#include "../../exec/elf_exec.h"
#include "../../lib/string.h"
#include "../shell.h"

static void put_sdec(terminal_t *t, int v, unsigned char color)
{
    char tmp[12];
    int n = 0;
    uint32_t u = (v < 0) ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) terminal_putchar(t, '-', color);
    while (n) terminal_putchar(t, tmp[--n], color);
}

/* name 是否以 ".sw" 结尾（不区分大小写，且 ".sw" 前面至少还有一个字符）。 */
static int has_sw_ext(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return 0;
    const char *e = name + n - 3;
    return e[0] == '.' && (e[1] == 's' || e[1] == 'S') && (e[2] == 'w' || e[2] == 'W');
}

/* "run: <path>: <reason>"；总长超过 78 列就把原因另起一行。 */
static void err_line(terminal_t *t, const char *path, const char *reason)
{
    terminal_puts(t, "run: ", TERM_COL_ERROR);
    terminal_puts(t, path, TERM_COL_ERROR);
    if (strlen(path) + strlen(reason) + 8 > 78) {
        terminal_puts(t, ":", TERM_COL_ERROR);
        terminal_newline(t);
        terminal_puts(t, "  ", TERM_COL_ERROR);
    } else {
        terminal_puts(t, ": ", TERM_COL_ERROR);
    }
    terminal_puts(t, reason, TERM_COL_ERROR);
    terminal_newline(t);
}

void cmd_run(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: run <program.sw>  (e.g. run hello.sw)\n",
                      TERM_COL_ERROR);
        return;
    }

    /* 先看扩展名，再碰磁盘。取最后一个 '/' 之后的文件名。 */
    const char *base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    if (!has_sw_ext(base)) {
        err_line(t, argv[1], "not a .sw program");
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "run: no mount-slot given and no "
                         "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }

    static elf_exec_result_t res;
    elf_exec(path_buf, t, &res);

    /* 程序真的跑过：先收拾输出（补换行），再报告退出码 / 栈溢出。 */
    if (res.ran) {
        if (res.info.out_total > 0 && res.info.out_last != '\n')
            terminal_newline(t);
        if (res.code != 0) {
            terminal_puts(t, "run: ", TERM_COL_ERROR);
            terminal_puts(t, path_buf, TERM_COL_ERROR);
            terminal_puts(t, " exited with code ", TERM_COL_ERROR);
            put_sdec(t, res.code, TERM_COL_ERROR);
            terminal_newline(t);
        }
        if (res.rc == ELF_ERR_STACK_OVERFLOW)
            err_line(t, path_buf, elf_strerror(res.rc));
        return;
    }

    if (res.rc == ELF_OK)
        return;                      /* 不会发生：成功必然 ran == 1，保险起见 */

    /* 没有跑：按“死在哪一步”给出准确的话。 */
    err_line(t, path_buf, elf_strerror(res.rc));
    if (res.stage == ELF_STAGE_LOAD)
        return;                      /* 文件问题：说明原因就够了 */
    terminal_puts(t, "run: ", TERM_COL_ERROR);
    terminal_puts(t, "the program was not executed\n", TERM_COL_ERROR);
}
