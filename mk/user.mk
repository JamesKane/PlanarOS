# Built-in user programs, for the freestanding platforms: the boot
# bundle. Included by mk/arch-<arch>.mk after it sets USER_FLAGS.
#
# - conformance/NAME.c: a test program, one file each.
# - servers/NAME/ and drv/NAME/: a server or a driver, every .c in its
#   directory but host.c and test.c (the host's harnesses). Drivers also
#   link drv.library (lib/drv), and acpid links ACPICA (mk/acpica.mk).
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
USER_RT      := $(BUILD)/user/crt0.o $(BUILD)/user/syscalls.o $(BUILD)/user/rt/string.o \
	$(BUILD)/user/rt/reloc.o $(BUILD)/user/exec/msg.o \
	$(patsubst %,$(BUILD)/user/%.o,$(USER_ARCH_C))
USER_DRV     := $(patsubst lib/drv/%.c,$(BUILD)/user/drv-lib/%.o,$(wildcard lib/drv/*.c))
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

$(BUILD)/user/exec/%.o: lib/exec/%.c
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
		$(USER_RT) arch/$(ARCH)/user/user.ld
	$(CC) $(USER_LDFLAGS) $(USER_RT) $< -o $@

# A program per directory: $(1) its name, $(2) the directory.
define user_dir_prog
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/user/%.o,$$(filter-out %/host.c %/test.c,$$(wildcard $(2)/*.c))) \
	$(if $(filter drv/%,$(2)),$(USER_DRV)) $(if $(filter servers/acpid,$(2)),$(USER_ACPICA))
$(BUILD)/user/$(1).elf: $$($(1)_OBJS) $(USER_RT) arch/$(ARCH)/user/user.ld
	$$(CC) $(USER_LDFLAGS) $(USER_RT) $$($(1)_OBJS) -o $$@
-include $$(patsubst %.o,%.d,$$(filter %.o,$$($(1)_OBJS)))
endef
$(foreach d,$(USER_DIRS),$(eval $(call user_dir_prog,$(notdir $(d)),$(d))))

$(BUILD)/user/%.bin: $(BUILD)/user/%.elf
	$(OBJCOPY) -O binary $< $@

# Every user object's dependencies: programs, runtime, stubs (a call
# renumbered in planar/syscalls.h must rebuild the stubs too).
-include $(USER_RT:.o=.d) $(patsubst %,$(BUILD)/user/%.d,$(USER_CPROGS))

USER_BINS := $(patsubst %,$(BUILD)/user/%.bin,$(USER_PROGS))

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
