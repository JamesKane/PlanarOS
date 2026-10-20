# QEMU aarch64 'virt' machine. With -kernel and no firmware, QEMU loads the
# image at the start of RAM (0x4000_0000) plus the header's text_offset,
# and starts the boot CPU at EL1 with x0 = device tree; the others wait in
# PSCI. QEMU_EL2=1 starts it at EL2 instead, to exercise the drop to EL1
# that the CIX and RK3588 boards need.
#
# EFI=1 boots the same image through UEFI instead: QEMU's edk2 build
# loads it from -kernel as an EFI application, with -append as its load
# options, and the stub (arch/aarch64/efi.c) enters the kernel. edk2
# passes the device tree, or with HW=acpi the ACPI tables instead (never
# both), and the kernel must say it read that one. The arguments start
# with "qemu", so the check can see the load options arrived.
#
# Each run writes the facts the kernel took from its description to
# $(BUILD)/facts-<how>.txt; make compare-hw checks the three agree.

include mk/arch-aarch64.mk

PLAT_SRCS := \
	plat/qemu-virt/plat.c

QEMU ?= qemu-system-aarch64

ifeq ($(QEMU_EL2),1)
QEMU_MACHINE := virt,gic-version=3,virtualization=on
else
QEMU_MACHINE := virt,gic-version=3
endif

HW ?= fdt
HOW := kernel
ifeq ($(EFI),1)
EDK2 ?= $(abspath $(dir $(shell command -v $(QEMU)))/../share/qemu/edk2-aarch64-code.fd)
QEMU_MACHINE := $(QEMU_MACHINE),acpi=$(if $(filter acpi,$(HW)),on,off)
QEMU_FIRMWARE := -bios $(EDK2) -nic none
HOW := efi-$(HW)
EFI_CHECK := --forbid '^V3.1 boot ok: from a device tree' \
	--forbid '^V3.1 boot ok: .*arguments "(?!qemu)'
endif
HW_NAME := $(if $(filter acpi,$(HW)),ACPI,the device tree)
HW_CHECK := --forbid '^V3.2 acpi ok: described by (?!$(HW_NAME))' \
	--facts $(BUILD)/facts-$(HOW).txt

RUN_CMD := $(QEMU) -machine $(QEMU_MACHINE) -cpu cortex-a76 -smp 4 -m $(QEMU_MEM) \
	-nographic $(QEMU_FIRMWARE) -kernel $(IMAGE) -append 'qemu $(BOOTARGS)'

run: $(IMAGE)
	$(RUN_CMD)

CHECK_FLAGS := $(FULL_CHECK) $(EFI_CHECK) $(HW_CHECK)
