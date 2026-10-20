# QEMU riscv64 'virt' machine. OpenSBI (QEMU's default -bios) loads -kernel
# at 0x8020_0000 and starts it in S-mode with a0 = hart, a1 = device tree.

LOAD_ADDR := 0x80200000
include mk/arch-rv64.mk

PLAT_SRCS := \
	plat/qemu-virt/plat.c

QEMU ?= qemu-system-riscv64

RUN_CMD := $(QEMU) -machine virt -cpu rv64,v=true,vlen=256 -smp 4 -m 256M \
	-nographic -bios default -kernel $(IMAGE)

run: $(IMAGE)
	$(RUN_CMD)
