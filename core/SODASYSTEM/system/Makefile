# Makefile - SODA 32-bit
CC      := gcc
ASM     := nasm
LD      := ld
OBJCOPY := objcopy
PYTHON  := python3

OUT     := out

CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-pie -nostdlib -nostdinc \
           -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fcf-protection=none -fno-stack-clash-protection -mno-red-zone \
           -Wall -Wextra -O2

ASMFLAGS := -f elf32
LDFLAGS  := -m elf_i386 -T linker.ld

# ---- 构建产物列表（clean 只删这些） ----
C_OBJS   := kernel.o lib.o kprintf.o idt.o keyboard.o pit.o shell.o ata.o fat12.o
ASM_OBJS := entry.o isr.o
OBJS     := $(addprefix $(OUT)/,$(ASM_OBJS) $(C_OBJS))

ARTIFACTS := $(OBJS) \
             $(OUT)/boot.bin \
             $(OUT)/kernel.elf \
             $(OUT)/kernel.bin \
             $(OUT)/soda.img

.PHONY: all clean distclean run debug

all: $(OUT)/soda.img

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/boot.bin: boot.asm | $(OUT)
	$(ASM) -f bin $< -o $@

$(OUT)/entry.o: entry.asm | $(OUT)
	$(ASM) $(ASMFLAGS) $< -o $@

$(OUT)/isr.o: isr.asm | $(OUT)
	$(ASM) $(ASMFLAGS) $< -o $@

$(OUT)/%.o: %.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(OUT)/kernel.bin: $(OUT)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/soda.img: $(OUT)/boot.bin $(OUT)/kernel.bin pad_image.py
	$(PYTHON) pad_image.py $< $(word 2,$^) $@

# ---- 清理构建产物，保留 out/ 里的数据文件（如 disk.img） ----
clean:
	@echo "Cleaning build artifacts (keeping data files in $(OUT)/)..."
	@rm -f $(ARTIFACTS)
	@echo "Done. $(OUT)/disk.img and other data files are preserved."

# ---- 彻底清理：连 out/ 目录一起删（会丢失 disk.img！） ----
distclean:
	@echo "WARNING: removing entire $(OUT)/ directory (including disk.img)..."
	@rm -rf $(OUT)

run: $(OUT)/soda.img
	qemu-system-i386 -no-reboot -no-shutdown \
	    -boot order=a \
	    -fda $(OUT)/soda.img \
	    -hda $(OUT)/disk.img

debug: $(OUT)/soda.img
	qemu-system-i386 -s -S -no-reboot -no-shutdown \
	    -boot order=a \
	    -fda $(OUT)/soda.img \
	    -hda $(OUT)/disk.img
