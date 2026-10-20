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
	kernel/console.c \
	kernel/fdt.c \
	kernel/handle.c \
	kernel/intc.c \
	kernel/irq.c \
	kernel/kprintf.c \
	kernel/kstring.c \
	kernel/ktime.c \
	kernel/memmap.c \
	kernel/pmm.c \
	kernel/process.c \
	kernel/sched.c \
	kernel/selftest.c \
	kernel/smp.c \
	kernel/syscall.c \
	drivers/pl011.c \
	drivers/plic.c \
	drivers/uart8250.c

# What every boot transcript must end with (tools/boot-check.py).
EXPECT := V1.4 done

CFLAGS := -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -O2 -g
CPPFLAGS := -Iinclude -Ikernel -Idrivers

include mk/toolchain.mk

# Link rules in mk/arch-*.mk name $$(OBJS) as a prerequisite before OBJS is
# set below, so prerequisites get a second expansion.
.SECONDEXPANSION:

include mk/$(PLATFORM).mk

CPPFLAGS += -Iarch/$(ARCH)/include

SRCS := $(PORTABLE_SRCS) $(ARCH_SRCS) $(PLAT_SRCS)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS)) $(ARCH_GEN_OBJS)

.PHONY: all run test test-all soak check clean
.DEFAULT_GOAL := all

all: $(IMAGE)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

test: $(IMAGE)
	python3 -I tools/boot-check.py --expect '$(EXPECT)' -- $(RUN_CMD)

TEST_PLATFORMS := qemu-virt-rv64 qemu-virt-a64 hosted

# ...and AArch64 once more entered at EL2, as the CIX and RK3588 boards
# enter it (the drop to EL1, PSCI over smc).
test-all:
	@for p in $(TEST_PLATFORMS); do \
	  $(MAKE) --no-print-directory PLATFORM=$$p test || exit 1; \
	done
	@$(MAKE) --no-print-directory PLATFORM=qemu-virt-a64 QEMU_EL2=1 test

# Boot many times and count failures: races that show once in a hundred
# boots don't show in one. make PLATFORM=... soak [SOAK=100]
SOAK ?= 100

soak: $(IMAGE)
	@fails=0; for i in $$(seq 1 $(SOAK)); do \
	  python3 -I tools/boot-check.py --timeout 10 --expect '$(EXPECT)' \
	    --log $(BUILD)/soak-last.log -- $(RUN_CMD) >/dev/null || \
	    { fails=$$((fails + 1)); cp $(BUILD)/soak-last.log $(BUILD)/soak-fail-$$fails.log; }; \
	done; echo "soak: $$fails of $(SOAK) boots failed ($(PLATFORM))"; test $$fails -eq 0

check:
	@tools/check-portable.sh

clean:
	rm -rf build
