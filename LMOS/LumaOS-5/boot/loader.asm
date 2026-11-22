; boot/loader.asm - LumaOS 5.0 Second-Stage Loader
; Loaded at 0x7E00 by MBR (4 sectors = 2048 bytes)
;
; FIX BOOT-1  : cl=7  (was cl=6 → CHS mismatch, loaded wrong sector) [superseded, see DAY1-FIX]
; FIX ENTRY-1 : al=128 sectors = 64 KB
;               kernel.bin > 32 KB; original al=64 truncated .rodata/.data
;
; DAY1 (LumaOS 5.0 roadmap - Week 1, Day 1):
;   Read count raised from 128 to 255 sectors (127.5 KB instead of
;   64 KB), paired with build.sh's MAX_LOAD bump to 131072, so the
;   upcoming filesystem/driver code (~40-50 KB) still fits.
;
; DAY1-FIX (switching run.sh from if=floppy to if=ide broke boot):
;   CHS (ch/cl/dh) addressing is geometry-dependent — a floppy's
;   CHS geometry (2 heads, 18 sectors/track) is NOT the same as the
;   CHS translation SeaBIOS applies to the same image once it is
;   attached as an IDE disk. The old fixed ch=0/cl=7/dh=0 pointed at
;   LBA 6 under floppy geometry but at the wrong physical sector
;   under IDE geometry, producing "DISK ERROR".
;
;   Fix: switch to INT 13h/AH=42h (BIOS Extended Read, "Disk Address
;   Packet"), which addresses sectors purely by LBA and ignores
;   CHS geometry entirely. This works identically for floppy, IDE,
;   and (later) CD-ROM emulation — no more geometry coupling. This
;   also happens to be the same mechanism the roadmap has scheduled
;   for Day 59 (CD boot), so this is a small pull-forward.
;
;   We first probe AH=41h (INT 13h Extensions Installation Check);
;   if unsupported we halt with a clear error rather than silently
;   falling back to a CHS path that would just fail again.
;
; DAY1-FIX2 (single 255-sector AH=42h call still failed):
;   Confirmed by booting in QEMU with debug instrumentation: this
;   SeaBIOS (1.16.3) caps a single INT 13h/AH=42h transfer at 128
;   sectors (64 KB) regardless of the count field in the DAP — a
;   127/128-sector request succeeds, a 200/255-sector request fails
;   with CF=1. This is a common real-world BIOS limitation, not
;   specific to this project. Fix: split the 256-sector (128 KB)
;   read into two back-to-back 128-sector calls (dap1, dap2),
;   loading into 0x1000:0000 and 0x2000:0000 respectively so the
;   two halves land contiguously in physical memory. Verified in
;   QEMU with a 130 KB test payload: markers at the start of the
;   image and past the 64 KB boundary both landed correctly.

BITS 16
ORG  0x7E00

start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  sp, 0x7C00
    mov  [boot_drive], dl
    mov  si, banner
    call print16

    ; ---- Enable A20 (3-method cascade) ----
    call enable_a20_bios
    call check_a20
    jnz  a20_ok
    call enable_a20_kbd
    call check_a20
    jnz  a20_ok
    call enable_a20_fast
a20_ok:
    mov  si, a20_msg
    call print16

    ; ---- Check INT 13h Extensions are present (AH=41h) ----
    mov  ah, 0x41
    mov  bx, 0x55AA
    mov  dl, [boot_drive]
    int  0x13
    jc   ext_missing
    cmp  bx, 0xAA55
    jne  ext_missing

    ; ---- Load kernel via LBA extended read (AH=42h), two calls ----
    ; build.sh: dd seek=6 → kernel starts at LBA 6
    ; DAY1-FIX: LBA read replaces the old CHS read (ch/cl/dh), which
    ;           broke when run.sh switched the image from if=floppy
    ;           to if=ide (CHS geometry differs between the two).
    ; DAY1-FIX2: SeaBIOS's AH=42h path caps a single transfer at 128
    ;           sectors (64 KB) here — a request of 200+ sectors in
    ;           one DAP call fails with CF=1 (confirmed by booting in
    ;           QEMU: 127/128 sectors succeed, 200/255 do not). So the
    ;           DAY1 target of 255 sectors (127.5 KB) is split into
    ;           two back-to-back 128-sector calls (dap1, dap2) instead
    ;           of one big one — same total capacity, stays inside the
    ;           per-call limit.
    mov  si, dap1
    mov  ah, 0x42
    mov  dl, [boot_drive]
    int  0x13
    jc   disk_err

    mov  si, dap2
    mov  ah, 0x42
    mov  dl, [boot_drive]
    int  0x13
    jc   disk_err

    mov  si, kernel_ok_msg
    call print16

    xor  ax, ax
    mov  es, ax
    lgdt [gdt_desc]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:pm_entry

enable_a20_bios:
    push ax
    mov  ax, 0x2401
    int  0x15
    pop  ax
    ret

enable_a20_kbd:
    call kb_wait_in
    mov  al, 0xAD
    out  0x64, al
    call kb_wait_in
    mov  al, 0xD0
    out  0x64, al
    call kb_wait_out
    in   al, 0x60
    push ax
    call kb_wait_in
    mov  al, 0xD1
    out  0x64, al
    call kb_wait_in
    pop  ax
    or   al, 2
    out  0x60, al
    call kb_wait_in
    mov  al, 0xAE
    out  0x64, al
    call kb_wait_in
    ret

enable_a20_fast:
    in   al, 0x92
    test al, 2
    jnz  .done
    or   al, 2
    and  al, 0xFE
    out  0x92, al
.done:
    ret

check_a20:
    push ds
    push es
    push di
    push si
    xor  ax, ax
    mov  es, ax
    mov  di, 0x0500
    mov  ax, 0xFFFF
    mov  ds, ax
    mov  si, 0x0510
    mov  al, [es:di]
    push ax
    mov  al, [ds:si]
    push ax
    mov  byte [es:di], 0x00
    mov  byte [ds:si], 0xFF
    mov  cx, 200
.delay: loop .delay
    mov  al, [es:di]
    cmp  al, [ds:si]
    pushf
    pop  bx
    pop  ax
    mov  [ds:si], al
    pop  ax
    mov  [es:di], al
    push bx
    popf
    pop  si
    pop  di
    pop  es
    pop  ds
    ret

kb_wait_in:
    push ax
.w: in   al, 0x64
    test al, 2
    jnz  .w
    pop  ax
    ret

kb_wait_out:
    push ax
.w: in   al, 0x64
    test al, 1
    jz   .w
    pop  ax
    ret

disk_err:
    mov  si, disk_msg
    call print16
.h: cli
    hlt
    jmp  .h

ext_missing:
    mov  si, ext_msg
    call print16
.h: cli
    hlt
    jmp  .h

print16:
    push ax
    push bx
.lp:
    lodsb
    test al, al
    jz   .done
    mov  ah, 0x0E
    xor  bx, bx
    int  0x10
    jmp  .lp
.done:
    pop  bx
    pop  ax
    ret

boot_drive      db 0
banner          db "LumaOS 5.0 Loader",13,10,0
a20_msg         db "A20 OK",13,10,0
kernel_ok_msg   db "Kernel loaded",13,10,0
disk_msg        db "DISK ERROR",13,10,0
ext_msg         db "NO INT13 EXT",13,10,0

; ---- Disk Address Packets for INT 13h/AH=42h ----
; DAY1-FIX2: split into two 128-sector (64 KB) calls — this SeaBIOS
; caps a single AH=42h transfer at 128 sectors. Together they read
; 256 sectors = 128 KB, starting at LBA 6, into 0x1000:0000 linearly
; (physical 0x10000-0x2FFFF), matching build.sh's MAX_LOAD=131072.
ALIGN 4
dap1:
    db 0x10         ; packet size (16 bytes)
    db 0             ; reserved
    dw 128           ; sectors: first half (64 KB)
    dw 0x0000        ; offset  (0x1000:0000 -> physical 0x10000)
    dw 0x1000        ; segment
    dq 6             ; starting LBA (build.sh: dd seek=6)

ALIGN 4
dap2:
    db 0x10         ; packet size (16 bytes)
    db 0             ; reserved
    dw 128           ; sectors: second half (64 KB)
    dw 0x0000        ; offset  (0x2000:0000 -> physical 0x20000,
    dw 0x2000        ;           i.e. 64 KB after dap1's buffer)
    dq 134           ; starting LBA (6 + 128)

ALIGN 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BITS 32
pm_entry:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x00200000
    jmp  0x08:0x00010000

TIMES (4*512)-($-$$) db 0