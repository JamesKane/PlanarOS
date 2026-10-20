# Built-in user programs (conformance/), for the freestanding platforms.
# Included by mk/arch-<arch>.mk after it sets USER_FLAGS. Each program is
# linked at 0 with arch/$(ARCH)/user/user.ld and flattened; the generated
# user_images.S embeds them all and lists them for kernel/userimg.h.

USER_PROGS   := $(basename $(notdir $(wildcard conformance/*.c)))
USER_CFLAGS  := -std=c23 -Wall -Wextra -O2 $(USER_FLAGS) -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic -Iinclude -Iconformance
USER_LDFLAGS := $(USER_FLAGS) -nostdlib -static --ld-path=$(LD_PATH) \
	-Wl,-T,arch/$(ARCH)/user/user.ld -Wl,--no-dynamic-linker
USER_ARCH_C  := $(basename $(notdir $(wildcard arch/$(ARCH)/user/*.c)))
USER_RT      := $(BUILD)/user/crt0.o $(BUILD)/user/syscalls.o $(BUILD)/user/rt/string.o \
	$(BUILD)/user/exec/msg.o $(patsubst %,$(BUILD)/user/%.o,$(USER_ARCH_C))

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

$(BUILD)/user/%.o: conformance/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.elf: $(BUILD)/user/%.o $(USER_RT) arch/$(ARCH)/user/user.ld
	$(CC) $(USER_LDFLAGS) $(USER_RT) $< -o $@

$(BUILD)/user/%.bin: $(BUILD)/user/%.elf
	$(OBJCOPY) -O binary $< $@

# Every user object's dependencies: programs, runtime, stubs (a call
# renumbered in planar/syscalls.h must rebuild the stubs too).
-include $(USER_RT:.o=.d) $(patsubst %,$(BUILD)/user/%.d,$(USER_PROGS))

USER_BINS := $(patsubst %,$(BUILD)/user/%.bin,$(USER_PROGS))

$(BUILD)/user_images.S: $(USER_BINS) mk/user.mk
	@{ echo '    .section .rodata'; \
	   echo '    .balign 8'; \
	   echo '    .globl user_images'; \
	   echo 'user_images:'; \
	   for p in $(USER_PROGS); do echo "    .quad name_$$p, start_$$p, end_$$p"; done; \
	   echo '    .quad 0, 0, 0'; \
	   for p in $(USER_PROGS); do \
	     echo "name_$$p: .asciz \"$$p\""; \
	     echo '    .balign 8'; \
	     echo "start_$$p: .incbin \"$(BUILD)/user/$$p.bin\""; \
	     echo "end_$$p:"; \
	   done; } > $@

$(BUILD)/user_images.o: $(BUILD)/user_images.S
	$(CC) $(ASFLAGS) -c $< -o $@

ARCH_GEN_OBJS += $(BUILD)/user_images.o
