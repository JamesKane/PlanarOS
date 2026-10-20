# AArch64 (EL1; entered at EL1 or EL2): arch compile units, flags and the
# image rule. Included by mk/<platform>.mk. The image runs wherever it's
# loaded, so there's no load address.

ARCH := aarch64
CC   := $(CROSS_CC)

# No FP/SIMD in the kernel (docs §7.4). Armv8.2-A is the oldest core on
# any target board (A55/A76, A78C/X1C), so LSE atomics are available.
TARGET_FLAGS := --target=aarch64-unknown-none-elf -march=armv8.2-a -mgeneral-regs-only

# The kernel runs wherever it's loaded (entry.S relocates it): position-
# independent, with every symbol local, so nothing goes through a GOT.
PIE_FLAGS := -fpie -fvisibility=hidden -fdirect-access-external-data

CFLAGS  += $(TARGET_FLAGS) $(FREESTANDING_CFLAGS) $(PIE_FLAGS)
ASFLAGS := $(TARGET_FLAGS)
LDFLAGS := $(TARGET_FLAGS) $(FREESTANDING_LDFLAGS) -Wl,-pie -Wl,-z,notext \
	-Wl,-T,arch/aarch64/link.ld

ARCH_SRCS := \
	arch/aarch64/entry.S \
	arch/aarch64/arch.c \
	arch/aarch64/efi.c \
	arch/aarch64/gicv3.c \
	arch/aarch64/lazy.c \
	arch/aarch64/lazy.S \
	arch/aarch64/mmu.c \
	arch/aarch64/switch.S \
	arch/aarch64/trap.c \
	arch/aarch64/vectors.S

# User programs get FP/SIMD (switched lazily, lazy.c); the compiler uses
# it freely.
USER_FLAGS := --target=aarch64-unknown-none-elf -march=armv8.2-a
include mk/user.mk

ELF   := $(BUILD)/planar.elf
IMAGE := $(BUILD)/planar.bin

# entry.S applies relative relocations only; and the EFI stub runs
# before it does, so it may use no absolute address at all (efi.c).
$(ELF): $$(OBJS) arch/aarch64/link.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@
	@if $(READELF) -r $@ | grep -E '^[0-9a-f]+ +[0-9a-f]+ R_' | grep -v R_AARCH64_RELATIVE; then \
	  echo "$@: relocations entry.S can't apply"; rm -f $@; exit 1; fi
	@if $(READELF) -r $(BUILD)/arch/aarch64/efi.c.o | \
	  awk '/^Relocation section/ { debug = $$3 ~ /debug/ } !debug' | \
	  grep -E 'R_AARCH64_(ABS(16|32|64)|ADR_GOT|LD64_GOT)'; then \
	  echo "efi.c: absolute addresses, which the stub can't use"; rm -f $@; exit 1; fi

# Flat binary with the arm64 Linux image header first: U-Boot's booti and
# QEMU's -kernel both take it.
$(IMAGE): $(ELF)
	$(OBJCOPY) -O binary $< $@
