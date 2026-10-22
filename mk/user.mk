# Built-in user programs, for the freestanding platforms: the boot
# bundle. Included by mk/arch-<arch>.mk after it sets USER_FLAGS.
#
# - conformance/NAME.c: a test program, one file each; blk* ones link
#   the block class's library (lib/blk), dos* ones dos.library (lib/dos).
# - servers/NAME/ and drv/NAME/: a server or a driver, every .c in its
#   directory but host.c and test.c (the host's harnesses). Drivers also
#   link drv.library (lib/drv), acpid links ACPICA (mk/acpica.mk), and
#   hwdb the block class's library (lib/blk) for its kill test.
#
# Each is linked at 0 as a position-independent executable with
# arch/$(ARCH)/user/user.ld (it relocates itself: lib/rt/reloc.c) and
# flattened; the generated user_images.S embeds them all and lists them
# by name for kernel/userimg.h.

USER_CPROGS  := $(basename $(notdir $(wildcard conformance/*.c)))
USER_DIRS    := $(sort $(patsubst %/,%,$(dir $(wildcard servers/*/*.c drv/*/*.c))))
USER_PROGS   := $(USER_CPROGS) $(notdir $(USER_DIRS))
USER_CFLAGS  := -std=c23 -Wall -Wextra -O2 $(USER_FLAGS) -ffreestanding -fno-builtin \
	-fno-stack-protector -fPIE -Iinclude -Iconformance
USER_LDFLAGS := $(USER_FLAGS) -nostdlib -static-pie --ld-path=$(LD_PATH) \
	-Wl,-T,arch/$(ARCH)/user/user.ld -Wl,--no-dynamic-linker
USER_ARCH_C  := $(basename $(notdir $(wildcard arch/$(ARCH)/user/*.c)))
USER_RT      := $(BUILD)/user/crt0.o $(BUILD)/user/syscalls.o $(BUILD)/user/string.o \
	$(BUILD)/user/rt/string.o $(BUILD)/user/rt/reloc.o $(BUILD)/user/rt/tls.o \
	$(BUILD)/user/exec/msg.o $(BUILD)/user/exec/task.o \
	$(patsubst %,$(BUILD)/user/%.o,$(USER_ARCH_C))
# exec.library's pools, AllocVec and RawDoFmt (lib/exec): an archive
# after the program and its libraries, so only a program that calls
# them carries them.
USER_EXECLIB := $(BUILD)/user/libexec.a
EXECLIB_OBJS := $(BUILD)/user/exec/pool.o $(BUILD)/user/exec/fmt.o
# compiler-rt's 128-bit builtins (third_party/compiler-rt): an archive,
# last on every link line, so a program takes only what it calls.
USER_BUILTINS := $(BUILD)/user/libcompiler-rt.a
BUILTINS_OBJS := $(patsubst third_party/compiler-rt/lib/builtins/%.c,$(BUILD)/user/compiler-rt/%.o,\
	$(wildcard third_party/compiler-rt/lib/builtins/*.c))
USER_DRV     := $(patsubst lib/drv/%.c,$(BUILD)/user/drv-lib/%.o,$(wildcard lib/drv/*.c))
USER_BLK     := $(BUILD)/user/blk/blk.o
USER_DOS     := $(BUILD)/user/dos/dos.o
USER_ACPICA  := $(BUILD)/user/libacpica.a
ACPICA_OBJS  := $(patsubst third_party/acpica/source/components/%.c,$(BUILD)/user/acpica/%.o,$(ACPICA_SRCS))

$(BUILD)/user/%.o: arch/$(ARCH)/user/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.o: arch/$(ARCH)/user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/rt/%.o: lib/rt/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/compiler-rt/%.o: third_party/compiler-rt/lib/builtins/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_FLAGS) -std=c23 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fPIE \
	  -MMD -MP -c $< -o $@

$(USER_EXECLIB): $(EXECLIB_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(USER_BUILTINS): $(BUILTINS_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/user/exec/%.o: lib/exec/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/dos/%.o: lib/dos/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/blk/%.o: lib/blk/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/drv-lib/%.o: lib/drv/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

# ACPICA, and acpid's files, which see its headers.
$(BUILD)/user/acpica/%.o: third_party/acpica/source/components/%.c servers/acpid/acplanar.h
	@mkdir -p $(dir $@)
	@$(CC) $(ACPICA_CFLAGS) $(USER_FLAGS) -ffreestanding -fno-builtin -fno-stack-protector -fPIE \
	  -c $< -o $@

$(USER_ACPICA): $(ACPICA_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/user/servers/acpid/%.o: USER_CFLAGS += $(ACPICA_INC)

$(BUILD)/user/servers/%.o: servers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/drv/%.o: drv/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.o: conformance/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(patsubst %,$(BUILD)/user/%.elf,$(USER_CPROGS)): $(BUILD)/user/%.elf: $(BUILD)/user/%.o \
		$(USER_RT) $(USER_BLK) $(USER_DOS) $(USER_EXECLIB) $(USER_BUILTINS) arch/$(ARCH)/user/user.ld
	$(CC) $(USER_LDFLAGS) $(USER_RT) $< $(if $(filter blk%,$*),$(USER_BLK)) \
	  $(if $(filter dos%,$*),$(USER_DOS)) $(USER_EXECLIB) $(USER_BUILTINS) -o $@
	$(check_tls)

# thread_local's segment, if a program has one, must be aligned to the
# 64 bytes lib/rt/tls.c and crt0 build each task's block for: a variable
# aligned to more would move the variables' offsets from the thread
# pointer on Arm.
check_tls = @$(READELF) -lW $@ | awk '$$1 == "TLS" && $$NF != "0x40" { exit 1 }' || \
	{ echo "$@: thread_local aligned to more than 64 bytes"; rm -f $@; exit 1; }

# A program per directory: $(1) its name, $(2) the directory.
define user_dir_prog
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/user/%.o,$$(filter-out %/host.c %/test.c,$$(wildcard $(2)/*.c))) \
	$(if $(filter drv/%,$(2)),$(USER_DRV)) $(if $(filter servers/acpid,$(2)),$(USER_ACPICA)) \
	$(if $(filter servers/hwdb servers/dos,$(2)),$(USER_BLK)) \
	$(if $(filter servers/hwdb drv/gpu-panthor drv/gpu-rogue,$(2)),$(USER_DOS))
$(BUILD)/user/$(1).elf: $$($(1)_OBJS) $(USER_RT) $(USER_EXECLIB) $(USER_BUILTINS) arch/$(ARCH)/user/user.ld
	$$(CC) $(USER_LDFLAGS) $(USER_RT) $$($(1)_OBJS) $(USER_EXECLIB) $(USER_BUILTINS) -o $$@
	$$(check_tls)
-include $$(patsubst %.o,%.d,$$(filter %.o,$$($(1)_OBJS)))
endef
$(foreach d,$(USER_DIRS),$(eval $(call user_dir_prog,$(notdir $(d)),$(d))))

$(BUILD)/user/%.bin: $(BUILD)/user/%.elf
	$(OBJCOPY) -O binary $< $@

# Every user object's dependencies: programs, runtime, stubs (a call
# renumbered in planar/syscalls.h must rebuild the stubs too).
-include $(USER_RT:.o=.d) $(EXECLIB_OBJS:.o=.d) $(patsubst %,$(BUILD)/user/%.d,$(USER_CPROGS))

USER_BINS := $(patsubst %,$(BUILD)/user/%.bin,$(USER_PROGS))

# Commands (cmd/NAME.c): programs that live on storage, in C: (step 6),
# not in the image. Each links dos.library, and the build or the board's
# loader puts its .elf on the disk.
CMD_PROGS := $(basename $(notdir $(wildcard cmd/*.c)))
CMD_ELFS  := $(patsubst %,$(BUILD)/user/cmd/%.elf,$(CMD_PROGS))

$(BUILD)/user/cmd/%.o: cmd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/cmd/%.elf: $(BUILD)/user/cmd/%.o $(USER_RT) $(USER_DOS) $(USER_EXECLIB) $(USER_BUILTINS) \
		arch/$(ARCH)/user/user.ld
	$(CC) $(USER_LDFLAGS) $(USER_RT) $< $(USER_DOS) $(USER_EXECLIB) $(USER_BUILTINS) -o $@
	$(check_tls)

-include $(patsubst %,$(BUILD)/user/cmd/%.d,$(CMD_PROGS))

$(BUILD)/user_images.S: $(USER_BINS) mk/user.mk
	@{ echo '    .section .rodata'; \
	   echo '    .balign 8'; \
	   echo '    .globl user_images'; \
	   echo 'user_images:'; \
	   for p in $(subst -,_,$(USER_PROGS)); do echo "    .quad name_$$p, start_$$p, end_$$p"; done; \
	   echo '    .quad 0, 0, 0'; \
	   for p in $(USER_PROGS); do l=$$(echo $$p | tr - _); \
	     echo "name_$$l: .asciz \"$$p\""; \
	     echo '    .balign 8'; \
	     echo "start_$$l: .incbin \"$(BUILD)/user/$$p.bin\""; \
	     echo "end_$$l:"; \
	   done; } > $@

$(BUILD)/user_images.o: $(BUILD)/user_images.S
	$(CC) $(ASFLAGS) -c $< -o $@

ARCH_GEN_OBJS += $(BUILD)/user_images.o
