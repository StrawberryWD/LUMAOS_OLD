/* kernel/shell/cmd/elfrun.c
 * LumaOS 5.0 — DAY46 (Week 7, Day 46): "实现 elf_run()。"
 *   验收："hello.sw 可以执行。"
 *
 * 用法:  elfrun [-v] <file>   例如  elfrun IDE3/hello.sw   （LMFS）
 *                                elfrun IDE0/HELLO.SW   （FAT）
 *                                cd IDE3  然后  elfrun hello.sw
 *
 * DAY47: 程序现在在自己的 64 KB 用户栈上运行，可以 ret 也可以 api->exit() 退出；
 * `-v` 在结论行之后多打印一行运行报告：栈用了多少字节、是 ret 还是 api->exit
 * 退出的。不带 -v 时输出与 Day 46 完全一致。栈溢出（护栏被写）会在结论行之后
 * 报错。原理见 elf.h 的 DAY47 注释。
 *
 * 作用：把加载链从头到尾走一遍并真正执行——
 *     elf_load()           验证文件（Day 44）
 *     elf_load_segments()  把 PT_LOAD 段装进 p_vaddr，清零 .bss（Day 45）
 *     elf_run()            call e_entry，程序 ret 回来（今天）
 * 然后打印程序的退出码（入口函数返回时的 eax）。原理见 elf.h 的 DAY46 注释。
 *
 * 程序在运行期间通过 api->puts 打印的文字直接出现在本终端里，夹在
 * "elfrun: running ..." 与 "elfrun: ... exited with code N" 两行之间。
 *
 * 与 Day 48 的关系：`run` 命令（Day 48）会取代它成为正式入口；elfrun 保留
 * 作为“低层调试命令”，与 elfinfo / elfload / elfmap 并列——每个只做加载链
 * 中的一段。
 *
 * 危险提示：今天程序与内核同在 Ring 0。一个死循环的程序会让整个系统停住
 * （Day 47 的“退出路径”、Day 56 的“用户态异常”才逐步解决），所以只运行
 * 自己信任的 .sw。每行 < 80 列。
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../exec/elf.h"
#include "../../exec/elf_loader.h"
#include "../../exec/elf_run.h"
#include "../../lib/string.h"
#include "../shell.h"

static void put_hex8(terminal_t *t, uint32_t v, unsigned char color)
{
    static const char hexd[] = "0123456789abcdef";
    terminal_puts(t, "0x", color);
    for (int i = 7; i >= 0; i--)
        terminal_putchar(t, hexd[(v >> (i * 4)) & 0xF], color);
}

/* 有符号十进制：退出码可以是负数（例如 -1）。 */
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

static void fail_line(terminal_t *t, const char *stage, const char *path, int rc)
{
    terminal_puts(t, "elfrun: ", TERM_COL_ERROR);
    terminal_puts(t, stage, TERM_COL_ERROR);
    terminal_puts(t, " ", TERM_COL_ERROR);
    terminal_puts(t, path, TERM_COL_ERROR);
    terminal_puts(t, ":", TERM_COL_ERROR);
    terminal_newline(t);
    /* 原因另起一行：最长的原因文字约 60 列，与前缀同行会超过 80 列而折行。 */
    terminal_puts(t, "  ", TERM_COL_ERROR);
    terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
    terminal_newline(t);
}

void cmd_elfrun(terminal_t *t, int argc, char **argv)
{
    int verbose = 0;
    int ai = 1;
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        verbose = 1;
        ai = 2;
    }
    if (argc <= ai) {
        terminal_puts(t, "usage: elfrun [-v] <file>  (e.g. elfrun IDE3/hello.sw)\n",
                      TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[ai], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "elfrun: no mount-slot given and no "
                         "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }

    /* 静态：elf_image_t 约 450 字节，不放内核栈。 */
    static elf_image_t img;

    /* ---- 1. 验证（Day 44） ---- */
    int rc = elf_load(path_buf, &img);
    if (rc != ELF_OK) {
        fail_line(t, "cannot load", path_buf, rc);
        terminal_puts(t, "elfrun: nothing was executed\n", TERM_COL_ERROR);
        return;
    }

    /* ---- 2. 装入内存（Day 45）：失败时 loaded 仍是 0，elf_run 也会拒绝 ---- */
    rc = elf_load_segments(path_buf, &img, (elf_load_stats_t *)0);
    if (rc != ELF_OK) {
        fail_line(t, "cannot map", path_buf, rc);
        terminal_puts(t, "elfrun: nothing was executed\n", TERM_COL_ERROR);
        return;
    }

    /* ---- 3. 执行（Day 46） ---- */
    terminal_puts(t, "elfrun: running ", TERM_COL_INFO);
    terminal_puts(t, path_buf, TERM_COL_INFO);
    terminal_puts(t, " at entry ", TERM_COL_INFO);
    put_hex8(t, img.ehdr.e_entry, TERM_COL_INFO);
    terminal_newline(t);

    int code = 0;
    elf_run_info_t info;
    rc = elf_run(&img, t, &code, &info);
    /* 栈溢出是“程序已经跑完、事后才发现”：照常打印结论，再报错。
     * 其余错误都发生在跳进程序之前（没有执行任何指令）。 */
    if (rc != ELF_OK && rc != ELF_ERR_STACK_OVERFLOW) {
        fail_line(t, "cannot run", path_buf, rc);
        return;
    }

    /* 约定：程序的最后一次输出以 '\n' 结尾，结论行才会独占一行。 */
    terminal_puts(t, "elfrun: ", code == 0 ? TERM_COL_SUCCESS : TERM_COL_ERROR);
    terminal_puts(t, path_buf, code == 0 ? TERM_COL_SUCCESS : TERM_COL_ERROR);
    terminal_puts(t, " exited with code ", code == 0 ? TERM_COL_SUCCESS : TERM_COL_ERROR);
    put_sdec(t, code, code == 0 ? TERM_COL_SUCCESS : TERM_COL_ERROR);
    terminal_newline(t);

    if (verbose) {
        terminal_puts(t, "elfrun: stack used ", TERM_COL_INFO);
        put_sdec(t, (int)info.stack_used, TERM_COL_INFO);
        terminal_puts(t, " of ", TERM_COL_INFO);
        put_sdec(t, (int)info.stack_size, TERM_COL_INFO);
        terminal_puts(t, " bytes (top ", TERM_COL_INFO);
        put_hex8(t, info.stack_top, TERM_COL_INFO);
        terminal_puts(t, "), left via ", TERM_COL_INFO);
        terminal_puts(t, info.via_exit ? "api->exit\n" : "ret\n", TERM_COL_INFO);
    }
    if (rc == ELF_ERR_STACK_OVERFLOW)
        fail_line(t, "after run", path_buf, rc);
}
