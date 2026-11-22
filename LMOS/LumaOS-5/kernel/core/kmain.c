/* kernel/core/kmain.c
 * LumaOS 4.1 - Kernel Main Entry Point
 *
 * DAY2 (Week 1, Day 2 of the 5.0 filesystem roadmap):
 *   - #include the new IDE driver header and call ide_test() once,
 *     right after interrupts are enabled, so the Day-2 acceptance
 *     check ("status register reads back non-0xFF") runs on every
 *     boot.
 *
 * DAY3 (Week 1, Day 3 of the 5.0 filesystem roadmap):
 *   - After ide_test(), call ide_read_sectors(0, 1, mbr_buf) to read
 *     LBA 0 into a stack buffer, then console_write_hex() the first
 *     64 bytes. This is the roadmap's *proof of correct PIO reads*:
 *     the driver is reading real disk data, not garbage/all-zero/
 *     all-0xFF.
 *
 *     NOTE ON THE "EB 3C 90" CHECK: the roadmap's acceptance text
 *     assumes LBA 0 holds a standard FAT boot sector, which begins
 *     with the 3-byte jump "EB 3C 90". LumaOS's LBA 0 is instead
 *     boot/mbr.asm's own custom (non-FAT) bootloader — its real
 *     first bytes assemble to "FA 31 C0 8E D8 ..." (cli; xor ax,ax;
 *     ...), confirmed by hand-assembling mbr.asm. That's expected and
 *     correct for this project: it proves the read pulled real,
 *     deterministic bytes off LBA 0 rather than confirming a FAT
 *     signature that doesn't apply until a FAT-formatted partition/
 *     image is read (Day 5+ MBR-partition work, and the "mkfs.fat"
 *     test images built starting Day 16). Don't "fix" this to force
 *     EB 3C 90 to appear — that would mean writing a fake boot sector
 *     over the real one.
 *
 * DAY4 (Week 1, Day 4 of the 5.0 filesystem roadmap):
 *   - After the Day 3 LBA 0 read/hex-dump, add the Day 4 acceptance
 *     test: build a test string in a stack buffer, zero-pad it to a
 *     full 512-byte sector, ide_write_sectors() it to LBA 2000, then
 *     ide_read_sectors() it back into a *separate* buffer and
 *     memcmp() the two.
 *
 * DAY5 (Week 1, Day 5 of the 5.0 filesystem roadmap):
 *   - "解析 MBR（LBA 0），扫描分区表（偏移 0x1BE）。识别第一个活动分区
 *      （Type 0x0C = FAT32 LBA，0x06 = FAT16，0x01 = FAT12）。在终端
 *      打印出分区起始 LBA 和大小。"
 *   - Reuses the same mbr_buf already read for the Day 3 hex-dump
 *     (LBA 0 is only read from disk once per boot) and hands it to
 *     the new kernel/drivers/block/mbr.{h,c} parser: mbr_parse()
 *     copies out the 4 raw partition-table entries at offset 0x1BE
 *     and checks the 0x55AA signature, mbr_dump() prints every
 *     non-empty entry's start LBA and sector count to the console
 *     (the Day 5 acceptance criteria), and
 *     mbr_find_first_active_fat() reports which entry (if any) is
 *     both active (status 0x80) and one of the three recognized FAT
 *     types.
 *   - NOTE: build.sh's disk image (see its DAY1/DAY2 comments) does
 *     not itself carve out a partitioned FAT volume — LBA 0 holds
 *     LumaOS's own custom mbr.asm bootloader, not a partitioned
 *     disk, so an all-empty/all-inactive partition table here is the
 *     *expected* result on the stock image. This step proves the
 *     scanner itself is correct (offsets, signature check, type
 *     matching); Day 6+ block-device work and the Day 16+ FAT test
 *     images are what will actually populate a matching partition
 *     table to exercise the "found a partition" path.
 *
 * DAY6 (Week 1, Day 6 of the 5.0 filesystem roadmap):
 *   "封装块设备层：struct block_device 包含 read(lba, count, buf) 函数
 *    指针。上层代码不再直接调用 ide 函数，统一走设备结构体。"
 *
 *   Every ide_read_sectors()/ide_write_sectors() call this file made
 *   directly on Day 3/4 is replaced with a call through a
 *   `block_device_t *` obtained from ide0_block_device()
 *   (kernel/drivers/block/block.{h,c}): dev->read(dev, lba, count,
 *   buf) and dev->write(dev, lba, count, buf) instead of
 *   ide_read_sectors(lba, count, buf) / ide_write_sectors(lba, count,
 *   buf). Functionally identical output (same acceptance checks, same
 *   hex dump, same LBA 2000 write/readback test, same MBR parse) —
 *   this step is purely about routing through the new abstraction so
 *   Week 2's VFS/FAT code has a device-independent interface to build
 *   on instead of naming the IDE driver directly. ide_test() itself
 *   is left as a direct call: it's an IDE-specific diagnostic (probes
 *   the raw status register), not a sector I/O operation, so it has
 *   no block_device_t equivalent and isn't in scope for this
 *   abstraction.
 *
 *     LBA 2000 (NOT LBA 1!) is used deliberately. build.sh lays out
 *     the image as: mbr.bin @ LBA 0, loader.bin @ LBA 1-5, kernel.bin
 *     @ LBA 6 through (6 + MAX_LOAD/512 - 1) = LBA 261 (MAX_LOAD is
 *     131072 bytes = 256 sectors). LBA 1 is loader.bin's *on-disk*
 *     location — writing there doesn't corrupt the current boot
 *     (loader.bin is already loaded into RAM and finished running by
 *     the time kmain() executes), but it permanently corrupts
 *     loader.bin inside lumaos-5.0.img itself, so the *next* boot
 *     from that same image file loads garbage in place of the real
 *     second-stage loader and fails at the BIOS INT13h/AH=42h read
 *     with "DISK ERROR" (confirmed via GDB: EIP lands in loader.asm's
 *     disk_err handler with AH=0x04). LBA 2000 sits far past LBA 261
 *     (still well inside the 2880-sector/1.44MB image build.sh
 *     creates) so it can never overlap mbr/loader/kernel on-disk,
 *     making the write-then-reread test idempotent across reboots.
 *
 * DAY7 (Week 1, Day 7 of the roadmap — weekly review):
 *   "整理 IDE 驱动代码，消除硬编码，增加超时保护（防止死循环）。
 *    写一个 ide_dump_info() 打印硬盘型号（IDENTIFY 命令 0xEC）。"
 *
 *   This file's only change: one new call, ide_dump_info(), right
 *   after ide_test() and before the Day 6 block_device_t section
 *   (grouped with ide_test() since both are IDE-specific diagnostics
 *   with no block_device_t equivalent — see the Day 6 note above on
 *   why ide_test() itself isn't routed through the abstraction
 *   either). The IDE driver's own cleanup (io_wait()-paced polling,
 *   the raised IDE_WAIT_TIMEOUT, and the new shared
 *   ide_setup_lba28() helper replacing duplicated register-setup
 *   code) lives entirely in kernel/drivers/block/ide.c and doesn't
 *   change this file's behavior — same hex dump, same LBA 2000
 *   write/readback result, same MBR parse as Day 6, just with more
 *   resilient timeout handling underneath.
 *
 * DAY9 (Week 2, Day 9 of the 5.0 filesystem roadmap):
 *   "创建 kernel/fs/fat/fat12.h。定义 BPB 结构体（bpb_12_t）和目录项
 *    结构（fat_dirent_t）。写一个 fat12_parse_bpb(buf)，能算出
 *    RootDir 扇区数、Data 起始扇区。"
 *
 *   Added a new section right after the Day 5 MBR-parse block: reads
 *   LBA 0 through the same disk->read(...) block_device_t call
 *   already used for the Day 3/5 mbr_buf (no second disk read —
 *   mbr_buf is reused again, same as Day 5 reused Day 3's read), and
 *   hands it to fat12_parse_bpb() (kernel/fs/fat/fat12.{h,c}).
 *
 *   NOTE ON WHY THIS "PARSES" LumaOS's OWN mbr.asm SECTOR: exactly
 *   like Day 3/5's note about LBA 0 not being a real FAT boot sector,
 *   fat12_parse_bpb()'s signature+field sanity check is *expected* to
 *   report failure (return -1) on the stock image, since LBA 0 here
 *   is LumaOS's own custom bootloader, not a FAT12-formatted volume.
 *   That's the correct, expected result — it proves the parser's
 *   sanity check and geometry arithmetic run without crashing on
 *   arbitrary sector data; a real "RootDirSectors/first data sector"
 *   result that matches a known-good FAT12 image is exercised once
 *   Day 10's fat12_mount() is pointed at an actual `mkfs.fat -F 12`
 *   test image (Day 16+ introduces the FAT16/32 test-image tooling
 *   this roadmap step is a precursor to). Printing the geometry
 *   fields regardless of the sanity-check result (same "print
 *   anyway, treat with suspicion" contract mbr_parse()/mbr_dump()
 *   already use) is what satisfies Day 9's acceptance criteria today
 *   without a FAT-formatted image yet on hand.
 *
 * DAY9-FOLLOWUP (verifying fat12_parse_bpb() against a REAL FAT12
 * volume, not just LBA 0's expected-invalid result):
 *   Added a second read/parse pass right after the LBA 0 one, this
 *   time against LBA 2500, which build.sh now bakes a genuine
 *   `mkfs.fat -F 12` boot sector onto (tests/fat12-bpb.bin — see
 *   build.sh's DAY9-FOLLOWUP comment for the exact layout/rationale).
 *   This is the positive-case counterpart to the LBA 0 negative case
 *   above: same fat12_parse_bpb() call, but against a sector that
 *   really is a valid FAT12 BPB, so bpb2_ok is expected to be true
 *   and the printed RootDirSectors/FirstDataSector/etc. are expected
 *   to be real, correct geometry (14/19/33/1/2880 for the specific
 *   1.44MB image tests/fat12-bpb.bin's boot sector came from) rather
 *   than the LBA 0 pass's garbage-but-harmless numbers.
 *
 * DAY10 (Week 2, Day 10 of the 5.0 filesystem roadmap):
 *   "实现 fat12_mount(block_dev)：读取引导扇区，检查 BS_FileSysType
 *    是否为 FAT12。挂载成功返回 0，失败返回 -1，在终端打印
 *    'FAT12 mounted'。"
 *
 *   Added two fat12_mount() calls right after the Day 9/9-followup
 *   parsing section: one against LBA 2500 (the same real FAT12
 *   fixture from the Day 9-followup section — expected to succeed,
 *   prints "FAT12 mounted"), and one against LBA 0 (LumaOS's own
 *   non-FAT mbr.asm sector — expected to fail with -1, demonstrating
 *   the acceptance criteria's other required path). Unlike the Day 9
 *   tests, which read the sector here in kmain.c and passed the
 *   buffer to fat12_parse_bpb(), fat12_mount() does its own
 *   dev->read(...) internally — kmain.c just passes it the
 *   block_device_t and an LBA. See kernel/fs/fat/fat12.{h,c}'s DAY10
 *   comments for fat12_mount()'s full contract.
 *
 * DAY11 (Week 2, Day 11 of the 5.0 filesystem roadmap):
 *   "实现 FAT12 簇链遍历函数 fat12_next_cluster(cluster)（处理 1.5
 *    字节对齐，注意奇偶簇）。给定根目录的起始簇（通常为 0），能遍历
 *    打印出前 10 个簇号。"
 *
 *   NOTE ON "根目录的起始簇（通常为 0）": FAT12's root directory is a
 *   fixed-size region (fat12_geometry_t.first_root_dir_sec), not a
 *   cluster chain — there's no root-directory "starting cluster" to
 *   hand fat12_next_cluster() on FAT12/16 (see fat12.h's DAY11
 *   comment). This section instead: (a) walks from cluster 2 — the
 *   first legal data cluster on any FAT12 volume — which is exactly
 *   BIGFILE.BIN's actual start cluster in the fixture below, so the
 *   10 printed values are a real, verifiable file's cluster chain,
 *   not arbitrary numbers; and (b) separately queries clusters 0 and
 *   1 (the two truly reserved FAT entries) to demonstrate the walker
 *   recognizes them as reserved/end-of-chain-range values rather than
 *   crashing or looping — the closer reading of what "簇 0 通常" is
 *   actually pointing at.
 *
 *   FIXTURE: tests/fat12-bpb.bin (Day 9/10, LBA 2500) is only a
 *   512-byte boot sector — everything past it on the disk image is
 *   whatever was already zeroed there, so its FAT table has no real
 *   entries and can't exercise the odd/even 1.5-byte packing with an
 *   actual multi-cluster chain. build.sh now also embeds a SECOND,
 *   larger fixture, tests/fat12-with-file.img, at LBA 5000: a
 *   complete 1.44MB `mkfs.fat -F 12` image (512 bytes/sector, 1
 *   sector/cluster) containing two real files copied in with mtools
 *   (mcopy) — BIGFILE.BIN (10240 bytes = exactly 20 clusters, so
 *   "print 10 cluster numbers" has 10 genuine, non-repeating chain
 *   links to show) and SMALL.BIN (1536 bytes = 3 clusters, included
 *   per the roadmap's "至少1-2个" but not otherwise used by this
 *   test). tests/fat12-bpb.bin/LBA 2500 is untouched — this is a
 *   second, independent mount, not a replacement.
 *
 *   CROSS-VALIDATION (done on the host, outside the kernel build, by
 *   parsing tests/fat12-with-file.img's BPB/FAT/root-directory bytes
 *   directly in a Python script — same BPB geometry formulas as
 *   fat12_parse_bpb(), same odd/even FAT12 unpacking as
 *   fat12_next_cluster(), reimplemented independently rather than by
 *   calling this project's own code, and cross-checked against
 *   `fsck.fat -v`'s "23/2847 clusters" used-cluster count and an
 *   `mcopy`-extracted-and-`cmp`'d-byte-identical copy of BIGFILE.BIN):
 *     - BIGFILE.BIN's directory entry: DIR_FstClusLO = 2, file size
 *       10240 bytes (= 20 * 512-byte clusters).
 *     - Expected first 10 fat12_next_cluster() outputs, starting from
 *       cluster 2 and feeding each result back in as the next call's
 *       argument:
 *           2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 8 -> 9 -> 10 -> 11 -> 12
 *         i.e. the sequence of RETURNED values (not counting the
 *         initial 2, which is the starting point, not a return value)
 *         is: 3, 4, 5, 6, 7, 8, 9, 10, 11, 12.
 *       (The full chain continues contiguously through cluster 22,
 *       then FAT[22] = 0xFFF marks end-of-chain — this project's
 *       fixture happens to be built on a freshly-formatted, otherwise
 *       empty volume, so mtools allocated BIGFILE.BIN's 20 clusters
 *       fully contiguously; that's a property of how the fixture was
 *       built; fat12_next_cluster() itself doesn't assume or require
 *       contiguity.)
 *     - FAT[0] = 0xFF0 (media-descriptor-derived) and FAT[1] = 0xFFF
 *       (reads as an ordinary end-of-chain value) — both are RESERVED
 *       BY DEFINITION (the spec never lets a real file claim clusters
 *       0/1), regardless of whether the stored value happens to fall
 *       in the FAT12_CLUSTER_EOC_MIN..MAX range or not (0xFF0 doesn't;
 *       0xFFF does) — confirming the "walking cluster 0 or 1 doesn't
 *       crash or loop" expectation from part (b) above.
 *   The section below prints fat12_next_cluster()'s ACTUAL output for
 *   this exact scenario; comparing the on-screen QEMU output against
 *   the expected values documented here is the acceptance check for
 *   this Day.
 *
 * ============================================================================
 * !! 非常重要 / IMPORTANT — 月末（Day 29 开始）前必须恢复的临时改动 !!
 * ============================================================================
 * 依据路线图第一个月建议（"第 1 个月千万别碰 GUI：文件系统调试期间，只使用
 * 文本终端"），本文件从 DAY2 起临时禁用了两处 GUI 相关调用，让 QEMU 全程
 * 停留在纯文本控制台模式，方便驱动/文件系统的快速调试迭代：
 *
 *   1) vga_init_graphics() 调用本身 —— 见下方 [6/7] 步骤处的注释块。
 *      这一步会把显示器从文本模式切到 BGA 800x450 图形模式；一旦切换，
 *      文本控制台（console_write 系列，写 0xB8000）的输出就再也不会显示
 *      在屏幕上了（黑屏），即使程序运行完全正常。DAY2 之前的版本只注释
 *      掉了下面第 2 条却漏了这一条，导致"程序其实没崩溃，但屏幕全黑"。
 *
 *   2) gui_init() 及其后的 gui_refresh() 主循环 —— 见 kmain() 末尾的
 *      注释块。这是 GUI 桌面/窗口系统的入口，正常情况下它会接管屏幕并
 *      持续绘制界面。
 *
 * 这两处必须【同时】保持注释状态或【同时】恢复，不能只改其中一个：
 *   - 只注释 (2) 不注释 (1) → 会黑屏（已实测复现的问题）。
 *   - 只注释 (1) 不注释 (2) → gui_init() 会在文本模式下强行接管显示，
 *     行为未定义，不要这样做。
 *
 * 本月（Day 1~28，文件系统与磁盘驱动开发期）请保持两处均为注释状态。
 * 第 29 天开始 GUI/桌面相关工作前，请【一次性】取消两处的注释，恢复成
 * 正常的图形模式 + gui_init() 主循环。
 * ============================================================================
 */
#include "kernel.h"
#include "../drivers/block/ide.h"
#include "../drivers/block/block.h"
#include "../drivers/block/mbr.h"
#include "../fs/fat/fat12.h"
#include "../fs/fat/fat_common.h"  /* DAY15: fat_mount(), fat_type_t, FAT_TYPE_*,
                                       fat_bpb_common_t, fat_geometry_t — pulled in
                                       transitively via fat12.h too, but included
                                       explicitly since this file's new DAY15 test
                                       section below uses these names directly */
#include "../fs/vfs/vfs.h"       /* DAY13: vfs_root, vfs_mount() */
#include "../shell/shell.h"      /* DAY14: shell_init(), shell_exec(),
                                     terminal_t (pulled in via shell.h's
                                     own #include "../gui/gui.h") */
#include "../drivers/keyboard.h"  /* DAY14 bugfix: KEY_UP/DOWN/LEFT/RIGHT/
                                      HOME/END constants for the text-mode
                                      line editor's arrow-key handling */

static const char banner[] =
    "LumaOS 4.1 Kernel\n"
    "==================\n";

/* ============================================================================
 * DAY14 (Week 2, Day 14 of the 5.0 filesystem roadmap):
 *   "整合 Shell：新增命令 cmd_ls（调用 vfs_readdir 列出文件）。在终端输入
 *    ls，能看到 FAT 镜像里的所有文件。"
 *   {大注意：此时需要在非GUI模式中启用shell再操作！}
 *
 * WHY A NEW DRIVER FUNCTION, NOT JUST "turn the GUI back on":
 *   shell_exec()/cmd_table (kernel/shell/shell.c) and every cmd_*.c
 *   file, including this Day's new cmd_ls, are written against
 *   `terminal_t *t` — kernel/gui/gui.h's 400-row/120-col in-memory
 *   scrollback buffer, normally driven by keyboard events routed
 *   through the GUI window system (window1.c's terminal_handle_key(),
 *   fed by gui_refresh()'s main loop) and displayed by
 *   terminal_render() drawing glyphs into the BGA graphics
 *   framebuffer. None of that is available this month: this file's
 *   own "非常重要" header block disables BOTH vga_init_graphics() and
 *   gui_init() through Day 28, staying in plain 80x25 VGA TEXT mode
 *   (0xB8000, console_* functions) for fast driver/filesystem
 *   iteration. Re-enabling the GUI just to reach `ls` would violate
 *   that standing rule and re-introduce the exact "screen goes black"
 *   risk documented at the top of this file.
 *
 *   The fix is NOT a new shell — shell_init()/shell_exec()/cmd_table
 *   and every existing command (including today's cmd_ls) are reused
 *   completely unmodified. What's missing is only the wiring around
 *   them: something to read keystrokes without the GUI's key-routing,
 *   and something to get terminal_t's buffered output onto the real
 *   text-mode screen without terminal_render()'s graphics-only
 *   drawing code. text_shell_run() below is exactly that adapter —
 *   the text-mode equivalent of what terminal_handle_key() +
 *   terminal_render() do for the GUI, nothing more:
 *
 *     1. INPUT: keyboard_getchar() (kernel/drivers/keyboard.c) is a
 *        blocking, interrupt-driven read that already existed before
 *        Day 14 (Day 4/testing infrastructure) and needs no GUI to
 *        work — it just pops bytes off the IRQ1 ring buffer. Each
 *        typed line is assembled into a small local buffer and echoed
 *        straight to the VGA console via console_putc_color() (Enter/
 *        Backspace map onto console_putc_color()'s existing '\n'/'\b'
 *        handling — see console.c — no new console code needed).
 *     2. DISPATCH: once Enter is pressed, the completed line is
 *        handed to shell_exec(&g_text_shell, line) exactly as
 *        terminal_handle_key() already does for the GUI path
 *        (window1.c) — shell_exec() itself has no idea whether its
 *        terminal_t is destined for a graphics window or a text-mode
 *        adapter, which is the whole point of keeping cmd_*.c
 *        GUI-agnostic.
 *     3. OUTPUT: shell_exec() -> the invoked cmd_*() function calls
 *        terminal_puts()/terminal_putchar()/terminal_newline()
 *        (window1.c), which — see that file's own functions — only
 *        ever write into g_text_shell's in-memory `lines[]` array;
 *        they never touch graphics hardware (only terminal_render()
 *        does that, and this file never calls it). So after each
 *        shell_exec() call, text_shell_flush_new_lines() below simply
 *        walks whichever of g_text_shell.lines[] are new since the
 *        last flush and prints THEIR TEXT (not colors — term_line_t
 *        stores a color per cell, but the simplest correct adapter
 *        for Day 14's acceptance bar re-prints each captured line in
 *        one console_write_color() call; a per-cell color remap is a
 *        nice-to-have, not required by "能看到 FAT 镜像里的所有文件")
 *        onto the real 0xB8000 console via console_write_color().
 *
 *   This keeps every later shell command (Day 15 on) working for free
 *   in text mode with zero changes to shell.c/cmd_*.c, and once Day 29
 *   restores the GUI, this whole text_shell_run() function simply
 *   stops being called (kmain() goes back to gui_init()+gui_refresh())
 *   — it does not need to be deleted, since it costs nothing sitting
 *   unused, but it also isn't in the GUI's call path in any way.
 * ============================================================================
 */

/* g_text_shell: the SAME terminal_t shape the GUI terminal window
 * uses (kernel/gui/gui.h), reused here purely as an in-memory command
 * buffer/dispatch target — never rendered to the graphics framebuffer.
 * Static (not a kmain() local) for the same reason kmain.c's other
 * large fixture buffers are static: this struct is a few dozen KB
 * (TERM_BUF_ROWS * TERM_BUF_COLS cells) and this project has no
 * kernel heap yet (Day 73). */
static terminal_t g_text_shell;

/* ---------------------------------------------------------------------
 * BUGFIX (post-DAY14, found from an actual boot screenshot): the first
 * version of this file tracked an ABSOLUTE line-number watermark
 * (g_text_shell_flushed_lines) and only printed g_text_shell lines at
 * index >= that watermark. That silently breaks the moment
 * g_text_shell.total_lines stops growing: window1.c's terminal_newline()
 * caps total_lines at TERM_BUF_ROWS (400) and, once the buffer is
 * full, starts SHIFTING every line down by one slot per newline
 * instead of incrementing total_lines any further (a ring-buffer-style
 * scroll — see terminal_newline()'s own body). After that point
 * total_lines never changes again, so a watermark that was already
 * sitting at 400 makes the very next flush's `for (i = 400; i <
 * total_lines; i++)` loop never execute — new command output is
 * computed correctly by cmd_ls/etc, but silently never reaches the
 * real screen. This is exactly what the reported screenshot showed:
 * the SECOND `ls` printed only SMALL.BIN, not BIGFILE.BIN, because by
 * then g_text_shell had scrolled past the 400-line mark and the
 * watermark-based flush stopped emitting anything for one line's
 * worth of output.
 *
 * FIX: don't track a cross-command watermark at all. terminal_clear()
 * g_text_shell before every shell_exec() call, so each command starts
 * from an empty buffer (total_lines reset to 1, see terminal_clear()),
 * and text_shell_flush_new_lines() below always prints from index 0.
 * No single shell command in this project prints anywhere close to
 * 400 lines, so the "does total_lines ever hit the ring-buffer cap"
 * failure mode this bug came from is now structurally unreachable,
 * not just patched around.
 * --------------------------------------------------------------------- */

/* text_shell_flush_new_lines()
 *
 * Prints every line currently in g_text_shell (index 0 through
 * total_lines-1) to the real text-mode console. Called once per
 * shell_exec(), right after it, with g_text_shell freshly cleared
 * beforehand by the caller — see the BUGFIX note above for why this
 * always starts at 0 rather than tracking a watermark. Reads
 * term_line_t.text[0..len) directly (NUL-terminating into a small
 * stack copy first, since term_line_t.text is not itself
 * NUL-terminated — it's a fixed TERM_BUF_COLS array padded with
 * spaces, see window1.c's terminal_init()/terminal_clear()).
 *
 * Per-cell color (term_line_t.color[]) is intentionally not consulted
 * here — see this file's DAY14 header comment above for why a single
 * COLOR_DEFAULT per line is the correct scope for Day 14 rather than
 * a full per-cell color remap. */
/* term_color_to_vga(gui_color)
 *
 * BUGFIX (found from a review of actual boot output): the first
 * version of text_shell_flush_new_lines() printed every line in one
 * flat COLOR_DEFAULT, discarding term_line_t.color[]'s per-character
 * colors entirely — which is why cmd_shell.c commands that rely on
 * color to communicate meaning (cmd_help.c's TERM_COL_CMD command
 * names, shell.c's own TERM_COL_ERROR "Unknown: foo" message,
 * cmd_color.c's whole reason for existing) all rendered as one flat
 * color in the text-mode shell even though they call terminal_puts()
 * with a real, distinct color argument every time.
 *
 * The reason this couldn't just be "pass ln->color[j] straight to
 * console_putc_color()" is that gui.h's TERM_COL_* constants are NOT
 * VGA text-mode attribute values — most of them (TERM_COL_DEFAULT =
 * COL_TERM_FG = 27, TERM_COL_CMD = COL_TEXT_PRIMARY = 40) are BGA
 * graphics-mode PALETTE INDICES from gui.h's extended UI theme
 * palette (COL_DESKTOP..COL_BTN_CLOSE and beyond, see gui.h), which
 * go well past VGA text mode's legal 0-15 foreground range. Writing
 * one of those raw into a VGA text attribute byte doesn't just pick
 * the "wrong" color — many values corrupt the BACKGROUND nibble too
 * (a VGA text attribute byte packs background in bits 4-7), which is
 * worse than the flat-gray fallback this replaces.
 *
 * So every TERM_COL_* value cmd_*.c code in this project actually
 * uses is mapped by hand below to its nearest VGA 0-15 foreground
 * color. This is a lookup table, not a formula, because the two
 * palettes don't share any numeric relationship (COL_TERM_FG=27 has
 * nothing to do with a VGA "27") — it only needs to cover the colors
 * this project's shell code actually emits (cmd_color.c's COLOR
 * command sets raw 0-15 values directly, which pass through
 * unchanged; everything else funnels through the small set of
 * TERM_COL_* names declared in gui.h). Anything unrecognized falls
 * back to COL_LGRAY (7), the standard VGA "default text" color,
 * rather than crashing or silently going black-on-black. */
static uint8_t term_color_to_vga(uint8_t gui_color)
{
    /* Raw VGA 0-15 values (as cmd_color.c's `color <n>` command uses
     * directly) pass through unchanged — this is what lets `color 1`,
     * `color 3`, etc. keep working exactly as before. */
    if (gui_color <= COL_WHITE)
        return gui_color;

    switch (gui_color) {
    case TERM_COL_DEFAULT: return COL_LGRAY;    /* COL_TERM_FG   -> light gray */
    case TERM_COL_CMD:     return COL_WHITE;    /* COL_TEXT_PRIMARY -> white   */
    /* TERM_COL_PROMPT/SUCCESS (COL_LGREEN), TERM_COL_ERROR (COL_LRED),
     * TERM_COL_INFO (COL_LCYAN) are all <= COL_WHITE already and
     * handled by the pass-through check above — listed here only for
     * documentation, these cases are unreachable. */
    default:
        return COL_LGRAY;
    }
}

/* text_shell_flush_new_lines()
 *
 * Prints every line currently in g_text_shell (index 0 through
 * total_lines-1) to the real text-mode console, ONE CHARACTER AT A
 * TIME through console_putc_color(), using each cell's own
 * ln->color[j] (mapped through term_color_to_vga() — see that
 * function's comment for why a straight pass-through doesn't work)
 * instead of one flat color per line. This is what restores the
 * "Unknown: foo" error appearing in red, `color N` actually changing
 * output color, cmd_help.c's command names appearing in a different
 * color from their descriptions, etc. — all of which cmd_*.c code
 * already requests via terminal_puts()'s color argument; only the
 * flush step was throwing that information away.
 *
 * Called once per shell_exec(), right after it, with g_text_shell
 * freshly cleared beforehand by the caller — see the earlier BUGFIX
 * note (above g_text_shell_flushed_lines' old declaration site) for
 * why this always starts at line index 0 rather than tracking a
 * watermark. */
static void text_shell_flush_new_lines(void)
{
    for (int i = 0; i < g_text_shell.total_lines; i++) {
        term_line_t *ln = &g_text_shell.lines[i];
        int len = ln->len;
        if (len > TERM_BUF_COLS) len = TERM_BUF_COLS;
        for (int j = 0; j < len; j++) {
            console_putc_color(ln->text[j], term_color_to_vga(ln->color[j]));
        }
        console_putc('\n');
    }
}

/* text_shell_prompt()
 *
 * Prints the shell prompt, reflecting the actually-mounted device
 * name (vfs_root->name, "IDE0" per vfs.c/fat12_vfs.c's numbered-
 * device-string convention — see vfs.c's header comment for why it's
 * "IDE0" and not bare "IDE") plus a trailing "/" before the ">".
 *
 * FORMAT NOTE (per 计划.md's "LumaOS 9.0 目录架构规范"): the final
 * architecture mounts IDE's root at paths like IDE/Boot/, IDE/System/
 * Kernel/, IDE/User/User_1/Documents/ — i.e. the shell's current
 * location is always a PATH, not a bare device name. The "/" here is
 * that path's root separator, shown now even though Day 14's shell
 * has no `cd`/subdirectory support yet (cmd_ls.c only lists
 * vfs_root's immediate children) specifically so the prompt format
 * doesn't need a breaking change later: once a Day well past 14 adds
 * a "current directory" concept, this becomes
 * "IDE/System/Kernel> " — filling in the same slot this "/" already
 * occupies — instead of every earlier "IDE0> "-style prompt needing a
 * retrofit. g_text_shell_cwd below is that future extension point,
 * deliberately kept as just "" for now (see its own comment).
 *
 * Falls back to "LumaOS/ " — the OS's own name, keeping the same
 * trailing-"/" path convention — only if vfs_mount("IDE0") failed and
 * vfs_root is still NULL, so the prompt never dereferences a NULL
 * vfs_root->name (same defensive NULL check cmd_ls.c's own body
 * already applies for the same underlying reason). */

/* g_text_shell_cwd: reserved for the future "current directory"
 * component of the prompt (e.g. "/System/Kernel") once `cd` and
 * subdirectory listing exist — see the LumaOS 9.0 directory layout
 * this file's DAY14 comments reference. Deliberately unused/empty
 * today: Day 14's vfs_root has no child-directory traversal yet
 * (fat12_vfs.c's readdir only enumerates one flat directory), so
 * there is nothing real to put here. Kept as a named, documented slot
 * rather than leaving a bare "/" as a magic string, so wiring `cd` up
 * later is a one-line change to this variable instead of a rewrite of
 * text_shell_prompt(). */
static char g_text_shell_cwd[64] = "";

static void text_shell_prompt(void)
{
    if (vfs_root && vfs_root->name[0]) {
        console_write_color(vfs_root->name, COLOR_PROMPT);
        console_write_color(g_text_shell_cwd, COLOR_PROMPT);
        console_write_color("/ > ", COLOR_PROMPT);
    } else {
        console_write_color("LumaOS", COLOR_PROMPT);
        console_write_color(g_text_shell_cwd, COLOR_PROMPT);
        console_write_color("/ > ", COLOR_PROMPT);
    }
}

/* ---------------------------------------------------------------------
 * DAY14 line editor (BUGFIX round 2 — history/cursor movement)
 *
 * The first version of text_shell_run() read keyboard_getchar() into
 * a flat local `char line[128]` buffer with backspace-only editing,
 * explicitly filtering out anything >= 0x80 — which silently threw
 * away KEY_UP/KEY_DOWN/KEY_LEFT/KEY_RIGHT (keyboard.h defines these
 * as 0x80-0x83) instead of acting on them. That matched the original
 * DAY14 scope note ("no arrow-key history recall in this text-mode
 * path"), but the reviewer correctly pointed out this isn't
 * acceptable — the GUI terminal (terminal_handle_key(), window1.c)
 * already has working history/cursor movement, and text_shell_run()
 * should not be a strictly worse line editor.
 *
 * Rather than inventing a parallel history/cursor implementation,
 * this reuses the EXACT SAME STATE FIELDS terminal_handle_key()
 * already maintains on a terminal_t — t->input[]/input_len/
 * input_cursor for the in-progress line, t->history[]/history_count/
 * history_pos for recall (all declared once in gui.h, shared by both
 * the GUI and text-mode code paths on the SAME g_text_shell struct).
 * What differs is only how a change gets drawn: terminal_handle_key()
 * sets t->redraw_needed and lets terminal_render() (graphics-only,
 * still never called from this file) repaint the whole GUI window;
 * text_shell_redraw_input_line() below instead repositions the real
 * VGA text-mode cursor and rewrites just the input line in place —
 * the text-console equivalent of the same redraw-on-change idea.
 * --------------------------------------------------------------------- */

/* text_shell_redraw_input_line(prompt_row, prompt_col)
 *
 * Rewrites the current input line (g_text_shell.input, up to
 * input_len bytes) starting at VGA text position (prompt_col,
 * prompt_row) — i.e. right after the prompt, which the caller must
 * have already printed. Pads over any leftover characters from a
 * previous, longer draw of this same line (e.g. after Backspace/Del
 * shortens it, or after loading a shorter history entry) with spaces,
 * then places the hardware cursor at prompt_col + input_cursor, so
 * LEFT/RIGHT visibly move the blinking cursor within the line instead
 * of only at its end. */
static void text_shell_redraw_input_line(int prompt_row, int prompt_col)
{
    static int prev_drawn_len = 0;

    console_set_cursor(prompt_col, prompt_row);
    for (int i = 0; i < g_text_shell.input_len; i++) {
        console_putc_color(g_text_shell.input[i], COLOR_DEFAULT);
    }
    int pad_from = g_text_shell.input_len;
    int pad_to   = prev_drawn_len;
    for (int i = pad_from; i < pad_to; i++) {
        console_putc_color(' ', COLOR_DEFAULT);
    }
    prev_drawn_len = g_text_shell.input_len;

    console_set_cursor(prompt_col + g_text_shell.input_cursor, prompt_row);
}

/* text_shell_history_push/load — thin wrappers around g_text_shell's
 * own history[]/history_count/history_pos fields, mirroring
 * window1.c's static history_push()/history_load() exactly (same
 * field names, same modulo-TERM_HISTORY ring behavior) since those
 * two functions are `static` to window1.c and not exported — rather
 * than exporting them, this project's convention (see fat12_vfs.c's
 * own "separate adapter file" split) is to keep the duplication small
 * and local instead of widening window1.c's API for one caller. */
static void text_shell_history_push(const char *cmd)
{
    if (!cmd[0]) return;
    int slot = g_text_shell.history_count % TERM_HISTORY;
    int ci = 0;
    while (cmd[ci] && ci < TERM_HIST_LEN - 1) {
        g_text_shell.history[slot][ci] = cmd[ci];
        ci++;
    }
    g_text_shell.history[slot][ci] = 0;
    g_text_shell.history_count++;
}

static void text_shell_history_load(void)
{
    if (g_text_shell.history_pos < 0) {
        g_text_shell.input[0] = 0;
        g_text_shell.input_len = 0;
        g_text_shell.input_cursor = 0;
        return;
    }
    int idx = (g_text_shell.history_count - 1 - g_text_shell.history_pos) % TERM_HISTORY;
    if (idx < 0) idx += TERM_HISTORY;
    int ci = 0;
    while (g_text_shell.history[idx][ci] && ci < TERM_INPUT_MAX) {
        g_text_shell.input[ci] = g_text_shell.history[idx][ci];
        ci++;
    }
    g_text_shell.input[ci] = 0;
    g_text_shell.input_len = ci;
    g_text_shell.input_cursor = ci;
}

static void text_shell_run(void)
{
    shell_init();
    terminal_init(&g_text_shell);

    console_write_color(
        "\n[shell] Text-mode shell ready. Type 'help' for commands.\n",
        COLOR_INFO);

    /* Requested: an explicit "starting the prompt on IDE0" status line,
     * separate from the one-time vfs_mount() success message printed
     * earlier by the DAY13 section above — that one reports the MOUNT
     * result; this one reports that the SHELL is now about to start
     * accepting input against that mount. Only claims "on IDE0" when
     * vfs_root is actually non-NULL and named "IDE0" — if the mount
     * failed, this says so plainly instead of lying about which
     * device the prompt is backed by. */
    if (vfs_root && vfs_root->name[0]) {
        printf("[shell] Start prompt on %s\n", vfs_root->name);
    } else {
        console_write_color(
            "[shell] Start prompt (no filesystem mounted)\n",
            COLOR_WARNING);
    }

    for (;;) {
        /* Each command's output is rendered into a freshly-cleared
         * g_text_shell (see text_shell_flush_new_lines()'s header
         * comment for why total_lines always starts back at 1 here
         * rather than tracking a cross-command watermark), so this
         * can never hit window1.c's TERM_BUF_ROWS ring-buffer-scroll
         * path for a single command's output. terminal_clear() does
         * NOT touch input_len/input_cursor/history[]/history_count/
         * history_pos (see its own body in window1.c) — those persist
         * across commands exactly as the GUI terminal's do, which is
         * what makes history recall span multiple commands. */
        terminal_clear(&g_text_shell);

        text_shell_prompt();
        int prompt_row = console_get_y();
        int prompt_col = console_get_x();

        g_text_shell.input[0] = 0;
        g_text_shell.input_len = 0;
        g_text_shell.input_cursor = 0;
        g_text_shell.history_pos = -1;

        for (;;) {
            char c = keyboard_getchar();
            unsigned char key = (unsigned char)c;

            if (key == KEY_UP) {
                if (g_text_shell.history_count > 0 &&
                    g_text_shell.history_pos < g_text_shell.history_count - 1) {
                    g_text_shell.history_pos++;
                    text_shell_history_load();
                }
                text_shell_redraw_input_line(prompt_row, prompt_col);
                continue;
            }
            if (key == KEY_DOWN) {
                if (g_text_shell.history_pos > 0) {
                    g_text_shell.history_pos--;
                    text_shell_history_load();
                } else if (g_text_shell.history_pos == 0) {
                    g_text_shell.history_pos = -1;
                    text_shell_history_load();
                }
                text_shell_redraw_input_line(prompt_row, prompt_col);
                continue;
            }
            if (key == KEY_LEFT) {
                if (g_text_shell.input_cursor > 0) g_text_shell.input_cursor--;
                console_set_cursor(prompt_col + g_text_shell.input_cursor, prompt_row);
                continue;
            }
            if (key == KEY_RIGHT) {
                if (g_text_shell.input_cursor < g_text_shell.input_len)
                    g_text_shell.input_cursor++;
                console_set_cursor(prompt_col + g_text_shell.input_cursor, prompt_row);
                continue;
            }
            if (key == KEY_HOME) {
                g_text_shell.input_cursor = 0;
                console_set_cursor(prompt_col, prompt_row);
                continue;
            }
            if (key == KEY_END) {
                g_text_shell.input_cursor = g_text_shell.input_len;
                console_set_cursor(prompt_col + g_text_shell.input_cursor, prompt_row);
                continue;
            }

            if (c == '\r' || c == '\n') {
                console_set_cursor(prompt_col + g_text_shell.input_len, prompt_row);
                console_putc('\n');
                break;
            }
            if (c == '\b') {
                /* Backspace: delete the char BEFORE the cursor
                 * (matches shell.c's simple model — keyboard.c's own
                 * keymap already gives us '\b' for the Backspace key,
                 * same as before this bugfix). */
                if (g_text_shell.input_cursor > 0) {
                    for (int i = g_text_shell.input_cursor - 1;
                         i < g_text_shell.input_len - 1; i++)
                        g_text_shell.input[i] = g_text_shell.input[i + 1];
                    g_text_shell.input_len--;
                    g_text_shell.input_cursor--;
                    g_text_shell.input[g_text_shell.input_len] = 0;
                    text_shell_redraw_input_line(prompt_row, prompt_col);
                }
                continue;
            }
            /* Any other non-printable/control byte (function keys,
             * PGUP/PGDN, Ctrl combos, KEY_DEL/KEY_INS, ...) is simply
             * not handled by this simple line editor — matches
             * shell.c's own "space-delimited argv" scope, same as
             * before this bugfix. */
            if (key < 0x20 || key > 0x7E)
                continue;

            if (g_text_shell.input_len < TERM_INPUT_MAX) {
                /* Insert at cursor (not always append) — shift
                 * everything from input_cursor onward right by one. */
                for (int i = g_text_shell.input_len; i > g_text_shell.input_cursor; i--)
                    g_text_shell.input[i] = g_text_shell.input[i - 1];
                g_text_shell.input[g_text_shell.input_cursor] = c;
                g_text_shell.input_len++;
                g_text_shell.input_cursor++;
                g_text_shell.input[g_text_shell.input_len] = 0;
                text_shell_redraw_input_line(prompt_row, prompt_col);
            }
        }

        text_shell_history_push(g_text_shell.input);
        shell_exec(&g_text_shell, g_text_shell.input);

        /* BUGFIX (reported: "clear 不好使"): cmd_clear.c's body is
         * just `terminal_clear(t)` — correct and sufficient for the
         * GUI path, where terminal_render() repaints the whole window
         * from g_terminal's (now-empty) buffer every frame, so an
         * empty buffer IS an empty screen. In this text-mode path
         * there is no equivalent repaint: text_shell_flush_new_lines()
         * only ever APPENDS g_text_shell's lines to whatever is
         * already on the real VGA screen, it never erases anything
         * already there. So after `clear` ran, g_text_shell was
         * correctly empty (flush prints nothing extra, matching this
         * function's own now-stale NOTE below) but the actual screen
         * — the thing the person is looking at — never got touched,
         * making the command look like a no-op.
         *
         * Fix: when the command that just ran was `clear` (bare, or
         * with any argv shell.c's parser would still route to
         * cmd_clear — this project's parser only ever looks at
         * argv[0], see shell.c's shell_exec()/shell_parse()), also
         * clear the REAL screen via console_clear() (console.c —
         * already used by console_init() at boot) and let the next
         * loop iteration's text_shell_prompt() print fresh at row 0
         * instead of wherever output had scrolled to. Detected by
         * comparing g_text_shell.input's first whitespace-delimited
         * token to "clear" here, rather than special-casing inside
         * cmd_clear.c itself — cmd_clear.c stays GUI/text-mode
         * agnostic like every other cmd_*.c file (see cmd_ls.c's own
         * header comment for why that separation matters). */
        {
            const char *p = g_text_shell.input;
            while (*p == ' ') p++;
            int i = 0;
            while (p[i] && p[i] != ' ' && i < 15) i++;
            if (i == 5 && p[0]=='c' && p[1]=='l' && p[2]=='e' && p[3]=='a' && p[4]=='r')
                console_clear();
        }

        text_shell_flush_new_lines();
    }
}
/* NOTE: g_text_shell is cleared at the TOP of each loop iteration
 * (right before text_shell_prompt()), not here at the bottom — this
 * matters for `clear`: cmd_clear.c's own body is `terminal_clear(t)`,
 * so if shell_exec() just ran `clear`, g_text_shell is already empty
 * and text_shell_flush_new_lines() correctly prints nothing extra;
 * the NEXT loop iteration's terminal_clear() at the top is then a
 * harmless no-op on an already-empty buffer. Separately, the REAL
 * screen is now also cleared right after shell_exec() above — see
 * that block's own comment for why that extra step was missing
 * before and made `clear` look broken in text mode specifically. */

void kmain(void)
{
    console_init();
    console_write_color(banner, COLOR_INFO);

    console_write_color("[1/7] GDT...\n", COLOR_INFO);
    gdt_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[2/7] IDT...\n", COLOR_INFO);
    idt_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[3/7] PIC...\n", COLOR_INFO);
    pic_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[4/7] Keyboard...\n", COLOR_INFO);
    keyboard_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[5/7] Mouse...\n", COLOR_INFO);
    mouse_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    /* ------------------------------------------------------------------
     * !! DAY2 临时改动 (禁用位置 1/2) — 月末 (Day 29) 前必须保持注释 !!
     * 详细原因见文件顶部的说明块。简述：调用 vga_init_graphics() 会把
     * 显示器切换到 BGA 800x450 图形模式，切换之后文本控制台的输出
     * (console_write 系列) 就不会再显示在屏幕上了 —— 这正是"程序其实
     * 跑得好好的，但屏幕全黑"这个现象的根因。第一个月保持文本模式调试，
     * 所以这里不调用它。要恢复图形模式时，取消下面两行注释，并同步
     * 取消文件末尾 gui_init() 那个块的注释（禁用位置 2/2）。
     *
     * console_write_color("[6/7] VGA BGA 800x450...\n", COLOR_INFO);
     * vga_init_graphics();
     * console_write_color("      OK\n", COLOR_SUCCESS);
     * ------------------------------------------------------------------ */
    console_write_color("[6/7] VGA BGA 800x450... (SKIPPED, month-1 text mode)\n", COLOR_WARNING);

    console_write_color("[7/7] Interrupts...\n", COLOR_INFO);
    enable_interrupts();
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[ide]  IDE driver test...\n", COLOR_INFO);
    ide_test();

    /* ------------------------------------------------------------------
     * DAY7: IDENTIFY (0xEC) — print the attached drive's model string
     * and total sector count. Like ide_test() above, this is an
     * IDE-specific diagnostic (no block_device_t equivalent), so it's
     * called directly rather than through the Day 6 abstraction below.
     * ------------------------------------------------------------------ */
    ide_dump_info();

    /* ------------------------------------------------------------------
     * DAY6: obtain the generic block_device_t for the primary IDE
     * disk. From here on, kmain() reads/writes sectors exclusively
     * through disk->read(...)/disk->write(...) — it never calls
     * ide_read_sectors()/ide_write_sectors() by name again below this
     * point. ide_test() above is exempt: it's a raw status-register
     * probe, not sector I/O, so it has no block_device_t equivalent.
     * ------------------------------------------------------------------ */
    block_device_t *disk = ide0_block_device();
    printf("[block] Using block device \"%s\" (sector_size=%u)\n",
           disk->name, disk->sector_size);

    /* ------------------------------------------------------------------
     * DAY3: read LBA 0 (the MBR) via PIO and hex-dump the first 64
     * bytes. Acceptance check per the roadmap: the first three bytes
     * should read "EB 3C 90" (a short jump + NOP), the standard FAT
     * boot-sector signature that boot/mbr.bin begins with.
     * ------------------------------------------------------------------ */
    console_write_color("[ide]  Reading LBA 0 (MBR) via PIO...\n", COLOR_INFO);
    /* DAY5: hoisted out of the old nested block (was `static uint8_t
     * mbr_buf[512];` scoped to just the hex-dump below) so the Day 5
     * partition-table scan further down can reuse the same 512-byte
     * read of LBA 0 instead of issuing a second read call for
     * identical data.
     * DAY6: now goes through disk->read(...) instead of calling
     * ide_read_sectors() directly. */
    static uint8_t mbr_buf[512];
    int mbr_read_ok = (disk->read(disk, 0, 1, mbr_buf) == 0);

    if (mbr_read_ok) {
        console_write_color("[ide]  OK: LBA 0 read (512 bytes)\n", COLOR_SUCCESS);
        console_write_color("[ide]  First 64 bytes:\n", COLOR_INFO);
        console_write_hex(mbr_buf, 64);
    } else {
        console_write_color("[ide]  FAIL: disk->read(0, 1, ...) failed\n",
                             COLOR_ERROR);
    }

    /* ------------------------------------------------------------------
     * DAY4: write a test string to LBA 2000, read it back, and compare.
     * LBA 2000 is used (NOT LBA 1) so this test can never overlap
     * mbr.bin (LBA 0), loader.bin (LBA 1-5), or kernel.bin (LBA
     * 6-261) on disk — see the file-header comment for why that
     * matters (overwriting loader.bin here would corrupt the image
     * for the *next* boot even though the *current* boot is fine).
     * DAY6: now goes through disk->write(...)/disk->read(...) instead
     * of calling ide_write_sectors()/ide_read_sectors() directly.
     * ------------------------------------------------------------------ */
    console_write_color("[ide]  Write/readback test on LBA 2000...\n", COLOR_INFO);
    {
        static const char test_msg[] = "LumaOS Day4 IDE write/read test - hello disk!";
        static uint8_t write_buf[512];
        static uint8_t read_buf[512];
        const uint32_t test_lba = 2000;

        memset(write_buf, 0, sizeof(write_buf));
        strncpy((char *)write_buf, test_msg, sizeof(write_buf));

        if (disk->write(disk, test_lba, 1, write_buf) != 0) {
            console_write_color("[ide]  FAIL: disk->write(2000, 1, ...) failed\n",
                                 COLOR_ERROR);
        } else {
            console_write_color("[ide]  OK: wrote 512 bytes to LBA 2000\n", COLOR_SUCCESS);

            memset(read_buf, 0, sizeof(read_buf));

            if (disk->read(disk, test_lba, 1, read_buf) != 0) {
                console_write_color("[ide]  FAIL: disk->read(2000, 1, ...) failed\n",
                                     COLOR_ERROR);
            } else if (memcmp(write_buf, read_buf, sizeof(write_buf)) == 0) {
                console_write_color("[ide]  OK: readback matches what was written\n",
                                     COLOR_SUCCESS);
                console_write_color("[ide]  Readback string: ", COLOR_INFO);
                console_write_color((const char *)read_buf, COLOR_DEFAULT);
                console_write_color("\n", COLOR_DEFAULT);
            } else {
                console_write_color("[ide]  FAIL: readback does NOT match written data\n",
                                     COLOR_ERROR);
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY5: parse the MBR partition table (offset 0x1BE of LBA 0) and
     * identify the first *active* FAT partition (type 0x0C/0x06/0x01).
     * Reuses mbr_buf from the Day 3 read above instead of reading
     * LBA 0 a second time. mbr_dump() prints every non-empty entry's
     * start LBA and sector count, satisfying the Day 5 acceptance
     * criteria; mbr_find_first_active_fat() then reports which entry
     * (if any) qualifies as "the first active partition".
     * ------------------------------------------------------------------ */
    console_write_color("[mbr]  Parsing partition table (LBA 0, offset 0x1BE)...\n",
                         COLOR_INFO);
    if (!mbr_read_ok) {
        console_write_color("[mbr]  SKIP: LBA 0 was not read successfully above\n",
                             COLOR_WARNING);
    } else {
        static mbr_partition_entry_t parts[MBR_PARTITION_COUNT];
        int sig_ok = (mbr_parse(mbr_buf, parts) == 0);

        if (sig_ok) {
            console_write_color("[mbr]  OK: boot signature 0x55AA present\n",
                                 COLOR_SUCCESS);
        } else {
            console_write_color(
                "[mbr]  NOTE: boot signature 0x55AA missing at offset 0x1FE "
                "(expected on LumaOS's own mbr.asm image — see kmain.c DAY5 "
                "header comment); parsing entries anyway.\n",
                COLOR_WARNING);
        }

        mbr_dump(parts);

        int active_idx = -1;
        const mbr_partition_entry_t *active =
            mbr_find_first_active_fat(parts, &active_idx);

        if (active) {
            printf("[mbr]  First active FAT partition: entry %d, type=%s, "
                   "start_lba=%u, size=%u sectors\n",
                   active_idx, mbr_type_name(active->type),
                   active->lba_start, active->sector_count);
        } else {
            console_write_color(
                "[mbr]  No active FAT partition found (table empty/inactive "
                "on this image — expected until a real FAT-partitioned "
                "image is used, Day 16+)\n",
                COLOR_WARNING);
        }
    }

    /* ------------------------------------------------------------------
     * DAY6 acceptance check: every sector I/O call above this point
     * (LBA 0 read, LBA 2000 write/readback) went through
     * disk->read(...)/disk->write(...) rather than calling
     * ide_read_sectors()/ide_write_sectors() by name — confirming the
     * block_device_t abstraction is what upper-layer code now uses.
     * ------------------------------------------------------------------ */
    printf("[block] OK: all sector I/O above went through block_device \"%s\"\n",
           disk->name);

    /* ------------------------------------------------------------------
     * DAY9: parse LBA 0 as a FAT12 BPB (fat12_parse_bpb()) and print
     * the two derived geometry values the roadmap's acceptance
     * criteria asks for: RootDirSectors and the first Data Region
     * sector. Reuses mbr_buf from the Day 3 read (already sitting in
     * memory) instead of a third read of LBA 0 — same reuse pattern
     * Day 5's MBR parse already established.
     * ------------------------------------------------------------------ */
    console_write_color("[fat12] Parsing LBA 0 as a FAT12 BPB...\n", COLOR_INFO);
    if (!mbr_read_ok) {
        console_write_color("[fat12] SKIP: LBA 0 was not read successfully above\n",
                             COLOR_WARNING);
    } else {
        static bpb_12_t bpb;
        static fat12_geometry_t geom;
        int bpb_ok = (fat12_parse_bpb(mbr_buf, &bpb, &geom) == 0);

        if (bpb_ok) {
            console_write_color("[fat12] OK: signature 0x55AA + BPB fields look valid\n",
                                 COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat12] NOTE: LBA 0 does not look like a valid FAT12 BPB "
                "(expected on LumaOS's own mbr.asm image — see kmain.c DAY9 "
                "header comment); printing derived geometry anyway.\n",
                COLOR_WARNING);
        }

        printf("[fat12] BytesPerSec=%u SecPerClus=%u NumFATs=%u "
               "RootEntCnt=%u FATSz16=%u\n",
               bpb.BPB_BytesPerSec, bpb.BPB_SecPerClus, bpb.BPB_NumFATs,
               bpb.BPB_RootEntCnt, bpb.BPB_FATSz16);
        printf("[fat12] RootDirSectors=%u  FirstRootDirSector=%u  "
               "FirstDataSector=%u  FirstFATSector=%u  TotalSectors=%u\n",
               geom.root_dir_sectors, geom.first_root_dir_sec,
               geom.first_data_sector, geom.first_fat_sector,
               geom.total_sectors);
    }

    /* ------------------------------------------------------------------
     * DAY9-FOLLOWUP: the LBA 0 pass above only proves fat12_parse_bpb()
     * correctly *rejects* a non-FAT sector without crashing — it can't
     * by itself prove the RootDirSectors/FirstDataSector arithmetic is
     * right, since LBA 0 never was a FAT12 volume. build.sh now bakes
     * a genuine `mkfs.fat -F 12` boot sector (tests/fat12-bpb.bin) onto
     * this image at LBA 2500 (see build.sh's DAY9-FOLLOWUP comment for
     * why that LBA is safe/unused), so this second pass reads *that*
     * sector and runs it through the same fat12_parse_bpb() call,
     * giving a visible, on-screen positive-case result to go with the
     * negative-case one above.
     * ------------------------------------------------------------------ */
    console_write_color("[fat12] Reading LBA 2500 (embedded real FAT12 BPB fixture)...\n",
                         COLOR_INFO);
    {
        static uint8_t fat12_fixture_buf[512];
        int fixture_read_ok = (disk->read(disk, 2500, 1, fat12_fixture_buf) == 0);

        if (!fixture_read_ok) {
            console_write_color("[fat12] SKIP: disk->read(2500, 1, ...) failed\n",
                                 COLOR_ERROR);
        } else {
            static bpb_12_t bpb2;
            static fat12_geometry_t geom2;
            int bpb2_ok = (fat12_parse_bpb(fat12_fixture_buf, &bpb2, &geom2) == 0);

            if (bpb2_ok) {
                console_write_color(
                    "[fat12] OK: LBA 2500 is a valid FAT12 BPB (real mkfs.fat image)\n",
                    COLOR_SUCCESS);
            } else {
                console_write_color(
                    "[fat12] FAIL: LBA 2500 did not parse as a valid FAT12 BPB "
                    "(tests/fat12-bpb.bin missing from image? see build.sh)\n",
                    COLOR_ERROR);
            }

            printf("[fat12] BytesPerSec=%u SecPerClus=%u NumFATs=%u "
                   "RootEntCnt=%u FATSz16=%u\n",
                   bpb2.BPB_BytesPerSec, bpb2.BPB_SecPerClus, bpb2.BPB_NumFATs,
                   bpb2.BPB_RootEntCnt, bpb2.BPB_FATSz16);
            printf("[fat12] RootDirSectors=%u  FirstRootDirSector=%u  "
                   "FirstDataSector=%u  FirstFATSector=%u  TotalSectors=%u\n",
                   geom2.root_dir_sectors, geom2.first_root_dir_sec,
                   geom2.first_data_sector, geom2.first_fat_sector,
                   geom2.total_sectors);
        }
    }

    /* ------------------------------------------------------------------
     * DAY10: "实现 fat12_mount(block_dev)：读取引导扇区，检查
     *  BS_FileSysType 是否为 FAT12。挂载成功返回 0，失败返回 -1，在
     *  终端打印 'FAT12 mounted'。"
     *
     *   fat12_mount() (kernel/fs/fat/fat12.{h,c}) does its own
     *   dev->read(...) internally (unlike the Day 9 tests above, which
     *   read the sector here in kmain.c and handed the buffer to
     *   fat12_parse_bpb() directly) — so this call passes `disk` and
     *   an LBA, not a buffer. Mounted against LBA 2500, the same real
     *   FAT12 fixture (tests/fat12-bpb.bin) the Day 9-followup section
     *   above already parsed successfully, so this is expected to
     *   return 0 and print "FAT12 mounted". A second call against
     *   LBA 0 (LumaOS's own non-FAT mbr.asm sector) demonstrates the
     *   -1/failure path the acceptance criteria also asks for.
     * ------------------------------------------------------------------ */
    console_write_color("[fat12] Mounting FAT12 volume at LBA 2500...\n", COLOR_INFO);
    {
        static fat12_fs_t fs;
        if (fat12_mount(disk, 2500, &fs) == 0) {
            console_write_color("FAT12 mounted\n", COLOR_SUCCESS);
            printf("[fat12] mounted: %u total sectors, %u bytes/cluster, "
                   "data region starts at sector %u\n",
                   fs.geom.total_sectors, fs.geom.bytes_per_cluster,
                   fs.geom.first_data_sector);
        } else {
            console_write_color("[fat12] FAIL: fat12_mount(disk, 2500, ...) "
                                 "returned -1 (unexpected — LBA 2500 should "
                                 "hold a valid FAT12 BPB)\n", COLOR_ERROR);
        }
    }

    console_write_color("[fat12] Mounting FAT12 volume at LBA 0 (expected to fail)...\n",
                         COLOR_INFO);
    {
        static fat12_fs_t fs0;
        if (fat12_mount(disk, 0, &fs0) == 0) {
            console_write_color("FAT12 mounted\n", COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat12] NOTE: mount failed as expected (LBA 0 is LumaOS's "
                "own mbr.asm sector, not a FAT12 volume — see kmain.c DAY9 "
                "comment)\n", COLOR_WARNING);
        }
    }

    /* ------------------------------------------------------------------
     * DAY11: "实现 FAT12 簇链遍历函数 fat12_next_cluster(cluster)（处理
     *  1.5 字节对齐，注意奇偶簇）。给定根目录的起始簇（通常为 0），能
     *  遍历打印出前 10 个簇号。"
     *
     *   Mounts the DAY11 fixture (tests/fat12-with-file.img, embedded
     *   by build.sh at LBA 5000 — see this file's DAY11 header comment
     *   for how it was built and independently cross-validated), then
     *   exercises fat12_next_cluster() two ways: first on the two
     *   truly-reserved cluster numbers (0 and 1), to show the walker
     *   handles them without crashing/looping; then on the real
     *   BIGFILE.BIN chain starting at cluster 2, printing 10
     *   consecutive fat12_next_cluster() return values. Compare the
     *   printed values against the "CROSS-VALIDATION" list in this
     *   file's DAY11 header comment.
     * ------------------------------------------------------------------ */
    console_write_color("[fat12] Mounting DAY11 fixture at LBA 5000 "
                         "(FAT12 volume w/ real files)...\n", COLOR_INFO);
    {
        static fat12_fs_t fs11;
        if (fat12_mount(disk, 5000, &fs11) != 0) {
            console_write_color(
                "[fat12] FAIL: fat12_mount(disk, 5000, ...) returned -1 "
                "(tests/fat12-with-file.img missing from image? see "
                "build.sh)\n", COLOR_ERROR);
        } else {
            console_write_color("FAT12 mounted\n", COLOR_SUCCESS);

            /* ---- (b) sanity pass: clusters 0 and 1 are reserved,
             * not real chain links. Confirm fat12_next_cluster()
             * reports them (as raw FAT[0]/FAT[1] values) rather than
             * crashing — see this file's DAY11 header comment for why
             * this stands in for the roadmap's literal "簇号 0".
             *
             * NOTE: clusters 0/1 are reserved BY DEFINITION (the spec
             * never lets any file claim them), not because their
             * stored value happens to fall in the FAT12_CLUSTER_EOC_
             * MIN..MAX range — cluster 0's entry conventionally holds
             * the media descriptor byte reflected into its low 8 bits
             * (0xFF0 for this fixture's 0xF0 media byte), which is
             * BELOW 0xFF8 despite being reserved. So this check must
             * NOT test "is the value >= EOC_MIN" (an earlier version
             * of this test did, and wrongly printed FAT[0]'s
             * legitimate 0xFF0 as "UNEXPECTED"); it just reports the
             * raw values for a human to read against the fixture's
             * known-good numbers documented above. ---- */
            console_write_color(
                "[fat12] Reserved-cluster sanity check (clusters 0 and 1):\n",
                COLOR_INFO);
            for (uint16_t c = 0; c <= 1; c++) {
                int32_t v = fat12_next_cluster(&fs11, c);
                if (v < 0) {
                    printf("[fat12]   FAT[%u] -> read error (%d)\n", c, v);
                } else {
                    printf("[fat12]   FAT[%u] = 0x%03X (reserved cluster -- "
                           "never a real chain link, walker did not crash)\n",
                           c, (unsigned)v);
                }
            }

            /* ---- (a) real chain walk: BIGFILE.BIN starts at cluster
             * 2 (fixture's own on-disk directory entry — see this
             * file's DAY11 header comment's CROSS-VALIDATION section
             * for the independently-computed expected sequence).
             * Print up to 10 fat12_next_cluster() outputs, stopping
             * early on error or end-of-chain/bad-cluster, per the
             * roadmap's "遍历打印出前 10 个簇号" (with 20 total
             * clusters in the fixture's chain, all 10 are expected to
             * print without hitting end-of-chain). ---- */
            console_write_color(
                "[fat12] Walking BIGFILE.BIN's cluster chain from cluster 2 "
                "(expect 3,4,5,6,7,8,9,10,11,12):\n", COLOR_INFO);
            uint16_t cluster = 2;
            for (int i = 0; i < 10; i++) {
                int32_t next = fat12_next_cluster(&fs11, cluster);

                if (next < 0) {
                    printf("[fat12]   [%d] fat12_next_cluster(%u) -> "
                           "read error (%d), stopping early\n",
                           i, cluster, next);
                    break;
                }
                if (next == FAT12_CLUSTER_FREE) {
                    printf("[fat12]   [%d] fat12_next_cluster(%u) -> "
                           "0x000 (free cluster?!), stopping early\n",
                           i, cluster);
                    break;
                }
                if (next == FAT12_CLUSTER_BAD) {
                    printf("[fat12]   [%d] fat12_next_cluster(%u) -> "
                           "0xFF7 (bad cluster), stopping early\n",
                           i, cluster);
                    break;
                }
                if (next >= FAT12_CLUSTER_EOC_MIN) {
                    printf("[fat12]   [%d] fat12_next_cluster(%u) -> "
                           "0x%03X (end of chain), stopping early\n",
                           i, cluster, (unsigned)next);
                    break;
                }

                printf("[fat12]   [%d] fat12_next_cluster(%u) -> %u\n",
                       i, cluster, (unsigned)next);
                cluster = (uint16_t)next;
            }
        }

        /* --------------------------------------------------------------
         * DAY12: "实现 fat12_readdir(root_node)：读取根目录区（Root
         *  Directory Region），遍历每 32 字节条目。提取文件名（去掉
         *  空格）、属性、起始簇、文件大小。在终端列出根目录下所有文件
         *  名（至少能看到 KERNEL BIN 和 LOADER BIN）。"
         *
         *   Lists the root directory of the SAME fs11 handle Day 11's
         *   cluster-chain walk above just mounted (tests/fat12-with-
         *   file.img at LBA 5000, still in scope here — no re-mount
         *   needed). See fat12.h's DAY12 header comment for why this
         *   fixture's BIGFILE.BIN/SMALL.BIN stand in for the roadmap
         *   text's literal "KERNEL BIN"/"LOADER BIN" example names:
         *   nothing in this project ever stores kernel.bin/loader.bin
         *   as named entries inside a FAT12 volume (build.sh dd's them
         *   onto raw disk-absolute sectors instead — see kmain.c's
         *   own DAY1/DAY6 header comments), so listing the real,
         *   already-cross-validated fixture files here is the closer
         *   reading of the acceptance criteria's intent ("show the
         *   readdir walk actually finds real files"), not a literal
         *   filename match.
         * -------------------------------------------------------------- */
        console_write_color(
            "[fat12] Listing root directory (fat12_readdir)...\n", COLOR_INFO);
        {
            static fat12_dirlist_entry_t dirlist[FAT12_DIRLIST_MAX_ENTRIES];
            uint32_t dirlist_count = 0;
            int rd_ret = fat12_readdir(&fs11, dirlist,
                                        FAT12_DIRLIST_MAX_ENTRIES, &dirlist_count);

            if (rd_ret != 0) {
                printf("[fat12] FAIL: fat12_readdir() returned %d after "
                       "decoding %u entries\n", rd_ret, dirlist_count);
            } else {
                printf("[fat12] OK: %u entries in root directory "
                       "(expect BIGFILE.BIN and SMALL.BIN):\n",
                       dirlist_count);

                int saw_bigfile = 0, saw_small = 0;
                for (uint32_t e = 0; e < dirlist_count; e++) {
                    fat12_dirlist_entry_t *ent = &dirlist[e];
                    const char *kind =
                        (ent->attr & FAT_ATTR_DIRECTORY) ? "<DIR>" : "     ";

                    /* NOTE: this project's printf() (kernel/lib/printf.c)
                     * only supports right-justified %s width padding
                     * (pad-then-string), not a '-' left-justify flag —
                     * so name is printed unpadded here rather than via
                     * a "%-13s"-style spec that this printf would
                     * misparse (it has no left-justify support at
                     * all; see printf.c's vprintf_internal()). */
                    printf("[fat12]   %s %s attr=0x%02X cluster=%u size=%u\n",
                           ent->name, kind, ent->attr,
                           (unsigned)ent->start_cluster, ent->size);

                    if (strcmp(ent->name, "BIGFILE.BIN") == 0)
                        saw_bigfile = 1;
                    if (strcmp(ent->name, "SMALL.BIN") == 0)
                        saw_small = 1;
                }

                if (saw_bigfile && saw_small) {
                    console_write_color(
                        "[fat12] OK: found both BIGFILE.BIN and SMALL.BIN\n",
                        COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[fat12] NOTE: did not find both expected fixture "
                        "files (tests/fat12-with-file.img contents changed?)\n",
                        COLOR_WARNING);
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY15: "重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录
     *  项结构），创建 fat_common.c。一个 fat_mount() 根据 BPB 的
     *  BytesPerSector 和 TotalSectors 自动判断类型。"
     *
     *   Exercises the NEW kernel/fs/fat/fat_common.c entry point,
     *   fat_mount(), against the exact three LBAs this file's Day 9-12
     *   sections above already read directly — not because fat_mount()
     *   needs a new fixture, but so this test can prove the Day 15
     *   refactor changed NOTHING observable about those known-good
     *   results while adding the new "which FAT variant is this"
     *   classification on top:
     *
     *     - LBA 0:    LumaOS's own boot/mbr.asm — NOT a FAT BPB at all
     *                 (see this file's DAY9 header comment). Expected:
     *                 FAT_TYPE_UNKNOWN, the negative case.
     *     - LBA 2500: tests/fat12-bpb.bin, a real `mkfs.fat -F 12` boot
     *                 sector (DAY9-FOLLOWUP fixture). Expected: FAT12.
     *     - LBA 5000: tests/fat12-with-file.img, the DAY11 fixture
     *                 (also a real `mkfs.fat -F 12` volume, just with
     *                 files on it). Expected: FAT12.
     *
     *   Both positive cases are genuine FAT12 volumes (this project has
     *   no FAT16/32 fixture yet — that's Day 16/17's job), so this test
     *   cannot show fat_classify_type() picking FAT16 or FAT32 on real
     *   media; what it CAN and does show is that the shared geometry
     *   math (now living in fat_common.c instead of duplicated inline
     *   in fat12.c) still produces a plausible, non-crashing
     *   TotalClusters figure and correctly classifies it under 4085 ->
     *   FAT12 for both real fixtures, while still correctly rejecting
     *   LBA 0 the same way fat12_parse_bpb()'s own sanity check always
     *   has. fat_classify_type()'s FAT16/32 boundaries themselves are
     *   pure arithmetic or synthetic BPBs, independently exercised by
     *   this project's host-side test harness (tests/
     *   day15_fat_mount_host_test.c) — see that file's own header
     *   comment for why a boot-time QEMU test can't manufacture a
     *   65525+-cluster volume just to prove a comparison operator.
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] fat_mount() auto-detection (DAY15 fat_common.c)...\n",
        COLOR_INFO);
    {
        struct { uint32_t lba; fat_type_t expect; const char *label; } cases[] = {
            { 0,    FAT_TYPE_UNKNOWN, "LBA 0 (LumaOS mbr.asm, not FAT)" },
            { 2500, FAT_TYPE_FAT12,   "LBA 2500 (fat12-bpb.bin fixture)" },
            { 5000, FAT_TYPE_FAT12,   "LBA 5000 (fat12-with-file.img fixture)" },
        };
        int all_ok = 1;

        for (int i = 0; i < 3; i++) {
            static fat_bpb_common_t bpb15;
            static fat_geometry_t   geom15;
            fat_type_t got = fat_mount(disk, cases[i].lba, &bpb15, &geom15);

            const char *got_name =
                (got == FAT_TYPE_FAT12) ? "FAT12" :
                (got == FAT_TYPE_FAT16) ? "FAT16" :
                (got == FAT_TYPE_FAT32) ? "FAT32" : "UNKNOWN";

            printf("[fat]   %s -> %s (BytesPerSec=%u SecPerClus=%u "
                   "TotalSectors=%u TotalClusters=%u)\n",
                   cases[i].label, got_name,
                   bpb15.BPB_BytesPerSec, bpb15.BPB_SecPerClus,
                   geom15.total_sectors, geom15.total_clusters);

            if (got != cases[i].expect) {
                printf("[fat]   FAIL: expected type %d, got %d\n",
                       (int)cases[i].expect, (int)got);
                all_ok = 0;
            }
        }

        if (all_ok) {
            console_write_color(
                "[fat] OK: fat_mount() classified all three LBAs correctly\n",
                COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat] FAIL: fat_mount() misclassified at least one LBA "
                "above\n", COLOR_ERROR);
        }
    }

    /* ------------------------------------------------------------------
     * DAY13: "将 FAT12 挂接到 VFS：注册 fat12_filesystem_ops。实现
     *  vfs_mount("IDE0")。调用 vfs_mount 后，vfs_root 指针指向 FAT
     *  根目录。"
     *
     *   Unlike Day 9-12 above (which call fat12_* functions directly
     *   and hold their own `fat12_fs_t` locals), this section goes
     *   through the NEW generic layer exclusively: vfs_mount()
     *   (kernel/fs/vfs/vfs.c) internally mounts FAT12 via
     *   fat12_vfs_mount() (kernel/fs/fat/fat12_vfs.c) and points the
     *   global vfs_root at the result — nothing here calls
     *   fat12_mount()/fat12_readdir() by name, proving the VFS
     *   indirection actually works end-to-end, the same way the
     *   DAY6 block_device_t check above proves I/O went through
     *   `disk->read()` instead of ide_read_sectors(). The device
     *   string is "IDE0", not plain "IDE" — numbered to match
     *   block.c's ide0_block_device() accessor, since a bare "IDE"
     *   couldn't distinguish a future second physical drive (see
     *   vfs.c's header comment for the full rationale). See vfs.c's
     *   header comment for exactly which physical LBA "IDE0"
     *   resolves to on this project's stock image today.
     * ------------------------------------------------------------------ */
    console_write_color("[vfs] vfs_mount(\"IDE0\")...\n", COLOR_INFO);
    if (vfs_mount("IDE0") != 0) {
        console_write_color(
            "[vfs] FAIL: vfs_mount(\"IDE0\") returned -1\n", COLOR_ERROR);
    } else if (!vfs_root) {
        console_write_color(
            "[vfs] FAIL: vfs_mount() returned 0 but vfs_root is still NULL\n",
            COLOR_ERROR);
    } else {
        console_write_color(
            "[vfs] OK: vfs_root now points at the mounted FAT12 root\n",
            COLOR_SUCCESS);
        /* NOTE: vfs_root->name itself is stored WITHOUT a trailing
         * "/" ("IDE0", not "IDE0/") — appending "/" here is purely a
         * DISPLAY convention matching how 计划.md's namespace diagram
         * writes every directory (IDE/, System/, Kernel/, ...), not
         * something baked into the stored name. Keeping the stored
         * name slash-free avoids double-slash bugs once Day 18 starts
         * concatenating path segments (e.g. "IDE0" + "/" + "System"),
         * so the "/" suffix is computed fresh here from `type` instead. */
        printf("[vfs] vfs_root: name=\"%s%s\" type=%s fs=\"%s\"\n",
               vfs_root->name,
               (vfs_root->type == VFS_TYPE_DIR) ? "/" : "",
               (vfs_root->type == VFS_TYPE_DIR) ? "DIR" : "FILE",
               vfs_root->fs->name);

        /* Walk vfs_root through fs->readdir()/fat12_filesystem_ops —
         * NOT fat12_readdir() directly — to prove the readdir call is
         * actually going through the vfs_node_t/filesystem_t function
         * pointers this file wires up, the same generic path Day 14's
         * cmd_ls is meant to use next. */
        console_write_color(
            "[vfs] Listing vfs_root via vfs_root->fs->readdir()...\n",
            COLOR_INFO);
        {
            vfs_node_t child;
            uint32_t idx = 0;
            int any = 0;
            while (vfs_root->fs->readdir(vfs_root, idx, &child) == 0) {
                /* Same display-only "/" suffix rule as vfs_root above —
                 * none of today's entries are directories (the DAY11
                 * fixture volume has no subdirectories), but this keeps
                 * the rule uniform for whenever one does show up. */
                const char *slash = (child.type == VFS_TYPE_DIR) ? "/" : "";
                printf("[vfs]   %s%s size=%u\n", child.name, slash, child.size);
                any = 1;
                idx++;
            }
            if (!any) {
                console_write_color(
                    "[vfs] NOTE: readdir returned no entries (unexpected — "
                    "see build.sh's tests/fat12-with-file.img embedding)\n",
                    COLOR_WARNING);
            } else {
                console_write_color(
                    "[vfs] OK: readdir via vfs_root->fs->readdir() worked\n",
                    COLOR_SUCCESS);
            }
        }
    }

    /* ------------------------------------------------------------------
     * !! DAY2 临时改动 (禁用位置 2/2) — 月末 (Day 29) 前必须保持注释 !!
     * 与文件顶部说明及 [6/7] 步骤处的禁用位置 1/2 是同一组改动，必须
     * 同时恢复，不能只改一处（只改一处会导致未定义行为或黑屏）。
     * 这里正常应该调用 gui_init() 接管屏幕并进入 GUI 主循环：
     *
     *   gui_init();
     *   for (;;) {
     *       gui_refresh();
     *       asm volatile("pause");
     *   }
     *
     * 第一个月 (Day 1~28) 保持注释，只用文本控制台 + hlt 循环。
     * ------------------------------------------------------------------ */
    console_write_color("\n[boot] Text-mode debug console ready.\n", COLOR_INFO);

    /* DAY14: hand off to the text-mode shell loop instead of an idle
     * "for(;;) hlt;" — see this file's DAY14 header comment (above
     * text_shell_run()'s definition) for the full rationale. This
     * call never returns; text_shell_run() itself loops forever. */
    text_shell_run();
}