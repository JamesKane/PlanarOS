# QEMU aarch64 'virt' machine. With -kernel and no firmware, QEMU loads the
# image at the start of RAM (0x4000_0000) plus the header's text_offset,
# and starts the boot CPU at EL1 with x0 = device tree; the others wait in
# PSCI. QEMU_EL2=1 starts it at EL2 instead, to exercise the drop to EL1
# that the CIX and RK3588 boards need.

LOAD_ADDR := 0x40200000
include mk/arch-aarch64.mk

PLAT_SRCS := \
	plat/qemu-virt/plat.c

QEMU ?= qemu-system-aarch64

ifeq ($(QEMU_EL2),1)
QEMU_MACHINE := virt,gic-version=3,virtualization=on
else
QEMU_MACHINE := virt,gic-version=3
endif

RUN_CMD := $(QEMU) -machine $(QEMU_MACHINE) -cpu cortex-a76 -smp 4 -m 256M \
	-nographic -kernel $(IMAGE)

run: $(IMAGE)
	$(RUN_CMD)
