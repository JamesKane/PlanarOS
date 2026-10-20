# Hosted build: a host executable. Kernel logic runs as a process for fast
# iteration and sanitizers (docs §6.4).
#
# ACPI_BOARD=cix-p1 runs it on that board's ACPI tables
# (docs/boards/<board>/acpi/*.dat) instead of nothing, and the transcript
# must have every line of the board's acpi-expect.txt.

include mk/arch-host.mk

PLAT_SRCS := \
	plat/hosted/plat.c

ifneq ($(ACPI_BOARD),)
ACPI_DIR := docs/boards/$(ACPI_BOARD)
ACPI_BOOT_CPU ?= $(shell sed -n 's/^hw: cpus.*; boot //p' $(ACPI_DIR)/acpi-expect.txt)
RUN_CMD := $(IMAGE) --acpi $(wildcard $(ACPI_DIR)/acpi/*.dat) --boot-cpu $(ACPI_BOOT_CPU)
CHECK_FLAGS := --require-lines $(ACPI_DIR)/acpi-expect.txt
else
RUN_CMD := $(IMAGE)
endif

run: $(IMAGE)
	$(RUN_CMD)
