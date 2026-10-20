# AArch64 (EL1; entered at EL1 or EL2): arch compile units, flags and the
# image rule. Included by mk/<platform>.mk, which sets LOAD_ADDR first.

ARCH := aarch64
CC   := $(CROSS_CC)

# No FP/SIMD in the kernel (docs §7.4). Armv8.2-A is the oldest core on
# any target board (A55/A76, A78C/X1C), so LSE atomics are available.
TARGET_FLAGS := --target=aarch64-unknown-none-elf -march=armv8.2-a -mgeneral-regs-only

CFLAGS  += $(TARGET_FLAGS) $(FREESTANDING_CFLAGS)
ASFLAGS := $(TARGET_FLAGS)
LDFLAGS := $(TARGET_FLAGS) $(FREESTANDING_LDFLAGS) \
	-Wl,-T,arch/aarch64/link.ld -Wl,--defsym=LOAD_ADDR=$(LOAD_ADDR)

ARCH_SRCS := \
	arch/aarch64/entry.S \
	arch/aarch64/arch.c \
	arch/aarch64/gicv3.c \
	arch/aarch64/switch.S \
	arch/aarch64/trap.c \
	arch/aarch64/vectors.S

ELF   := $(BUILD)/planar.elf
IMAGE := $(BUILD)/planar.bin

$(ELF): $$(OBJS) arch/aarch64/link.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

# Flat binary with the arm64 Linux image header first: U-Boot's booti and
# QEMU's -kernel both take it.
$(IMAGE): $(ELF)
	$(OBJCOPY) -O binary $< $@
