# AArch64 (EL1; entered at EL1 or EL2): arch compile units, flags and the
# image rule. Included by mk/<platform>.mk, which sets LOAD_ADDR first.

ARCH := aarch64
CC   := $(CROSS_CC)

# No FP/SIMD in the kernel (docs §7.4). The MMU is off until the memory
# layout is chosen (VQ3), so every access is Device memory and must be
# aligned: -mstrict-align keeps the compiler from merging unaligned loads.
TARGET_FLAGS := --target=aarch64-unknown-none-elf -mgeneral-regs-only -mstrict-align

CFLAGS  += $(TARGET_FLAGS) $(FREESTANDING_CFLAGS)
ASFLAGS := $(TARGET_FLAGS)
LDFLAGS := $(TARGET_FLAGS) $(FREESTANDING_LDFLAGS) \
	-Wl,-T,arch/aarch64/link.ld -Wl,--defsym=LOAD_ADDR=$(LOAD_ADDR)

ARCH_SRCS := \
	arch/aarch64/entry.S \
	arch/aarch64/arch.c

ELF   := $(BUILD)/vectra.elf
IMAGE := $(BUILD)/vectra.bin

$(ELF): $$(OBJS) arch/aarch64/link.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

# Flat binary with the arm64 Linux image header first: U-Boot's booti and
# QEMU's -kernel both take it.
$(IMAGE): $(ELF)
	$(OBJCOPY) -O binary $< $@
