# Orange Pi RV2 (Ky X1, a SpacemiT K1). The stock firmware in SPI flash
# (SPL, OpenSBI 1.3, U-Boot 2022.10) starts the image in S-mode. DRAM
# starts at 0 and OpenSBI owns its first 512 KiB, so the image loads at
# 2 MiB, where a Linux Image would.
#
#   make PLATFORM=ky-x1 run        boot it over the serial console (U-Boot loadx)
#   make PLATFORM=ky-x1 test       the same, checking the transcript
#
# Over the serial console, the kernel arguments (LAYOUT, BOOTARGS) go in
# U-Boot's bootargs; "reboot" makes the board reset after its tests, so
# runs can follow each other without anyone at the power switch.

LOAD_ADDR := 0x200000
BOOT_TIMEOUT := 900

# With "reboot", success is the board going round again; serial-boot.py
# reports a reset the firmware refused.
ifneq ($(findstring reboot,$(BOOTARGS))$(findstring hang,$(BOOTARGS)),)
EXPECT := serial-boot: the board reset itself
endif
include mk/arch-rv64.mk

PLAT_SRCS := \
	plat/ky-x1/plat.c \
	plat/ky-x1/wdt.c

# The board's serial adapter.
SERIAL ?= $(firstword $(wildcard /dev/cu.usbserial-*))

RUN_CMD := python3 -I tools/serial-boot.py --port $(SERIAL) --image $(IMAGE) \
	--bootargs 'layout=$(LAYOUT) $(BOOTARGS)' --log $(BUILD)/serial-boot.log

run: $(IMAGE)
	@test -n "$(SERIAL)" || { echo "no serial adapter found (set SERIAL=...)"; exit 1; }
	$(RUN_CMD)

CHECK_FLAGS := $(FULL_CHECK)
