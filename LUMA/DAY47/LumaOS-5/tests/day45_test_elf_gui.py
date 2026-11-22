"""
tests/day45_test_elf_gui.py - Day 45 smoke test on the GUI build.

    ./build.sh && python3 tests/day45_test_elf_gui.py [outdir]

Boots the GUI image, runs `elfmap IDE3/data.sw` and `elfmap IDE0/HELLO.SW` in the GUI
terminal, checks g_terminal shows the segment lines / [ok] / OK verdict, and that the guest
physical memory (monitor `xp`) holds data.sw's bytes. Exhaustive checks: day45_test_elf_map.py.
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d45_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d45_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
data = open("tests/data.sw", "rb").read()
m = Machine(IMG, "d45gui", gui=True, elf="build/kernel.elf")
fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)
try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI booted", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.line("elfmap IDE3/data.sw", 3.5)
    scr = m.gui_screen()
    tail = scr.split("elfmap IDE3/data.sw")[-1]
    check("size line", "IDE3/data.sw: %d bytes" % len(data) in tail, scr)
    check("4 segments listed, all [ok]", tail.count("[ok]") == 4, scr)
    check(".bss line is zero", "bss 0804b084: 00 00 00 00 00 00 00 00" in tail, scr)
    check("OK verdict", "elfmap: OK - image loaded, entry 0x08049000" in tail, scr)
    check("guest memory at 0x0804b060 == data.sw .data bytes", m._read_bytes(0x0804b060, 0x24) == data[0x2060:0x2060 + 0x24])
    check("guest .bss zero", m._read_bytes(0x0804b084, 0x201c) == b"\x00" * 0x201c)
    m.png(f"{out}/1_elfmap.png")
    m.line("elfmap IDE0/HELLO.SW", 3.0)
    tail = m.gui_screen().split("elfmap IDE0/HELLO.SW")[-1]
    check("FAT: OK verdict", "elfmap: OK" in tail, tail)
    m.line("elfmap IDE0/HELLO.TXT", 2.0)
    tail = m.gui_screen().split("elfmap IDE0/HELLO.TXT")[-1]
    check("non-ELF: FAIL line, untouched memory notice", "elfmap: FAIL" in tail and "memory untouched" in tail, tail)
    m.line("ls", 1.0)
    check("terminal still alive", "hello.sw" in m.gui_screen().split("ls")[-1])
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
