# ApexOS — Makefile
#
# Целевая платформа: x86_64, freestanding, без libc.
#
# ВАЖНО (честно, по опыту сборки в текущем dev-окружении):
#   - `make` (компиляция + линковка kernel.elf) проверено и работает
#     системным набором as/gcc/ld на этой машине.
#   - `make iso` требует grub-mkrescue + xorriso.
#   - `make run`/`make run-serial` требуют qemu-system-x86_64.
#   Ни xorriso, ни grub-mkrescue, ни qemu в текущем dev-контейнере НЕ
#   установлены — эти цели проверялись только на предмет корректности
#   команд, но не запускались здесь. Прогоните их у себя локально.

CC      := gcc
AS      := gcc
LD      := ld

BUILD_DIR := build
ISO_DIR   := iso

CFLAGS := -std=c11 -Wall -Wextra -Werror \
          -ffreestanding -nostdlib -fno-builtin \
          -fno-pic -fno-pie -fno-stack-protector \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel -Iinclude -MMD -MP -g

ASFLAGS := -ffreestanding -fno-pic -fno-pie -Iinclude -MMD -MP

LDFLAGS := -n -T linker.ld -nostdlib -static

C_SOURCES := \
    kernel/main.c \
    kernel/panic.c \
    kernel/syscall.c \
    kernel/lib/string.c \
    kernel/lib/fmt.c \
    kernel/arch/x86_64/serial.c \
    kernel/arch/x86_64/gdt.c \
    kernel/arch/x86_64/idt.c \
    kernel/arch/x86_64/pic.c \
    kernel/arch/x86_64/pit.c \
    kernel/arch/x86_64/keyboard.c \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kheap.c \
    kernel/elf/elf64_loader.c \
    kernel/drivers/fb.c \
    kernel/drivers/console.c \
    kernel/drivers/font8x16_data.c \
    kernel/drivers/ramdisk.c \
    kernel/drivers/rtc.c \
    kernel/drivers/pci.c \
    kernel/fs/fat32.c \
    kernel/shell/shell.c \
    kernel/shell/highlight.c \
    kernel/shell/editor.c \
    kernel/process.c \
    kernel/task.c

ASM_SOURCES := \
    boot/multiboot2_header.S \
    boot/boot.S \
    kernel/arch/x86_64/entry.S \
    kernel/arch/x86_64/gdt_flush.S \
    kernel/arch/x86_64/isr_stubs.S \
    kernel/arch/x86_64/usermode.S \
    kernel/arch/x86_64/context.S

C_OBJECTS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
OBJECTS     := $(ASM_OBJECTS) $(C_OBJECTS)
DEPS        := $(OBJECTS:.o=.d)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
USER_ELF   := $(BUILD_DIR)/user/hello.elf
ISO_IMAGE  := nikios.iso

.PHONY: all clean iso run run-serial run-ata run-sata debug test check-multiboot user-app asc asc-app

all: $(KERNEL_ELF) $(USER_ELF)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJECTS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo "--- kernel.elf built ---"

# Userspace-программа собирается ОТДЕЛЬНО от ядра: своя точка входа,
# свой (гораздо более простой) линкер-скрипт, никаких kernel-специфичных
# флагов (-mcmodel=kernel и т.п. для userspace не нужны и были бы неверны).
$(BUILD_DIR)/user/hello.o: user/hello.S
	@mkdir -p $(dir $@)
	$(AS) -ffreestanding -fno-pic -fno-pie -c $< -o $@

$(USER_ELF): $(BUILD_DIR)/user/hello.o user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -n -T user/user.ld -nostdlib -static -o $@ $(BUILD_DIR)/user/hello.o
	@echo "--- user/hello.elf built ---"

# Компилирует ПРОИЗВОЛЬНУЮ C-программу пользователя на ХОСТЕ (где есть
# настоящий gcc) под ApexOS syscall ABI (см. user/nikios_syscall.h).
# Компилятора C внутри ApexOS нет и не будет в обозримом будущем — это
# честный, стандартный для hobby OS способ получить реальный код на C
# запускаемым в системе. Результат кладётся в build/user/hello.elf —
# тот же путь, что использует `make iso` как boot-модуль — так что
# `make user-app SRC=... && make iso` заменяет демо-программу на вашу.
user-app:
	@if [ -z "$(SRC)" ]; then \
		echo "usage: make user-app SRC=path/to/file.c"; \
		exit 1; \
	fi
	@mkdir -p $(BUILD_DIR)/user
	$(CC) -ffreestanding -fno-pic -fno-pie -fno-stack-protector -nostdlib -static \
		-Wall -Wextra -Iuser -c $(SRC) -o $(BUILD_DIR)/user/hello.o
	$(LD) -n -T user/user.ld -nostdlib -static -o $(USER_ELF) $(BUILD_DIR)/user/hello.o
	@echo "--- built $(USER_ELF) from $(SRC) -- run 'make iso' to embed it as the boot module ---"

# ASC (ApexOS Simple Compiler) — наш собственный, написанный с нуля
# компилятор небольшого C-подобного подмножества (см. tools/asc.c для
# честного списка ограничений). Компилируется системным gcc как
# обычная host-программа (как и `as`/`ld`, которые сама использует) —
# ей не нужно быть самим ApexOS, чтобы генерировать код ДЛЯ ApexOS.
ASC := $(BUILD_DIR)/tools/asc

$(ASC): tools/asc.c
	@mkdir -p $(dir $@)
	gcc -Wall -Wextra -O2 -o $@ tools/asc.c

asc: $(ASC)
	@echo "--- built $(ASC) ---"

# Компилирует .c через ASC вместо системного gcc/as/ld-через-C.
# Тот же выходной путь $(USER_ELF), что и user-app — `make iso` после
# этого подхватит результат как boot-модуль.
asc-app: $(ASC)
	@if [ -z "$(SRC)" ]; then \
		echo "usage: make asc-app SRC=path/to/file.c"; \
		exit 1; \
	fi
	@mkdir -p $(BUILD_DIR)/user
	$(ASC) $(SRC) $(BUILD_DIR)/user/hello.S
	$(AS) -ffreestanding -fno-pic -fno-pie -c $(BUILD_DIR)/user/hello.S -o $(BUILD_DIR)/user/hello.o
	$(LD) -n -T user/user.ld -nostdlib -static -o $(USER_ELF) $(BUILD_DIR)/user/hello.o
	@echo "--- built $(USER_ELF) from $(SRC) via ASC -- run 'make iso' to embed it ---"

# Проверка того, что Multiboot2 header реально находится в первых
# 32 KiB файла — пункт 1 обязательной проверки из ТЗ.
check-multiboot: $(KERNEL_ELF)
	@if command -v grub-file >/dev/null 2>&1; then \
		grub-file --is-x86-multiboot2 $(KERNEL_ELF) && echo "OK: valid multiboot2 kernel"; \
	else \
		echo "grub-file not found, skipping (see 'make test' for manual check)"; \
	fi

iso: $(KERNEL_ELF) $(USER_ELF)
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	cp $(USER_ELF) $(ISO_DIR)/boot/hello.elf
	printf 'set timeout=0\nset default=0\n\nmenuentry "ApexOS" {\n\tmultiboot2 /boot/kernel.elf\n\tmodule2 /boot/hello.elf HELLO.ELF\n\tboot\n}\n' > $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO_IMAGE) $(ISO_DIR)

run: iso
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -m 256M -vga std

run-serial: iso
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -m 256M -vga std -serial stdio -display none

run-ata: iso
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -m 256M -vga std -serial stdio \
		-drive file=disk.img,format=raw,if=ide

run-sata: iso
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -m 256M -vga std -serial stdio \
		-drive file=disk.img,format=raw,if=none,id=sata_disk \
		-device ahci,id=ahci -device ide-hd,drive=sata_disk,bus=ahci.0

debug: iso
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -m 256M -vga std -serial stdio -s -S

# test: то, что реально можно проверить БЕЗ qemu — структурная валидация
# бинарника (multiboot2 header внутри первых 32 KiB, checksum, entry point).
test: $(KERNEL_ELF)
	@python3 tools/check_multiboot2.py $(KERNEL_ELF)

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR) $(ISO_IMAGE)

-include $(DEPS)
