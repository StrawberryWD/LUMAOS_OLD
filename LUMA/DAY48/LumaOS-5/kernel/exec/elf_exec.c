/* kernel/exec/elf_exec.c
 * LumaOS 5.0 - elf_exec(): 按路径运行一个 .sw 程序（load -> map -> run）
 *
 * DAY48 (Week 7, Day 48): "Shell 集成 `run`。"  原理见 elf.h 的 DAY48 注释，接口见 elf_exec.h。
 *
 * 这个函数本身几乎没有逻辑——它的价值是“把三步放在一个地方、顺序不可能写错”：
 *   - 必须先 load（验证）再 map（装入），最后 run；
 *   - run 只认 loaded==1，所以 map 失败时即使调用者忘了检查 rc，elf_run 也会拒绝；
 *   - 失败时告诉调用者“死在哪一步”，调用者据此给出准确的话：load 失败 = 文件有问题，
 *     什么都没动；map 失败 = 内存有问题，程序没跑；run 失败 = 栈有问题（程序没跑）或
 *     栈溢出（程序已经跑完，事后才发现）。
 */
#include "elf_exec.h"
#include "elf_loader.h"
#include "../lib/string.h"

int elf_exec(const char *path, terminal_t *out, elf_exec_result_t *res)
{
    if (res == (elf_exec_result_t *)0)
        return ELF_ERR_NULL;
    memset(res, 0, sizeof(*res));
    if (path == (const char *)0 || out == (terminal_t *)0) {
        res->stage = ELF_STAGE_LOAD;
        res->rc = ELF_ERR_NULL;
        return res->rc;
    }

    /* 静态：elf_image_t 约 450 字节，不放内核栈。 */
    static elf_image_t img;

    int rc = elf_load(path, &img);                       /* Day 44 */
    if (rc != ELF_OK) {
        res->stage = ELF_STAGE_LOAD;
        res->rc = rc;
        return rc;
    }
    res->entry = img.ehdr.e_entry;

    rc = elf_load_segments(path, &img, (elf_load_stats_t *)0);   /* Day 45 */
    if (rc != ELF_OK) {
        res->stage = ELF_STAGE_MAP;
        res->rc = rc;
        return rc;
    }

    rc = elf_run(&img, out, &res->code, &res->info);     /* Day 46/47 */
    res->rc = rc;
    if (rc == ELF_OK || rc == ELF_ERR_STACK_OVERFLOW)
        res->ran = 1;                                    /* 栈溢出：程序已跑完，事后才发现 */
    res->stage = (rc == ELF_OK) ? ELF_STAGE_DONE : ELF_STAGE_RUN;
    return rc;
}
