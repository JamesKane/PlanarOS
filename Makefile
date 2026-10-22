# Planar -- one tree, every platform.
#
#   make PLATFORM=qemu-virt-rv64   (default) RV64 image for QEMU's virt machine
#   make PLATFORM=qemu-virt-a64    AArch64 image for QEMU's virt machine
#                                  (EFI=1 runs it through UEFI, edk2;
#                                  HW=acpi with ACPI, not a device tree)
#   make PLATFORM=uefi-a64         AArch64 image for UEFI boards with ACPI
#                                  (the CIX P1; run and test boot it there)
#   make PLATFORM=ky-x1            RV64 image for the Orange Pi RV2
#                                  (run and test boot it over the serial console)
#   make PLATFORM=hosted           host executable
#   make PLATFORM=... run          build and run where that makes sense
#   make PLATFORM=... test         build, run, and check the boot transcript
#   make test-all                  test every platform that can run here
#   make compare-hw                the AArch64 boots took the same facts
#                                  from the device tree and ACPI
#   make PLATFORM=... soak         boot SOAK times (100) and count failures
#   make vq3                       the VQ3 soak of Areas, on each QEMU platform
#   make check                     portable code must not test arch or platform,
#                                  and no two drivers' match tables tie
#   make hwdb-test                 hwdb's device graph on the captured trees
#
# mk/<platform>.mk names the arch and board compile units, and includes
# mk/arch-<arch>.mk for the arch units and the link rule. Portable units
# are listed here, once. The toolchain is pinned in mk/toolchain.mk.

PLATFORM ?= qemu-virt-rv64

ifeq ($(wildcard mk/$(PLATFORM).mk),)
$(error unknown PLATFORM '$(PLATFORM)': no mk/$(PLATFORM).mk)
endif

BUILD = build/$(PLATFORM)$(BUILD_SUFFIX)

# Portable compile units: built for every platform.
PORTABLE_SRCS := \
	kernel/main.c \
	kernel/acpi.c \
	kernel/area.c \
	kernel/bootargs.c \
	kernel/bootinfo.c \
	kernel/console.c \
	kernel/cpuclass.c \
	kernel/devmem.c \
	kernel/fdt.c \
	kernel/futex.c \
	kernel/handle.c \
	kernel/hw.c \
	kernel/intc.c \
	kernel/irq.c \
	kernel/kmalloc.c \
	kernel/kprintf.c \
	kernel/kstring.c \
	kernel/ktime.c \
	kernel/lease.c \
	kernel/library.c \
	kernel/memmap.c \
	kernel/pmm.c \
	kernel/port.c \
	kernel/prof.c \
	kernel/process.c \
	kernel/sched.c \
	kernel/selftest.c \
	kernel/smp.c \
	kernel/syscall.c \
	drivers/pl011.c \
	drivers/plic.c \
	drivers/uart8250.c

# What every boot transcript must end with (tools/boot-check.py). A
# platform that can run every test sets CHECK_FLAGS to forbid skipping
# one (the VQ3 soak is skipped unless asked for), and terminal escapes in
# a program's KLog lines (the firmware's own output may have them).
CHECK_FLAGS :=
FULL_CHECK := --forbid 'skipped: (?!not asked for)' --forbid '^\[[a-z0-9]+\] .*\x1b'

EXPECT := V7.4 done

CFLAGS := -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -O2 -g

# make PROFILE=1: kernel/prof.h's marks count cycles (a separate build).
ifeq ($(PROFILE),1)
CFLAGS += -DPLANAR_PROF
BUILD_SUFFIX := -prof
endif
CPPFLAGS := -Iinclude -Ikernel -Idrivers

include mk/toolchain.mk
include mk/acpica.mk

# Kernel arguments for QEMU runs (BOOTARGS=vq3 runs the VQ3 soak).
BOOTARGS ?=
QEMU_MEM ?= 256M

# The disks QEMU's NVMe and SD card hold (tools/mkdisk.py): GPT, a FAT32
# EFI partition with C:hello on it (step 6's dostest runs it), then a
# partition each block of which holds its own number. Writes are
# discarded (snapshot), so the images stay as made.
DISK = $(BUILD)/disk.img
SDCARD = $(BUILD)/sd.img
QEMU_DISK = -drive if=none,id=disk,file=$(DISK),format=raw,snapshot=on \
	-device nvme,serial=planar0,drive=disk \
	-drive if=none,id=sd,file=$(SDCARD),format=raw,snapshot=on \
	-device sdhci-pci -device sd-card,drive=sd

# USB (v5-plan step 4): an xHCI with a hub, a keyboard and a mouse behind
# it, and QMP's socket, through which the boot check types what the
# input test asks for (tools/boot-check.py).
QMP_SOCK = $(BUILD)/qmp.sock
QEMU_USB = -device qemu-xhci,id=xhci -device usb-hub,bus=xhci.0,port=1 \
	-device usb-kbd,bus=xhci.0,port=1.1 -device usb-mouse,bus=xhci.0,port=1.2 \
	-qmp unix:$(QMP_SOCK),server=on,wait=off
QEMU_INPUT_CHECK = --qmp $(QMP_SOCK) --forbid '^\[hwdb\] V5.4 input: not exercised' \
	--forbid 'typing not exercised'
# A QEMU machine with a screen (RV64's ramfb, AArch64's through edk2)
# must show it: display's and intuition's tests may not be skipped there.
QEMU_SCREEN_CHECK = --forbid '^\[hwdb\] V(5\.3 display|7\.[0-9] (screen|windows|input)): not exercised'

# Link rules in mk/arch-*.mk name $$(OBJS) as a prerequisite before OBJS is
# set below, so prerequisites get a second expansion.
.SECONDEXPANSION:

include mk/$(PLATFORM).mk

CPPFLAGS += -Iarch/$(ARCH)/include

# What a disk's EFI partition holds under EFI/planar (tools/mkdisk.py for
# QEMU's, tools/efi-boot.py for a board's): the commands in C/, and the
# scripts (cmd/scripts) in S/. C/hello is the built-in hello, for dostest.
DISK_FILES = $(BUILD)/user/hello.elf:C/hello \
	$(foreach p,$(CMD_PROGS),$(BUILD)/user/cmd/$(p).elf:C/$(p)) \
	$(foreach f,$(wildcard cmd/scripts/*),$(f):S/$(notdir $(f)))
DISK_DEPS = tools/mkdisk.py $(BUILD)/user/hello.elf $(CMD_ELFS) $(wildcard cmd/scripts/*)

$(BUILD)/disk.img $(BUILD)/sd.img: $$(DISK_DEPS)
	@mkdir -p $(dir $@)
	python3 -I tools/mkdisk.py $@ $(foreach f,$(DISK_FILES),--file $(subst :,:EFI/planar/,$(f)))

SRCS := $(PORTABLE_SRCS) $(ARCH_SRCS) $(PLAT_SRCS)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS)) $(ARCH_GEN_OBJS)

.PHONY: all run test test-all compare-hw vq3 soak check hwdb-test dos-test power-test display-test rt-test exec-test utility-test span-fuzz clib-test dos-lib-test gfx-test intuition-test clean
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
# console needs minutes (mk/ky-x1.mk). RV64 QEMU's run near 20 once it
# had a screen and display's tests (V7 step 1).
BOOT_TIMEOUT ?= 40

# BOOT_LOG=file keeps the transcript.
test: $(IMAGE) $(TEST_DEPS)
	python3 -I tools/boot-check.py --timeout $(BOOT_TIMEOUT) --expect '$(EXPECT)' $(CHECK_FLAGS) \
	  $(if $(BOOT_LOG),--log $(BOOT_LOG)) -- $(RUN_CMD)


TEST_PLATFORMS := qemu-virt-rv64 qemu-virt-a64

# Both ISAs; AArch64 once more entered at EL2 (as the CIX
# and RK3588 boards enter it), and through UEFI at EL1 and EL2, from the
# device tree and from ACPI; RV64 with 17 GiB (RAM and the device tree
# above 16 GiB); the hosted build, alone and on the CIX's ACPI tables.
# Then the AArch64 boots' facts are compared, and hwdb's graph tested.
test-all:
	@rm -f $(A64_FACTS)/facts-*.txt $(A64_FACTS)/hwdb-facts-*.txt
	@for p in $(TEST_PLATFORMS); do \
	  $(MAKE) --no-print-directory PLATFORM=$$p test || exit 1; \
	done
	@$(MAKE) --no-print-directory PLATFORM=qemu-virt-a64 QEMU_EL2=1 test
	@$(MAKE) --no-print-directory PLATFORM=qemu-virt-a64 EFI=1 test
	@$(MAKE) --no-print-directory PLATFORM=qemu-virt-a64 EFI=1 QEMU_EL2=1 test
	@$(MAKE) --no-print-directory PLATFORM=qemu-virt-a64 EFI=1 HW=acpi test
	@$(MAKE) --no-print-directory PLATFORM=qemu-virt-a64 EFI=1 HW=acpi QEMU_EL2=1 test
	@$(MAKE) --no-print-directory PLATFORM=qemu-virt-rv64 QEMU_MEM=17G test
	@$(MAKE) --no-print-directory PLATFORM=hosted test
	@$(MAKE) --no-print-directory PLATFORM=hosted ACPI_BOARD=cix-p1 test
	@$(MAKE) --no-print-directory compare-hw
	@$(MAKE) --no-print-directory hwdb-test
	@$(MAKE) --no-print-directory power-test
	@$(MAKE) --no-print-directory display-test
	@$(MAKE) --no-print-directory rt-test
	@$(MAKE) --no-print-directory exec-test
	@$(MAKE) --no-print-directory utility-test
	@$(MAKE) --no-print-directory span-fuzz
	@$(MAKE) --no-print-directory clib-test
	@$(MAKE) --no-print-directory dos-lib-test
	@$(MAKE) --no-print-directory gfx-test
	@$(MAKE) --no-print-directory intuition-test

# lib/rt/string.c's word loops, which the ISAs' own replace, on the host
# with the sanitizers, renamed so as not to be the host's.
RT_TEST := build/rt/rt-test
RT_RENAME := -Dmemcpy=rt_memcpy -Dmemmove=rt_memmove -Dmemset=rt_memset -Dmemcmp=rt_memcmp

$(RT_TEST): lib/rt/string.c lib/rt/test.c conformance/memcheck.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iconformance) -ffreestanding -fno-builtin $(RT_RENAME) \
	  -c lib/rt/string.c -o $(dir $@)string.o
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iconformance) lib/rt/test.c $(dir $@)string.o -o $@

rt-test: $(RT_TEST)
	@$(RT_TEST)

# lib/exec's pools, AllocVec and RawDoFmt on the host with the
# sanitizers, over Areas lib/exec/test.c fakes.
EXEC_TEST := build/exec/exec-test

$(EXEC_TEST): lib/exec/pool.c lib/exec/fmt.c lib/exec/test.c conformance/fmtcheck.h include/planar/exec.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iconformance) lib/exec/pool.c lib/exec/fmt.c \
	  lib/exec/test.c -o $@

exec-test: $(EXEC_TEST)
	@$(EXEC_TEST)

# utility.library on the host with the sanitizers, against
# conformance/utilcheck.h; and its Span and Str calls under libFuzzer,
# a million inputs from an empty corpus each run.
comma := ,
UTILITY_TEST := build/utility/utility-test
SPAN_FUZZ := build/utility/span-fuzz
UTILITY_SRCS := $(filter-out %/test.c,$(wildcard lib/utility/*.c))

$(UTILITY_TEST): $(UTILITY_SRCS) lib/exec/fmt.c lib/utility/test.c conformance/utilcheck.h \
		include/planar/utility.h include/planar/exec.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iconformance) $(UTILITY_SRCS) lib/exec/fmt.c \
	  lib/utility/test.c -o $@

utility-test: $(UTILITY_TEST)
	@$(UTILITY_TEST)

$(SPAN_FUZZ): $(UTILITY_SRCS) lib/exec/fmt.c tools/span-fuzz/fuzz.c include/planar/utility.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(subst -fsanitize=,-fsanitize=fuzzer$(comma),$(HWDB_CFLAGS:-Iservers/hwdb=)) \
	  $(UTILITY_SRCS) lib/exec/fmt.c tools/span-fuzz/fuzz.c -o $@

span-fuzz: $(SPAN_FUZZ)
	@$(SPAN_FUZZ) -runs=1000000 -seed=1 > $(SPAN_FUZZ).log 2>&1 || { cat $(SPAN_FUZZ).log; exit 1; }
	@echo "span-fuzz: ok: $$(tail -1 $(SPAN_FUZZ).log)"

# lib/clib on the host with the sanitizers, its functions renamed clib_*
# so as not to be the host's: conformance/clibcheck.h's cases, then
# against the host's own (lib/clib/testhost.c, with the host's headers).
CLIB_TEST := build/clib/clib-test
CLIB_SRCS := $(filter-out %/test.c %/testhost.c,$(wildcard lib/clib/*.c))
CLIB_NAMES := malloc calloc realloc free memalignment exit _Exit abort strtol strtoll strtoul \
	strtoull strtoimax strtoumax imaxabs qsort bsearch abs labs llabs memset_explicit memchr \
	strlen strnlen strcmp strncmp strcpy strncpy strcat strncat strchr strrchr strstr snprintf \
	vsnprintf errno isalnum isalpha isblank iscntrl isdigit isgraph islower isprint ispunct \
	isspace isupper isxdigit toupper tolower
CLIB_TEST_OBJS := $(patsubst lib/clib/%.c,build/clib/%.o,$(CLIB_SRCS) lib/clib/test.c)

build/clib/%.o: lib/clib/%.c conformance/clibcheck.h $(wildcard include/clib/*.h)
	@mkdir -p $(dir $@)
	@$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iconformance) -Iinclude/clib -ffreestanding \
	  -fno-builtin $(foreach n,$(CLIB_NAMES),-D$(n)=clib_$(n)) -c $< -o $@

$(CLIB_TEST): $(CLIB_TEST_OBJS) lib/clib/testhost.c $(UTILITY_SRCS) lib/exec/pool.c lib/exec/fmt.c
	$(CROSS_CC) $(HWDB_CFLAGS) $(CLIB_TEST_OBJS) lib/clib/testhost.c $(UTILITY_SRCS) \
	  lib/exec/pool.c lib/exec/fmt.c -o $@

clib-test: $(CLIB_TEST)
	@$(CLIB_TEST)

# dos.library's ReadArgs, path calls and console on the host with the
# sanitizers: conformance/argscheck.h, random lines, and Printf over a
# fake console (lib/dos/test.c).
DOS_LIB_TEST := build/dos-lib/dos-lib-test
DOS_LIB_SRCS := lib/dos/args.c lib/dos/path.c lib/dos/con.c

$(DOS_LIB_TEST): $(DOS_LIB_SRCS) lib/dos/test.c conformance/argscheck.h include/planar/dos.h \
		$(UTILITY_SRCS) lib/exec/fmt.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iconformance) $(DOS_LIB_SRCS) $(UTILITY_SRCS) \
	  lib/exec/fmt.c lib/dos/test.c -o $@

dos-lib-test: $(DOS_LIB_TEST)
	@$(DOS_LIB_TEST)

# gfx.library (lib/gfx) on the host with the sanitizers, against
# conformance/gfxcheck.h's reference: its scalar entries, and on an Arm
# host its NEON ones (arch/aarch64/lib/gfx.c). RVV's run in the guest.
GFX_TEST := build/gfx/gfx-test
GFX_SRCS := $(filter-out %/test.c,$(wildcard lib/gfx/*.c))
GFX_NEON := $(if $(filter arm64 aarch64,$(shell uname -m)),arch/aarch64/lib/gfx.c)

$(GFX_TEST): $(GFX_SRCS) $(GFX_NEON) lib/gfx/test.c lib/gfx/rows.h lib/gfx/font8x8.h \
		conformance/gfxcheck.h conformance/gfxref.h include/planar/gfx.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iconformance) -Ilib/gfx $(if $(GFX_NEON),-DGFX_TEST_NEON) \
	  $(GFX_SRCS) $(GFX_NEON) lib/gfx/test.c -o $@

gfx-test: $(GFX_TEST)
	@$(GFX_TEST)

# intuition's compositor (servers/intuition/compose.c) on the host with the
# sanitizers, over made-up windows with and without chrome, against a
# naive compositor that redraws everything: through lib/gfx's scalar
# entries, and NEON's too on an Arm host. Then the chrome (chrome.c) and
# a client's events (events.c) alone.
INTUITION_TEST := build/intuition/intuition-test
INTUITION_HOST_SRCS := $(addprefix servers/intuition/,compose.c chrome.c events.c test.c)

$(INTUITION_TEST): $(GFX_SRCS) $(GFX_NEON) $(INTUITION_HOST_SRCS) $(wildcard servers/intuition/*.h) \
		lib/gfx/rows.h include/planar/gfx.h include/planar/intuition.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iservers/intuition) $(if $(GFX_NEON),-DGFX_TEST_NEON) \
	  $(GFX_SRCS) $(GFX_NEON) $(INTUITION_HOST_SRCS) -o $@

intuition-test: $(INTUITION_TEST)
	@$(INTUITION_TEST)

# One QEMU machine described three ways (mk/qemu-virt-a64.mk): the device
# tree from -kernel, the device tree through edk2, and ACPI through edk2.
# The kernel must take the same facts from each (kernel/hw.h), and hwdb's
# drivers must bind the same devices through edk2's device tree and
# through acpid (servers/hwdb/server.c, hwdb_facts).
A64_FACTS := build/qemu-virt-a64
compare-hw:
	@for f in kernel efi-fdt efi-acpi; do \
	  test -s $(A64_FACTS)/facts-$$f.txt || { echo "compare-hw: no facts from the $$f boot"; exit 1; }; \
	done
	@for f in efi-fdt efi-acpi; do \
	  diff -u $(A64_FACTS)/facts-kernel.txt $(A64_FACTS)/facts-$$f.txt || \
	    { echo "compare-hw: FAILED: the $$f boot took other facts"; exit 1; }; \
	done
	@echo "compare-hw: ok: $$(wc -l < $(A64_FACTS)/facts-kernel.txt | tr -d ' ') facts the same from the device tree, twice, and from ACPI"
	@for f in efi-fdt efi-acpi; do \
	  test -s $(A64_FACTS)/hwdb-facts-$$f.txt || { echo "compare-hw: no hwdb facts from the $$f boot"; exit 1; }; \
	done
	@diff -u $(A64_FACTS)/hwdb-facts-efi-fdt.txt $(A64_FACTS)/hwdb-facts-efi-acpi.txt || \
	  { echo "compare-hw: FAILED: hwdb's drivers bound otherwise through ACPI"; exit 1; }
	@echo "compare-hw: ok: $$(wc -l < $(A64_FACTS)/hwdb-facts-efi-fdt.txt | tr -d ' ') of hwdb's the same from the device tree and through acpid"

# The VQ3 soak (docs/vq3-results.md): a random workload of Areas under
# memory pressure, on each QEMU platform. Slow, so not part of make test.
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

# hwdb's graph (servers/hwdb, docs §11.5), built for the host with the
# sanitizers. hwdb-test runs its rules on made-up graphs and damaged
# blobs, then each captured description against the report it should
# give (docs/boards/*/hwdb*.txt); HWDB_UPDATE=1 rewrites the reports.
HWDB_BUILD := build/hwdb
ACPID_BUILD := build/acpid
# The graph: what the server and the host share (not the server's loop).
HWDB_SRCS := $(filter-out servers/hwdb/host.c servers/hwdb/test.c servers/hwdb/server.c \
	servers/hwdb/killtest.c servers/hwdb/acpid.c servers/hwdb/pci.c servers/hwdb/power.c \
	servers/hwdb/display.c servers/hwdb/intuition.c servers/hwdb/input.c servers/hwdb/gpu.c,\
	$(wildcard servers/hwdb/*.c))
HWDB_HOST := $(HWDB_BUILD)/hwdb-host
HWDB_TEST := $(HWDB_BUILD)/hwdb-test
HWDB_CFLAGS := -std=c23 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -O1 -g \
	-fsanitize=address,undefined -fno-sanitize-recover=all -Iservers/hwdb -Iinclude
CIX_ACPI := $(wildcard docs/boards/cix-p1/acpi/*.dat)
QEMU_ACPI := $(wildcard docs/boards/qemu-virt/a64-acpi/*.dat)

# Each case: the report file, then hwdb-host's source arguments.
HWDB_CASES := \
	docs/boards/qemu-virt/rv64-hwdb.txt@--dtb@docs/boards/qemu-virt/rv64.dtb \
	docs/boards/qemu-virt/a64-hwdb.txt@--dtb@docs/boards/qemu-virt/a64.dtb \
	docs/boards/orangepi-rv2/hwdb.txt@--dtb@docs/boards/orangepi-rv2/u-boot.dtb \
	docs/boards/cix-p1/hwdb-static.txt@--acpi@$(subst $(eval) ,@,$(CIX_ACPI)) \
	docs/boards/cix-p1/hwdb.txt@--namespace@$(ACPID_BUILD)/cix-p1.ns@--acpi@$(subst $(eval) ,@,$(CIX_ACPI)) \
	docs/boards/qemu-virt/a64-acpi-hwdb.txt@--namespace@$(ACPID_BUILD)/qemu-a64.ns@--acpi@$(subst $(eval) ,@,$(QEMU_ACPI))

# acpid-host on each captured set of tables: the report it must give
# (the devices and §9.6's count), then the namespace file the cases
# above add.
ACPID_CASES := \
	docs/boards/qemu-virt/a64-acpid.txt@$(ACPID_BUILD)/qemu-a64.ns@$(subst $(eval) ,@,$(QEMU_ACPI)) \
	docs/boards/cix-p1/acpid.txt@$(ACPID_BUILD)/cix-p1.ns@$(subst $(eval) ,@,$(CIX_ACPI))

$(HWDB_HOST): $(HWDB_SRCS) servers/hwdb/host.c servers/hwdb/hwdb.h include/planar/acpid.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS) $(HWDB_SRCS) servers/hwdb/host.c -o $@

$(HWDB_TEST): $(HWDB_SRCS) servers/hwdb/test.c servers/hwdb/hwdb.h include/planar/acpid.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS) $(HWDB_SRCS) servers/hwdb/test.c -o $@

# acpid on the host (servers/acpid/host.c): the same ACPICA, walk and OS
# layer on captured tables, for the CIX's count (§9.6) and the namespaces
# hwdb-test adds to its graphs.
ACPID_HOST := $(ACPID_BUILD)/acpid-host
ACPID_HOST_CFLAGS := -std=c23 -Wall -Wextra -Wpedantic -Wshadow -O1 -g \
	-fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude
ACPICA_HOST_OBJS := $(patsubst third_party/acpica/source/components/%.c,$(ACPID_BUILD)/acpica/%.o,$(ACPICA_SRCS))
ACPID_HOST_OBJS := $(patsubst servers/acpid/%.c,$(ACPID_BUILD)/%.o,$(ACPID_COMMON) servers/acpid/host.c)

$(ACPID_BUILD)/acpica/%.o: third_party/acpica/source/components/%.c servers/acpid/acplanar.h
	@mkdir -p $(dir $@)
	@$(CROSS_CC) $(ACPICA_CFLAGS) -g -c $< -o $@

$(ACPID_BUILD)/host.o: servers/acpid/host.c servers/acpid/acpid.h include/planar/acpid.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(ACPID_HOST_CFLAGS) -c $< -o $@

$(ACPID_BUILD)/%.o: servers/acpid/%.c servers/acpid/acpid.h servers/acpid/acplanar.h include/planar/acpid.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(ACPID_HOST_CFLAGS) $(ACPICA_INC) -c $< -o $@

$(ACPID_HOST): $(ACPID_HOST_OBJS) $(ACPICA_HOST_OBJS)
	@echo "link $@"
	@$(CROSS_CC) $(ACPID_HOST_CFLAGS) $^ -o $@

check: $(HWDB_HOST)
	@tools/check-portable.sh
	@$(HWDB_HOST) --ties

hwdb-test: $(HWDB_HOST) $(HWDB_TEST) $(ACPID_HOST)
	@$(HWDB_TEST) docs/boards/qemu-virt/a64.dtb $(CIX_ACPI)
	@$(HWDB_TEST) docs/boards/orangepi-rv2/u-boot.dtb $(CIX_ACPI) >/dev/null
	@for c in $(ACPID_CASES); do \
	  set -- $$(echo $$c | tr @ ' '); want=$$1; ns=$$2; shift 2; \
	  $(ACPID_HOST) --out $$ns "$$@" > $$ns.txt 2>/dev/null || { echo "acpid-host: FAILED on $$want's tables"; exit 1; }; \
	  if [ "$(HWDB_UPDATE)" = 1 ]; then cp $$ns.txt $$want; fi; \
	  diff -u $$want $$ns.txt || { echo "hwdb-test: FAILED: $$want"; exit 1; }; \
	  echo "hwdb-test: ok: $$(grep -m1 'devices,' $$want)"; \
	done
	@$(HWDB_TEST) --namespace $(ACPID_BUILD)/cix-p1.ns $(CIX_ACPI)
	@for c in $(HWDB_CASES); do \
	  set -- $$(echo $$c | tr @ ' '); want=$$1; shift; \
	  if [ "$(HWDB_UPDATE)" = 1 ]; then $(HWDB_HOST) --detail "$$@" > $$want || exit 1; fi; \
	  $(HWDB_HOST) --detail "$$@" | diff -u $$want - || \
	    { echo "hwdb-test: FAILED: $$want"; exit 1; }; \
	  echo "hwdb-test: ok: $$(tail -1 $$want)"; \
	done
	@echo "V4.2 done: hwdb's graph on $(words $(HWDB_CASES)) captured descriptions, $(words $(ACPID_CASES)) through acpid"

# The power server's core and backends (drv/power) on the host, with the
# sanitizers: votes and their order, a vote that fails half way, a
# client's going, and each backend against made-up registers (the K1's
# APMU, an SCMI agent on both of the CIX's channels).
POWER_BUILD := build/power
POWER_TEST := $(POWER_BUILD)/power-test
POWER_SRCS := drv/power/core.c drv/power/model.c drv/power/k1.c drv/power/scmi.c drv/power/test.c

$(POWER_TEST): $(POWER_SRCS) drv/power/power.h include/planar/devices/power.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Idrv/power) $(POWER_SRCS) -o $@

power-test: $(POWER_TEST)
	@$(POWER_TEST)

# display's core and backends (drv/display) on the host, with the
# sanitizers: commits latched at the vblank, clipping, a client's going,
# and each DPU backend against made-up registers as the firmware leaves
# them, with hwdb's stop method; fb.c composing in memory.
DISPLAY_BUILD := build/display
DISPLAY_TEST := $(DISPLAY_BUILD)/display-test
DISPLAY_SRCS := drv/display/core.c drv/display/komeda.c drv/display/saturn.c drv/display/fb.c \
	drv/display/test.c

$(DISPLAY_TEST): $(DISPLAY_SRCS) drv/display/display.h drv/display/komeda.h drv/display/saturn.h \
		include/planar/devices/display.h
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Idrv/display) $(DISPLAY_SRCS) -o $@

display-test: $(DISPLAY_TEST)
	@$(DISPLAY_TEST)

# The FAT32 code (servers/dos/fat.c) on the host, with the sanitizers:
# on a disk tools/mkdisk.py makes, it reads what's there, writes and
# removes files and directories, and leaves some for fsck_msdos (macOS's)
# or fsck.fat to check.
DOS_BUILD := build/dos
FAT_HOST := $(DOS_BUILD)/fat-host

$(FAT_HOST): servers/dos/fat.c servers/dos/fat.h servers/dos/host.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(HWDB_CFLAGS:-Iservers/hwdb=-Iservers/dos) servers/dos/fat.c servers/dos/host.c -o $@

dos-test: $(FAT_HOST)
	@python3 -I tools/mkdisk.py $(DOS_BUILD)/test.img
	@$(FAT_HOST) $(DOS_BUILD)/test.img test
	@dd if=$(DOS_BUILD)/test.img of=$(DOS_BUILD)/esp.img bs=512 skip=2048 count=131072 2>/dev/null
	@if command -v fsck_msdos >/dev/null; then fsck_msdos -n $(DOS_BUILD)/esp.img >$(DOS_BUILD)/fsck.txt 2>&1; \
	 elif command -v fsck.fat >/dev/null; then fsck.fat -n $(DOS_BUILD)/esp.img >$(DOS_BUILD)/fsck.txt 2>&1; \
	 else echo "dos-test: no fsck for FAT here: not checked"; exit 0; fi; \
	 st=$$?; cat $(DOS_BUILD)/fsck.txt | grep -v '^\*\*' ; test $$st -eq 0 || { echo "dos-test: FAILED: fsck"; exit 1; }
	@$(FAT_HOST) $(DOS_BUILD)/test.img cat EFI/planar/made/note.txt
	@echo "dos-test: ok"

clean:
	rm -rf build
