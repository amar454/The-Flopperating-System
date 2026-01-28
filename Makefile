CP := cp
RM := rm -rf
MKDIR := mkdir -pv
FIND := find

BIN = build/_kernel_bin
CFG = grub.cfg
ISO_PATH := iso
BOOT_PATH := $(ISO_PATH)/boot
GRUB_PATH := $(BOOT_PATH)/grub
BUILD_PATH := build

# Compiler settings
CC = gcc
LD = ld
NASM = nasm
CFLAGS = -m32 -ffreestanding -fno-stack-protector -std=gnu2x -g
CSTOPONFIRSTERROR_FLAG = -Wfatal-errors
CWARNINGS = -Werror \
            -Wall \
            -Wextra \
            -Wno-unused-parameter \
            -Wno-unused-variable \
            -Wno-unused-function \
            -Wno-unused-but-set-variable \
            -Wno-missing-field-initializers \
            -Wno-sign-compare \
	    -Wno-array-bounds \
            -Wno-format-truncation \
            -Wno-format-overflow \
            -Wno-format-extra-args \
            -Wno-format-zero-length \
            -Wno-maybe-uninitialized \
            -Wno-implicit-fallthrough \
			-Wno-pointer-to-int-cast
INTERRUPT_FLAGS = $(CFLAGS) -mgeneral-regs-only
LD_FLAGS = -m elf_i386 -T kernel/linker.ld

# Source files
SCHED_SRC = task/sched.c task/tss.c task/process.c task/ipc/pipe.c task/ipc/signal.c
MEM_SRC = mem/vmm.c mem/pmm.c mem/paging.c mem/utils.c mem/gdt.c mem/alloc.c mem/early.c
DRIVER_SRC = drivers/vga/vgahandler.c drivers/keyboard/keyboard.c drivers/time/floptime.c \
             drivers/io/io.c drivers/vga/framebuffer.c drivers/acpi/acpi.c drivers/mouse/ps2ms.c drivers/ata/ata.c
FS_SRC = fs/tmpflopfs/tmpflopfs.c fs/vfs/vfs.c fs/procfs/procfs.c
LIB_SRC = lib/str.c lib/flopmath.c lib/logging.c
APP_SRC = apps/echo.c 
OTHER_SRC = kernel/kernel.c multiboot/multiboot.c sys/syscall.c init/init.c
ASM_SRC = kernel/entry.asm task/usermode_entry.asm task/ctx.asm interrupts/interrupts_asm.asm sys/syscall_asm.asm
FLANTERM_SRC = flanterm/src/flanterm.c flanterm/src/flanterm_backends/fb.c

C_SRC = $(SCHED_SRC) $(MEM_SRC) $(DRIVER_SRC) $(FS_SRC) $(LIB_SRC) $(APP_SRC) $(OTHER_SRC) $(FLANTERM_SRC)
OBJ_SRC = $(addprefix $(BUILD_PATH)/, $(ASM_SRC:.asm=.o) $(C_SRC:.c=.o) interrupts.o)

.PHONY: clean cleanobj all kernel entry interrupts linker iso qemu qemu-monitor qemu-log

# Cleanup targets
clean:
	$(RM) $(BIN) $(ISO_PATH) $(BUILD_PATH)

cleanobj:
	$(RM) $(BUILD_PATH)

# Main build targets
all: $(BUILD_PATH) entry kernel interrupts flanterm linker iso
	@echo "Build completed successfully."

$(BUILD_PATH):
	$(MKDIR) $(BUILD_PATH)

entry: kernel/entry.asm | $(BUILD_PATH)
	@mkdir -p $(BUILD_PATH)/kernel
	$(NASM) -f elf32 kernel/entry.asm -o $(BUILD_PATH)/kernel/entry.o

kernel:  sched mem drivers fs lib apps other asm  | $(BUILD_PATH)

# Object file compilation
$(BUILD_PATH)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CWARNINGS) -c $< -o $@

# Component targets
sched:   $(addprefix $(BUILD_PATH)/, $(SCHED_SRC:.c=.o))
mem:     $(addprefix $(BUILD_PATH)/, $(MEM_SRC:.c=.o))
drivers: $(addprefix $(BUILD_PATH)/, $(DRIVER_SRC:.c=.o))
fs:      $(addprefix $(BUILD_PATH)/, $(FS_SRC:.c=.o))
lib:     $(addprefix $(BUILD_PATH)/, $(LIB_SRC:.c=.o))
apps:    $(addprefix $(BUILD_PATH)/, $(APP_SRC:.c=.o))
other:   $(addprefix $(BUILD_PATH)/, $(OTHER_SRC:.c=.o))

asm: $(ASM_SRC)
	@for file in $(ASM_SRC); do \
		out=$(BUILD_PATH)/$${file%.asm}.o; \
		mkdir -p $$(dirname $$out); \
		$(NASM) -f elf32 $$file -o $$out; \
	done

interrupts: interrupts/interrupts.c  | $(BUILD_PATH)
	@mkdir -p $(BUILD_PATH)/interrupts
	$(NASM) -f elf32 interrupts/interrupts_asm.asm -o $(BUILD_PATH)/interrupts/interrupts_asm.o
	$(CC) $(INTERRUPT_FLAGS) $(CWARNINGS) -c interrupts/interrupts.c -o $(BUILD_PATH)/interrupts.o

flanterm: $(addprefix $(BUILD_PATH)/, $(FLANTERM_SRC:.c=.o))
	@mkdir -p $(BUILD_PATH)/flanterm
	$(CC) $(CFLAGS) $(CWARNINGS) -c flanterm/src/flanterm.c -o $(BUILD_PATH)/flanterm.o
	$(CC) $(CFLAGS) $(CWARNINGS) -c flanterm/src/flanterm_backends/fb.c -o $(BUILD_PATH)/fb.o

linker: $(OBJ_SRC)
	$(LD) $(LD_FLAGS) -o $(BIN) $(OBJ_SRC)

iso: $(BIN)
	$(MKDIR) $(GRUB_PATH)
	$(CP) $(BIN) $(BOOT_PATH)
	$(CP) $(CFG) $(GRUB_PATH)
	grub-file --is-x86-multiboot $(BOOT_PATH)/_kernel_bin
	grub-mkrescue -o $(ISO_PATH)/floppaOS-alpha.iso $(ISO_PATH)

fatal: $(C_SRC) interrupts/interrupts.c | $(BUILD_PATH)
	@for file in $(C_SRC); do \
		mkdir -p $(BUILD_PATH)/$(dirname $file); \
		$(CC) $(CFLAGS) $(CSTOPONFIRSTERROR_FLAG) -c $file -o $(BUILD_PATH)/${file%.c}.o; \
	done
	@mkdir -p $(BUILD_PATH)/interrupts
	$(CC) $(INTERRUPT_FLAGS) $(CSTOPONFIRSTERROR_FLAG) -c interrupts/interrupts.c -o $(BUILD_PATH)/interrupts/interrupts.o

# QEMU targets
qemu: clean all
	qemu-system-i386 -cdrom $(ISO_PATH)/floppaOS-alpha.iso --no-shutdown

qemu-monitor: clean all
	qemu-system-i386 -cdrom $(ISO_PATH)/floppaOS-alpha.iso --no-shutdown -monitor stdio

qemu-log-all: clean all
	qemu-system-i386 -d int,cpu_reset,exec -cdrom $(ISO_PATH)/floppaOS-alpha.iso

qemu-log-int: clean all
	qemu-system-i386 -d int -cdrom $(ISO_PATH)/floppaOS-alpha.iso -monitor stdio

qemu-log-int-serial: clean all
	qemu-system-i386 -d int,cpu_reset,exec -cdrom $(ISO_PATH)/floppaOS-alpha.iso -monitor stdio -serial stdio

qemu-log-exec: clean all
	qemu-system-i386 -d int,cpu_reset,exec -cdrom $(ISO_PATH)/floppaOS-alpha.iso -monitor stdio -serial stdio -s -S
