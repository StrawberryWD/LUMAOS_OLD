/* kernel/exec/elf_run.c
 * LumaOS 5.0 - elf_run(): 跳到已装入内存的 ELF 入口点并执行
 *
 * DAY46 (Week 7, Day 46): "实现 elf_run()。"  验收："hello.sw 可以执行。"
 * DAY47: 用户栈 + 统一退出路径 + 栈事后检查。
 * 原理见 elf.h 的 DAY46 / DAY47 注释，接口见 elf_run.h，跳板见 elf_enter.asm。
 */
#include "elf_run.h"
#include "../lib/string.h"

/* 汇编跳板：存现场 -> 切到用户栈 -> 跳到 entry(api) -> 统一出口 -> 恢复现场，
 * 返回程序的退出码。elf_api_exit 是给程序的 api->exit；elf_exit_via 记录走了哪条路。 */
extern int      elf_enter(uint32_t entry, const void *api, uint32_t stack_top);
extern void     elf_api_exit(int code);
extern uint32_t elf_exit_via;

/* ---- 给程序用的“内核服务”（luma_api_t 的实现） ---- */

/* 当前正在运行的程序把输出送到哪个终端。elf_run() 进入前设置、返回后清空。 */
static terminal_t *g_run_out;

/* 一次 puts 最多输出多少字符：程序给的指针指向哪、有没有 NUL 都不可信，
 * 设个上限，免得一个忘了写 '\0' 的字符串把整个内存当文字刷屏到天荒地老。 */
#define API_PUTS_MAX  4096

/* DAY48: 程序一共输出了多少字符、最后一个是什么。`run` 靠它决定要不要补一个换行：
 * 程序最后一次输出不以 '\n' 结尾时，shell 的下一个提示符会接在同一行末尾。 */
static uint32_t g_out_total;
static int      g_out_last;

static void api_putchar(int c)
{
    if (g_run_out) {
        terminal_putchar(g_run_out, (char)c, TERM_COL_DEFAULT);
        g_out_total++;
        g_out_last = (unsigned char)c;
    }
}

static void api_puts(const char *s)
{
    if (!g_run_out || !s)
        return;
    for (int i = 0; i < API_PUTS_MAX && s[i]; i++)
        api_putchar(s[i]);
}

static const luma_api_t g_api = {
    LUMA_API_MAGIC,
    LUMA_API_VERSION,
    api_puts,
    api_putchar,
    elf_api_exit,                        /* DAY47 */
};

/* ---- DAY47: 用户栈的准备与事后检查 ---- */

/* 把 [BASE, TOP) 整块填成 ELF_STACK_FILL 并逐字节读回。成功返回 1。
 * 目标内存用 volatile 访问（与 Day 45 同理：防止编译器认为“刚写进去的当然读得回来”）。
 * 局限与 Day 45 相同：QEMU 里不存在的地址写入被丢弃、读回 0x00，会被发现。 */
static int stack_prepare(void)
{
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)ELF_USER_STACK_BASE;
    for (uint32_t i = 0; i < ELF_USER_STACK_SIZE; i++) {
        p[i] = ELF_STACK_FILL;
        if (p[i] != ELF_STACK_FILL)
            return 0;
    }
    return 1;
}

/* 运行后：从栈底往上找第一个不再是填充值的字节 -> 栈最深用到哪；
 * 最低 ELF_STACK_GUARD 字节（护栏）里只要有被改过的就算溢出。 */
static void stack_inspect(elf_run_info_t *r)
{
    volatile const unsigned char *p = (volatile const unsigned char *)(uintptr_t)ELF_USER_STACK_BASE;
    uint32_t first = ELF_USER_STACK_SIZE;          /* 没找到 = 一个字节都没用 */
    for (uint32_t i = 0; i < ELF_USER_STACK_SIZE; i++) {
        if (p[i] != ELF_STACK_FILL) { first = i; break; }
    }
    r->stack_top  = ELF_USER_STACK_TOP;
    r->stack_size = ELF_USER_STACK_SIZE;
    r->stack_used = ELF_USER_STACK_SIZE - first;
    r->overflow   = (first < ELF_STACK_GUARD) ? 1u : 0u;
}

int elf_run(elf_image_t *img, terminal_t *out, int *exit_code, elf_run_info_t *info)
{
    if (exit_code)
        *exit_code = 0;
    if (info)
        memset(info, 0, sizeof(*info));
    if (img == (elf_image_t *)0 || out == (terminal_t *)0)
        return ELF_ERR_NULL;

    /* 1. 只认“已装入”的映像：loaded 只有 elf_load_segments() 全部成功才会置 1。 */
    if (img->loaded != 1)
        return ELF_ERR_NOT_LOADED;

    /* 2. 不信任 img：重新校验。静态变量——elf_image_t 约 450 字节，不放内核栈。
     *    elf32_check_phdrs() 会整体重写它的 *img 参数，所以用一份临时拷贝。
     *    （DAY47：这次校验同时拒绝与用户栈重叠的段 -> ELF_ERR_SEG_STACK。） */
    static elf_image_t chk;
    int rc = elf32_check_header(&img->ehdr, img->file_size);
    if (rc != ELF_OK)
        return rc;
    if (img->ehdr.e_phnum > ELF_MAX_PHDRS)
        return ELF_ERR_TOO_MANY_PHDRS;
    {
        elf32_ehdr_t eh = img->ehdr;
        rc = elf32_check_phdrs(&eh, img->phdr, img->ehdr.e_phnum,
                               img->file_size, &chk);
        if (rc != ELF_OK)
            return rc;
    }

    /* 入口点：校验函数已保证它落在某个带 PF_X 的 PT_LOAD 的文件部分里，
     * 这里再用“校验结果”（chk，而不是调用者传来的 img->entry_seg）确认一次。
     * 因为 img->entry_seg 也是调用者可以改的字段。 */
    const elf32_phdr_t *es = &chk.phdr[chk.entry_seg];
    uint32_t entry = chk.ehdr.e_entry;
    if (es->p_type != PT_LOAD || !(es->p_flags & PF_X) ||
        entry < es->p_vaddr || entry - es->p_vaddr >= es->p_filesz)
        return ELF_ERR_ENTRY_NOT_EXEC;

    /* 3. DAY47: 准备用户栈（失败 = 该处没有 RAM：此时还没执行任何一条程序指令）。 */
    if (!stack_prepare())
        return ELF_ERR_NO_STACK;

    /* 4. 跳进去（在用户栈上）。g_run_out 在进入前设好，返回后清掉
     *    （程序只在运行期间能打印）。无论 ret 还是 api->exit 都从同一个出口回来。 */
    g_run_out = out;
    g_out_total = 0;
    g_out_last = 0;
    int code = elf_enter(entry, &g_api, ELF_USER_STACK_TOP);
    g_run_out = (terminal_t *)0;

    /* 5. 程序已经改写过自己的 .data/.bss：这份内存映像不再“干净”。 */
    img->loaded = 0;

    if (exit_code)
        *exit_code = code;

    /* 6. DAY47: 事后检查栈。 */
    elf_run_info_t r;
    stack_inspect(&r);
    r.via_exit = elf_exit_via;
    r.out_total = g_out_total;           /* DAY48 */
    r.out_last  = (uint32_t)g_out_last;
    if (info)
        *info = r;
    if (r.overflow)
        return ELF_ERR_STACK_OVERFLOW;
    return ELF_OK;
}
