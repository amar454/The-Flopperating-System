#!/usr/bin/env bash
if ! command -v xorriso >/dev/null 2>&1; then
    echo "Error: xorriso is not installed. Run: sudo apt install xorriso"
    exit 1
fi

KERNEL=$1
ISO_DIR="iso"

rm -rf $ISO_DIR
mkdir -p $ISO_DIR/boot/grub

cp "$KERNEL" $ISO_DIR/boot/
cp grub.cfg $ISO_DIR/boot/grub/

if grub-file --is-x86-multiboot $ISO_DIR/boot/$(basename "$KERNEL"); then
    echo "Multiboot check: Confirmed"
else
    echo "Error: The kernel is not Multiboot compliant!"
    exit 1
fi

grub-mkrescue -o $2 $ISO_DIR

rm -rf $ISO_DIR
