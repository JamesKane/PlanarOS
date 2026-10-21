# Any Arm board that boots through UEFI and describes itself by ACPI: the
# CIX P1 first (docs/v3-plan.md, step 3). The image is the AArch64 one, an
# EFI application; plat/uefi has nothing board-specific.
#
#   make PLATFORM=uefi-a64 run     boot it once on the board (tools/efi-boot.py)
#   make PLATFORM=uefi-a64 test    the same, checking the transcript
#
# The board runs FreeBSD, with root over ssh as BOARD; each run installs
# the image on its EFI partition, sets BootNext and reboots, and FreeBSD
# comes back after. The kernel arguments (BOOTARGS) are the boot
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

# The GPU's firmware (v5-plan decision 5): not in the repository, but
# copied from the NAS into the build, checked by its SHA-256, and
# installed as DEVS/mali_csffw.bin, where gpu-panthor reads it.
FIRMWARE_DIR ?= /Volumes/nas/firmware
MALI_FW := $(BUILD)/firmware/mali_csffw.bin
MALI_FW_SHA256 := 267f958f7e63065b8cdb6c2119526f92f0860b2376ad4b00bcc121727d6c4ed5

$(MALI_FW):
	@mkdir -p $(dir $@)
	@test -f $(FIRMWARE_DIR)/mali_csffw.bin || \
	  { echo "no $(FIRMWARE_DIR)/mali_csffw.bin (set FIRMWARE_DIR=...)"; exit 1; }
	@echo "$(MALI_FW_SHA256)  $(FIRMWARE_DIR)/mali_csffw.bin" | shasum -a 256 -c --status || \
	  { echo "$(FIRMWARE_DIR)/mali_csffw.bin isn't the firmware expected"; exit 1; }
	cp $(FIRMWARE_DIR)/mali_csffw.bin $@

# The commands and scripts it loads from storage (Makefile's DISK_FILES),
# and the firmware, installed next to the image.
RUN_CMD = python3 -I tools/efi-boot.py --host $(BOARD) --port $(SERIAL) --image $(IMAGE) \
	--bootargs 'reboot $(BOOTARGS)' --log $(BUILD)/efi-boot.log \
	$(foreach f,$(DISK_FILES) $(MALI_FW):DEVS/mali_csffw.bin,--file $(f))
TEST_DEPS = $(DISK_DEPS) $(MALI_FW)

run: $(IMAGE) $$(TEST_DEPS)
	@test -n "$(SERIAL)" || { echo "no serial adapter found (set SERIAL=...)"; exit 1; }
	$(RUN_CMD)

CHECK_FLAGS := $(FULL_CHECK)
