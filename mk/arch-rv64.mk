# RV64 (S-mode under OpenSBI): arch compile units, flags and the image rule.
# Included by mk/<platform>.mk, which sets LOAD_ADDR first.

ARCH := rv64
CC   := $(CROSS_CC)

# The kernel uses no FP or vector registers (docs §7.4): they hold user
# state, switched lazily.
TARGET_FLAGS := --target=riscv64-unknown-elf -march=rv64imac_zba_zbb -mabi=lp64 \
	-mcmodel=medany -mno-relax

CFLAGS  += $(TARGET_FLAGS) $(FREESTANDING_CFLAGS)
ASFLAGS := $(TARGET_FLAGS)
LDFLAGS := $(TARGET_FLAGS) $(FREESTANDING_LDFLAGS) \
	-Wl,-T,arch/rv64/link.ld -Wl,--defsym=LOAD_ADDR=$(LOAD_ADDR)

ARCH_SRCS := \
	arch/rv64/entry.S \
	arch/rv64/arch.c

ELF   := $(BUILD)/vectra.elf
IMAGE := $(BUILD)/vectra.bin

$(ELF): $$(OBJS) arch/rv64/link.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

# Flat binary with the RISC-V image header first: U-Boot's booti and QEMU's
# -kernel both take it.
$(IMAGE): $(ELF)
	$(OBJCOPY) -O binary $< $@
