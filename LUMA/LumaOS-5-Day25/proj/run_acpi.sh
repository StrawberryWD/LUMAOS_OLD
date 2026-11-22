#!/usr/bin/env bash
# run_acpi.sh — LumaOS 4.1  QEMU with ACPI + isa-debug-exit
#
# STAGE1: Added -vga std for BGA support.
#   isa-debug-exit on port 0x501 lets the shutdown command exit QEMU.
#   -no-reboot prevents the VM from restarting on triple-fault during
#   development so you can see the crash state.

set -euo pipefail

IMG="lumaos-4.1.img"
if [ ! -f "$IMG" ]; then
    echo "Error: $IMG not found — run ./build.sh first"
    exit 1
fi

echo "=== LumaOS 4.1 — QEMU ACPI mode (BGA 800x450) ==="
echo "  Image       : $IMG"
echo "  Memory      : 256 MB"
echo "  VGA         : std (Bochs VBE BGA)"
echo "  ACPI port   : 0x0604 (PM1a S5)"
echo "  Debug exit  : 0x0501 (isa-debug-exit)"
echo "  Reboot port : 0x0CF9"
echo ""
echo "  In-kernel shutdown : type 'shutdown'"
echo "  In-kernel reboot   : type 'reboot'"
echo "  Force quit QEMU    : Ctrl+A then X (serial console)"
echo ""

qemu-system-i386 \
    -machine type=pc \
    -m 256 \
    -vga std \
    -drive format=raw,file="$IMG",if=floppy \
    -device intel-hda \
    -device hda-duplex \
    -device isa-debug-exit,iobase=0x501,iosize=0x1 \
    -no-reboot \
    -serial stdio
