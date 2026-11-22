; kernel/exec/elf_enter.asm - LumaOS 5.0
;
; DAY46 (Week 7, Day 46): "实现 elf_run()。"  验收："hello.sw 可以执行。"
; DAY47 (Week 7, Day 47): "增加用户态栈和退出路径。" 验收："程序执行后返回 Shell。"
;
; int elf_enter(uint32_t entry, const void *api, uint32_t stack_top);   (cdecl)
; void elf_api_exit(int code);            程序主动退出（api->exit），永不返回
; extern uint32_t elf_exit_via;           上一次是怎么回来的：0 = 入口 ret，1 = api->exit
;
;   “真正跳进程序、再从统一出口回来”的汇编。C 语言没法可靠地表达下面这些事，
;   所以单独写在这里；elf_run.c 负责所有检查，这里只负责“切栈、进去、回来”。
;
;   ===== 进入（elf_enter）=====
;     1. [Day 46] 把内核现场存到内核栈：EFLAGS、8 个通用寄存器(pushad)、
;        ds/es/fs/gs。程序是别人写的，不能假设它守规矩，所以全存、回来全恢复。
;     2. [Day 46] 记下此刻的 esp 到 elf_saved_esp——这是“回家的路”，放在
;        内核栈上。回来时无论程序把自己的 esp 弄成什么样，都从这个值恢复。
;     3. [Day 47] 把 esp 切到程序自己的栈 stack_top（用户栈，见 elf.h 的 DAY47
;        注释）。从这一刻起程序的 push/call/局部变量都落在用户栈上，碰不到内核栈。
;     4. [Day 47] 在用户栈上摆出“好像被 call 进来”的样子（i386 System V ABI）：
;            stack_top-16   api 指针   <- 入口函数的 [esp+4]（第一个 cdecl 参数）
;            stack_top-20   elf_exit_stub  <- 入口函数的 [esp]（“返回地址”）
;        进入点处 (esp+4) 按 16 字节对齐，DF=0。然后用 jmp（不是 call）进入——
;        返回地址我们已经手工放好了，指向出口桩。
;
;   ===== 退出（两条路，一个出口 elf_leave）=====
;     路径 A  入口函数执行 ret -> 弹出栈顶的返回地址 = elf_exit_stub -> 落进出口，
;             eax = 程序的返回值。
;     路径 B  程序调用 api->exit(code)（= elf_api_exit）：不管它此刻嵌套了多少层
;             函数、栈上压了多少东西，直接去出口——栈帧不需要逐层返回，出口把 esp
;             一步切回内核栈，等于把整个用户栈的活动现场一次丢掉（C 的 longjmp 同理；
;             Day 52 它会变成 sys_exit）。
;     出口 elf_leave：
;        - 用 ss 把 ds/es 重载成内核平坦数据段（Ring 0 的程序可能把它们改坏，下面
;          读 elf_saved_esp 要用 ds）；
;        - 记录走了哪条路（elf_exit_via）；
;        - esp = elf_saved_esp（回到内核栈），退出码写进保存区的 eax 槽，
;          恢复 gs/fs/es/ds、popad、popfd、pop ebp、ret —— 于是 elf_enter 像一个
;          普通 C 函数一样“返回”给 elf_run()，值就是程序的退出码。
;
;   今天程序仍在 Ring 0（与内核共用地址空间），所以“用户栈”只是一块被划出来的普通内存，
;   并没有硬件保护——程序故意乱写内核内存照样写得进去。Day 54 分页、Day 55 降到
;   Ring 3 之后这道界线才由硬件强制；那时 elf_enter 的“存现场 → 切栈 → 进入 → 统一
;   出口回来”骨架不变，只是 jmp 变成 iret、出口变成系统调用/异常入口。
;
;   不可重入：elf_saved_esp 只有一份。多任务（7.0）之后它会挪进每个进程的结构体。

BITS 32

section .text
global elf_enter
global elf_api_exit
global elf_exit_via

elf_enter:
    push ebp
    mov  ebp, esp
    pushfd                      ; [EFLAGS]
    pushad                      ; edi esi ebp esp ebx edx ecx eax（eax 在最高处）
    push ds
    push es
    push fs
    push gs
    ; 现在 esp 指向 gs；pushad 帧 = esp+16 .. esp+47，其中 eax 槽在 esp+16+28

    mov  eax, [ebp + 8]         ; entry
    mov  edx, [ebp + 12]        ; api
    mov  ecx, [ebp + 16]        ; stack_top（调用者保证 16 字节对齐）
    mov  [elf_saved_esp], esp   ; 回家的路：记在内核栈上
    mov  dword [elf_exit_via], 0

    mov  esp, ecx               ; ===== 切到用户栈 =====
    sub  esp, 12                ; 之后 push 两次，入口处 esp = stack_top-20，(esp+4) 16 对齐
    push edx                    ; [stack_top-16] = api    （第一个参数）
    push dword elf_exit_stub    ; [stack_top-20] = 返回地址（入口函数 ret 到这里）
    cld                         ; DF=0（ABI 要求）
    jmp  eax                    ; ===== 跳进程序 =====

; 路径 A：入口函数 ret 之后落到这里（此刻 esp 在用户栈上，eax = 返回值）
elf_exit_stub:
    xor  ecx, ecx               ; via = 0（正常返回）
    jmp  elf_leave

; 路径 B：void elf_api_exit(int code)  —— 程序主动退出，永不返回
elf_api_exit:
    mov  eax, [esp + 4]         ; code
    mov  ecx, 1                 ; via = 1（api->exit）
    ; fallthrough

; 统一出口：eax = 退出码，ecx = 走的哪条路
elf_leave:
    mov  dx, ss                 ; 程序可能改过 ds/es：先用 ss 改回内核平坦数据段，
    mov  ds, dx                 ; 否则下面读写 [elf_saved_esp] 会用到被改坏的 ds
    mov  es, dx
    mov  [elf_exit_via], ecx
    mov  esp, [elf_saved_esp]   ; ===== 切回内核栈 =====（无论用户栈上发生过什么）
    mov  [esp + 16 + 28], eax   ; 退出码 -> 保存区的 eax 槽
    pop  gs
    pop  fs
    pop  es
    pop  ds
    popad                       ; eax = 退出码；其余寄存器 = 进入前的值
    popfd                       ; 恢复 EFLAGS（包括 IF、DF）
    pop  ebp
    ret

section .bss
align 4
elf_saved_esp: resd 1
elf_exit_via:  resd 1

section .note.GNU-stack noalloc noexec nowrite progbits
