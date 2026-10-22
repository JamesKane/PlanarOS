# Orange Pi RV2 (Ky X1, a SpacemiT K1). The stock firmware in SPI flash
# (SPL, OpenSBI 1.3, U-Boot 2022.10) starts the image in S-mode. DRAM
# starts at 0 and OpenSBI owns its first 512 KiB, so the image loads at
# 2 MiB, where a Linux Image would.
#
#   make PLATFORM=ky-x1 run        boot it over the serial console (U-Boot loadx)
#   make PLATFORM=ky-x1 test       the same, checking the transcript
#
# Over the serial console, the kernel arguments (BOOTARGS) go in
# U-Boot's bootargs, always with "reboot": the board resets after its
# tests, so runs can follow each other without anyone at the power
# switch (as on the CIX, mk/uefi-a64.mk). A board left idle in the
# kernel answers no U-Boot prompt.

LOAD_ADDR := 0x200000
BOOT_TIMEOUT := 900

# Success is the board going round again; serial-boot.py reports a
# reset the firmware refused.
EXPECT := serial-boot: the board reset itself
include mk/arch-rv64.mk

PLAT_SRCS := \
	plat/ky-x1/plat.c \
	plat/ky-x1/wdt.c

# The board's serial adapter.
SERIAL ?= $(firstword $(wildcard /dev/cu.usbserial-*))

# SD=1 loads the image from the SD card (make sdcard put it there) with
# U-Boot's load mmc, not over the serial line, which takes minutes.
RUN_CMD := python3 -I tools/serial-boot.py --port $(SERIAL) --image $(IMAGE) \
	--bootargs 'reboot $(BOOTARGS)' --log $(BUILD)/serial-boot.log $(if $(SD),--sd)

run: $(IMAGE)
	@test -n "$(SERIAL)" || { echo "no serial adapter found (set SERIAL=...)"; exit 1; }
	$(RUN_CMD)

# The GPU's firmware (v5-plan decision 5): not in the repository, but
# copied from the NAS into the build, checked by its SHA-256, and
# installed on the SD card as DEVS/rogue.fw, where gpu-rogue reads it.
FIRMWARE_DIR ?= /Volumes/nas/firmware
ROGUE_FW_NAME := rogue_36.29.52.182_v1.fw
ROGUE_FW := $(BUILD)/firmware/$(ROGUE_FW_NAME)
ROGUE_FW_SHA256 := 79e465eaf12ed1bc38f43deb48d6dc971f7485b063becb511f14822e3c39ada5

$(ROGUE_FW):
	@mkdir -p $(dir $@)
	@test -f $(FIRMWARE_DIR)/$(ROGUE_FW_NAME) || \
	  { echo "no $(FIRMWARE_DIR)/$(ROGUE_FW_NAME) (set FIRMWARE_DIR=...)"; exit 1; }
	@echo "$(ROGUE_FW_SHA256)  $(FIRMWARE_DIR)/$(ROGUE_FW_NAME)" | shasum -a 256 -c --status || \
	  { echo "$(FIRMWARE_DIR)/$(ROGUE_FW_NAME) isn't the firmware expected"; exit 1; }
	cp $(FIRMWARE_DIR)/$(ROGUE_FW_NAME) $@

# The board's own storage (V4 step 6): the SD card's FAT partition, mounted
# here, gets the image, the commands and scripts under EFI/planar (as the
# CIX's EFI partition does), the GPU's firmware in DEVS, and a boot.scr,
# so the board boots to the Shell on power-on. Files already there are
# left.
SD_MOUNT ?= /Volumes/OMEGA
BOOTSCR := $(BUILD)/boot.scr

$(BOOTSCR): plat/ky-x1/boot.cmd tools/mkscript.py
	@mkdir -p $(dir $@)
	python3 -I tools/mkscript.py $< $@

.PHONY: sdcard
sdcard: $(IMAGE) $(BOOTSCR) $$(DISK_DEPS) $(ROGUE_FW)
	@test -d $(SD_MOUNT) || { echo "no SD card at $(SD_MOUNT) (set SD_MOUNT=...)"; exit 1; }
	mkdir -p $(SD_MOUNT)/EFI/planar/C $(SD_MOUNT)/EFI/planar/S $(SD_MOUNT)/EFI/planar/DEVS
	cp -X $(IMAGE) $(SD_MOUNT)/EFI/planar/planar.bin
	cp -X $(ROGUE_FW) $(SD_MOUNT)/EFI/planar/DEVS/rogue.fw
	@for f in $(DISK_FILES); do cp -X $${f%%:*} $(SD_MOUNT)/EFI/planar/$${f#*:}; done
	cp -X $(BOOTSCR) $(SD_MOUNT)/boot.scr
	-dot_clean -m $(SD_MOUNT)
	sync
	@echo "Eject the card, put it in the board and power on."

CHECK_FLAGS := $(FULL_CHECK)
