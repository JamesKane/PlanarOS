# Planar -- one tree, every platform.
#
#   make PLATFORM=qemu-virt-rv64   (default) RV64 image for QEMU's virt machine
#   make PLATFORM=qemu-virt-a64    AArch64 image for QEMU's virt machine
#   make PLATFORM=hosted           host executable
#   make PLATFORM=... run          build and run where that makes sense
#   make PLATFORM=... test         build, run, and check the boot transcript
#   make test-all                  test every platform that can run here
#   make PLATFORM=... soak         boot SOAK times (100) and count failures
#   make check                     portable code must not test arch or platform
#
# mk/<platform>.mk names the arch and board compile units, and includes
# mk/arch-<arch>.mk for the arch units and the link rule. Portable units
# are listed here, once. The toolchain is pinned in mk/toolchain.mk.

PLATFORM ?= qemu-virt-rv64

ifeq ($(wildcard mk/$(PLATFORM).mk),)
$(error unknown PLATFORM '$(PLATFORM)': no mk/$(PLATFORM).mk)
endif

BUILD := build/$(PLATFORM)

# Portable compile units: built for every platform.
PORTABLE_SRCS := \
	kernel/main.c \
	kernel/area.c \
	kernel/bootargs.c \
	kernel/console.c \
	kernel/fdt.c \
	kernel/handle.c \
	kernel/intc.c \
	kernel/irq.c \
	kernel/kmalloc.c \
	kernel/kprintf.c \
	kernel/kstring.c \
	kernel/ktime.c \
	kernel/layout.c \
	kernel/memmap.c \
	kernel/pmm.c \
	kernel/port.c \
	kernel/process.c \
	kernel/sched.c \
	kernel/selftest.c \
	kernel/smp.c \
	kernel/syscall.c \
	kernel/va.c \
	drivers/pl011.c \
	drivers/plic.c \
	drivers/uart8250.c

# What every boot transcript must end with (tools/boot-check.py). A
# platform that can run every test sets CHECK_FLAGS to forbid skipping
# one (the VQ3 soak is skipped unless asked for), and terminal escapes in
# a program's KLog lines (the firmware's own output may have them).
CHECK_FLAGS :=
FULL_CHECK := --forbid 'skipped: (?!not asked for)' --forbid '^\[[a-z0-9]+\] .*\x1b'

EXPECT := V2.4 done

CFLAGS := -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -O2 -g
CPPFLAGS := -Iinclude -Ikernel -Idrivers

include mk/toolchain.mk

# Kernel arguments for QEMU runs: the user layout (docs §3.1, VQ3;
# kernel/layout.h) and anything else (BOOTARGS=vq3 runs the VQ3 soak).
LAYOUT ?= identity
BOOTARGS ?=
QEMU_MEM ?= 256M

# Link rules in mk/arch-*.mk name $$(OBJS) as a prerequisite before OBJS is
# set below, so prerequisites get a second expansion.
.SECONDEXPANSION:

include mk/$(PLATFORM).mk

CPPFLAGS += -Iarch/$(ARCH)/include

SRCS := $(PORTABLE_SRCS) $(ARCH_SRCS) $(PLAT_SRCS)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS)) $(ARCH_GEN_OBJS)

.PHONY: all run test test-all vq3 soak check clean
.DEFAULT_GOAL := all

all: $(IMAGE)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

# Seconds a boot may take to reach EXPECT; a board over the serial
# console needs minutes (mk/ky-x1.mk).
BOOT_TIMEOUT ?= 20

test: $(IMAGE)
	python3 -I tools/boot-check.py --timeout $(BOOT_TIMEOUT) --expect '$(EXPECT)' $(CHECK_FLAGS) \
	  -- $(RUN_CMD)


TEST_PLATFORMS := qemu-virt-rv64 qemu-virt-a64

# Both ISAs in both layouts, AArch64 once more entered at EL2 (as the CIX
# and RK3588 boards enter it), and the hosted build.
test-all:
	@for p in $(TEST_PLATFORMS); do for l in identity global; do \
	  $(MAKE) --no-print-directory PLATFORM=$$p LAYOUT=$$l test || exit 1; \
	done; done
	@$(MAKE) --no-print-directory PLATFORM=qemu-virt-a64 QEMU_EL2=1 LAYOUT=global test
	@$(MAKE) --no-print-directory PLATFORM=hosted test

# The VQ3 measurement (docs/v1-plan.md): the same random workload against
# both layouts, on each QEMU platform. Slow, so not part of make test.
vq3:
	@for p in $(TEST_PLATFORMS); do \
	  $(MAKE) --no-print-directory PLATFORM=$$p BOOTARGS=vq3 QEMU_MEM=$(QEMU_MEM) test | \
	    grep -E 'V1.5 vq3|boot-check'; \
	done

# Boot many times and count failures: races that show once in a hundred
# boots don't show in one. make PLATFORM=... soak [SOAK=100]
SOAK ?= 100

soak: $(IMAGE)
	@rm -f $(BUILD)/soak-fail-*.log; fails=0; for i in $$(seq 1 $(SOAK)); do \
	  python3 -I tools/boot-check.py --timeout $(BOOT_TIMEOUT) --expect '$(EXPECT)' $(CHECK_FLAGS) \
	    --log $(BUILD)/soak-last.log -- $(RUN_CMD) >/dev/null || \
	    { fails=$$((fails + 1)); cp $(BUILD)/soak-last.log $(BUILD)/soak-fail-$$fails.log; }; \
	done; echo "soak: $$fails of $(SOAK) boots failed ($(PLATFORM))"; test $$fails -eq 0

check:
	@tools/check-portable.sh

clean:
	rm -rf build
