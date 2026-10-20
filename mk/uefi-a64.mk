# Any Arm board that boots through UEFI and describes itself by ACPI: the
# CIX P1 first (docs/v3-plan.md, step 3). The image is the AArch64 one, an
# EFI application; plat/uefi has nothing board-specific.
#
#   make PLATFORM=uefi-a64 run     boot it once on the board (tools/efi-boot.py)
#   make PLATFORM=uefi-a64 test    the same, checking the transcript
#
# The board runs FreeBSD, with root over ssh as BOARD; each run installs
# the image on its EFI partition, sets BootNext and reboots, and FreeBSD
# comes back after. The kernel arguments (LAYOUT, BOOTARGS) are the boot
# entry's load options; "reboot" is added so the board goes back to
# FreeBSD after its tests. BOOTARGS=stub=reset stops in the stub,
# which reports what UEFI gave and resets the board.

include mk/arch-aarch64.mk

PLAT_SRCS := \
	plat/uefi/plat.c \
	drivers/sbsa_wdt.c

BOARD ?= root@192.168.0.19
SERIAL ?= $(firstword $(wildcard /dev/cu.usbserial-*))
BOOT_TIMEOUT := 900

ifneq ($(findstring stub=reset,$(BOOTARGS)),)
EXPECT := planar: EFI stub resetting
else
EXPECT := efi-boot: the board reset itself
endif

RUN_CMD := python3 -I tools/efi-boot.py --host $(BOARD) --port $(SERIAL) --image $(IMAGE) \
	--bootargs 'layout=$(LAYOUT) reboot $(BOOTARGS)' --log $(BUILD)/efi-boot.log

run: $(IMAGE)
	@test -n "$(SERIAL)" || { echo "no serial adapter found (set SERIAL=...)"; exit 1; }
	$(RUN_CMD)

CHECK_FLAGS := $(FULL_CHECK)
