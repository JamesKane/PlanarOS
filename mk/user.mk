# Built-in user programs, for the freestanding platforms: the boot
# bundle. Included by mk/arch-<arch>.mk after it sets USER_FLAGS.
#
# - conformance/NAME.c: a test program, one file each; blk* ones link
#   the block class's library (lib/blk), dos* ones and window dos.library (lib/dos),
#   clib* ones C's library (lib/clib), with its headers.
# - servers/NAME/ and drv/NAME/: a server or a driver, every .c in its
#   directory but host.c and test.c (the host's harnesses). Drivers also
#   link drv.library (lib/drv), acpid links ACPICA (mk/acpica.mk) and
#   C's library (lib/clib), and hwdb the block class's library (lib/blk)
#   for its kill test.
#
# Each is linked at 0 as a position-independent executable with
# arch/$(ARCH)/user/user.ld (it relocates itself: lib/rt/reloc.c) and
# flattened; the generated user_images.S embeds them all and lists them
# by name for kernel/userimg.h.

USER_CPROGS  := $(basename $(notdir $(wildcard conformance/*.c)))
USER_DIRS    := $(sort $(patsubst %/,%,$(dir $(wildcard servers/*/*.c drv/*/*.c))))
USER_PROGS   := $(USER_CPROGS) $(notdir $(USER_DIRS))
USER_CFLAGS  := -std=c23 -Wall -Wextra -O2 $(USER_FLAGS) -ffreestanding -fno-builtin \
	-fno-stack-protector -fPIE $(FRAME_CFLAGS) -Iinclude -Iconformance
# Every image carries a 16-byte build-id, as planar-ld's do (docs §19.4).
USER_LDFLAGS := $(USER_FLAGS) -nostdlib -static-pie --ld-path=$(LD_PATH) \
	-Wl,-T,arch/$(ARCH)/user/user.ld -Wl,--no-dynamic-linker -Wl,--build-id=md5
USER_ARCH_C  := $(basename $(notdir $(wildcard arch/$(ARCH)/user/*.c)))
# code.c's kernel-call test sets every callee-saved register, RV64's s0
# (the frame pointer) among them: it can't keep a frame record.
$(BUILD)/user/code.o: USER_CFLAGS += -fomit-frame-pointer
USER_RT      := $(BUILD)/user/crt0.o $(BUILD)/user/syscalls.o $(BUILD)/user/string.o \
	$(BUILD)/user/rt/string.o $(BUILD)/user/rt/reloc.o $(BUILD)/user/rt/tls.o \
	$(BUILD)/user/exec/msg.o $(BUILD)/user/exec/task.o \
	$(patsubst %,$(BUILD)/user/%.o,$(USER_ARCH_C))
# exec.library's pools, AllocVec and RawDoFmt (lib/exec): an archive
# after the program and its libraries, so only a program that calls
# them carries them.
USER_EXECLIB := $(BUILD)/user/libexec.a
EXECLIB_OBJS := $(BUILD)/user/exec/pool.o $(BUILD)/user/exec/fmt.o $(BUILD)/user/exec/report.o
# utility.library (lib/utility) the same way, before libexec.a, which
# its SNPrintf calls.
USER_UTILLIB := $(BUILD)/user/libutility.a
UTILLIB_OBJS := $(patsubst lib/utility/%.c,$(BUILD)/user/utility/%.o,\
	$(filter-out %/test.c,$(wildcard lib/utility/*.c)))
# gfx.library's own code (lib/gfx, and the vector unit's kernels) the
# same way: display's console takes the 8x8 font from it, and the tests
# its scalar entries, to hold the library's against.
USER_GFXLIB := $(BUILD)/user/libgfx.a
GFXLIB_OBJS := $(patsubst lib/gfx/%.c,$(BUILD)/user/gfx/%.o,$(filter-out %/test.c,$(wildcard lib/gfx/*.c))) \
	$(BUILD)/user/gfx/vector.o
# lib/clib, C's library, for a program that asks for it: acpid. Its
# headers are include/clib, which nothing else sees.
USER_CLIB := $(BUILD)/user/libclib.a
CLIB_OBJS := $(patsubst lib/clib/%.c,$(BUILD)/user/clib/%.o,$(filter-out %/test.c %/testhost.c,$(wildcard lib/clib/*.c)))
# compiler-rt's 128-bit builtins (third_party/compiler-rt): an archive,
# last on every link line, so a program takes only what it calls.
USER_BUILTINS := $(BUILD)/user/libcompiler-rt.a
BUILTINS_OBJS := $(patsubst third_party/compiler-rt/lib/builtins/%.c,$(BUILD)/user/compiler-rt/%.o,\
	$(wildcard third_party/compiler-rt/lib/builtins/*.c))
USER_DRV     := $(patsubst lib/drv/%.c,$(BUILD)/user/drv-lib/%.o,$(wildcard lib/drv/*.c))
USER_BLK     := $(BUILD)/user/blk/blk.o
# dos.library (lib/dos): an archive, before libutility.a and libexec.a,
# which its ReadArgs and Printf call.
USER_DOS     := $(BUILD)/user/libdos.a
DOS_OBJS     := $(patsubst lib/dos/%.c,$(BUILD)/user/dos/%.o,$(filter-out %/test.c,$(wildcard lib/dos/*.c)))
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

$(USER_CLIB): $(CLIB_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(USER_DOS): $(DOS_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(USER_UTILLIB): $(UTILLIB_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(USER_GFXLIB): $(GFXLIB_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(USER_BUILTINS): $(BUILTINS_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/user/clib/%.o: lib/clib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iinclude/clib -MMD -MP -c $< -o $@

$(BUILD)/user/exec/%.o: lib/exec/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/utility/%.o: lib/utility/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/gfx/%.o: lib/gfx/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/gfx/vector.o: arch/$(ARCH)/lib/gfx.c
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
	@$(CC) $(ACPICA_CFLAGS) -Iinclude/clib $(USER_FLAGS) -ffreestanding -fno-builtin -fno-stack-protector -fPIE \
	  -c $< -o $@

$(USER_ACPICA): $(ACPICA_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/user/servers/acpid/%.o: USER_CFLAGS += $(ACPICA_INC) -Iinclude/clib

$(BUILD)/user/servers/%.o: servers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/drv/%.o: drv/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/clib.o $(BUILD)/user/clib%.o: USER_CFLAGS += -Iinclude/clib

$(BUILD)/user/%.o: conformance/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(patsubst %,$(BUILD)/user/%.elf,$(USER_CPROGS)): $(BUILD)/user/%.elf: $(BUILD)/user/%.o \
		$(USER_RT) $(USER_BLK) $(USER_DOS) $(USER_CLIB) $(USER_GFXLIB) $(USER_UTILLIB) $(USER_EXECLIB) \
		$(USER_BUILTINS) arch/$(ARCH)/user/user.ld
	$(CC) $(USER_LDFLAGS) $(USER_RT) $< $(if $(filter blk%,$*),$(USER_BLK)) \
	  $(if $(filter dos% window,$*),$(USER_DOS)) $(if $(filter clib clib%,$*),$(USER_CLIB)) \
	  $(USER_GFXLIB) $(USER_UTILLIB) $(USER_EXECLIB) $(USER_BUILTINS) -o $@
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
	$(if $(filter drv/%,$(2)),$(USER_DRV)) $(if $(filter servers/acpid,$(2)),$(USER_ACPICA) $(USER_CLIB)) \
	$(if $(filter servers/hwdb servers/dos,$(2)),$(USER_BLK)) \
	$(if $(filter servers/hwdb servers/conwin servers/input servers/intuition servers/workbench servers/prefs \
	  drv/gpu-panthor drv/gpu-rogue,$(2)),$(USER_DOS))
$(BUILD)/user/$(1).elf: $$($(1)_OBJS) $(USER_RT) $(USER_GFXLIB) $(USER_UTILLIB) $(USER_EXECLIB) $(USER_BUILTINS) \
		arch/$(ARCH)/user/user.ld
	$$(CC) $(USER_LDFLAGS) $(USER_RT) $$($(1)_OBJS) $(USER_GFXLIB) $(USER_UTILLIB) $(USER_EXECLIB) $(USER_BUILTINS) -o $$@
	$$(check_tls)
-include $$(patsubst %.o,%.d,$$(filter %.o,$$($(1)_OBJS)))
endef
$(foreach d,$(USER_DIRS),$(eval $(call user_dir_prog,$(notdir $(d)),$(d))))

$(BUILD)/user/%.bin: $(BUILD)/user/%.elf
	$(OBJCOPY) -O binary $< $@

# Every user object's dependencies: programs, runtime, stubs (a call
# renumbered in planar/syscalls.h must rebuild the stubs too).
-include $(USER_RT:.o=.d) $(EXECLIB_OBJS:.o=.d) $(UTILLIB_OBJS:.o=.d) $(GFXLIB_OBJS:.o=.d) $(CLIB_OBJS:.o=.d) $(patsubst %,$(BUILD)/user/%.d,$(USER_CPROGS))

USER_BINS := $(patsubst %,$(BUILD)/user/%.bin,$(USER_PROGS))

# Shared libraries (libs/NAME, V6 step 4): each one image of code and
# constants, linked by lib.ld, which refuses data, and relocated once at
# boot into the shared library region (kernel/library.c). Built for the
# ISA's base, so a RISC-V hart without V runs it: the vector unit's
# memory functions are string.S's, renamed, which the kernel picks at
# boot where the hart has V.
USER_LIBS := exec utility gfx intuition font ui
LIB_CFLAGS := $(subst rv64gcv,rv64gc,$(USER_CFLAGS))
LIB_LDFLAGS := $(subst /user.ld,/lib.ld,$(USER_LDFLAGS)) -Wl,-z,norelro
LIB_exec_SRCS := libs/exec/base.c lib/exec/pool.c lib/exec/fmt.c lib/exec/task.c \
	arch/$(ARCH)/user/procdata.c \
	lib/rt/string.c arch/$(ARCH)/user/syscalls.S
LIB_utility_SRCS := libs/utility/base.c $(filter-out %/test.c,$(wildcard lib/utility/*.c)) \
	lib/exec/fmt.c lib/rt/string.c
# gfx.library's: lib/gfx, the stubs for its Areas, the process data's
# reader (it opens font.library by the program's IExec), and the vector
# unit's kernels, built for it (V on RISC-V), which only a hart with it runs.
LIB_gfx_SRCS := libs/gfx/base.c $(filter-out %/test.c,$(wildcard lib/gfx/*.c)) \
	arch/$(ARCH)/lib/gfx.c lib/rt/string.c arch/$(ARCH)/user/syscalls.S arch/$(ARCH)/user/procdata.c
$(BUILD)/libs/arch/$(ARCH)/lib/gfx.o: LIB_CFLAGS := $(USER_CFLAGS)
# intuition.library's: lib/intuition, and what it calls linked in: the
# messages (lib/exec/msg.c), tag lists (lib/utility/tags.c) and the stubs.
LIB_intuition_SRCS := libs/intuition/base.c lib/intuition/intuition.c lib/exec/msg.c \
	lib/utility/tags.c lib/rt/string.c arch/$(ARCH)/user/syscalls.S
# font.library's: lib/font, its fonts embedded (lib/font/fonts), and
# utility's checked readers linked in.
LIB_font_SRCS := libs/font/base.c $(filter-out %/test.c,$(wildcard lib/font/*.c)) lib/utility/span.c \
	lib/rt/string.c
$(BUILD)/libs/lib/font/fonts.o: $(wildcard lib/font/fonts/*.ttf)
# ui.library's: lib/ui, with utility's tag lists linked in. It draws
# through the caller's gfx.library.
LIB_ui_SRCS := libs/ui/base.c lib/ui/ui.c lib/utility/tags.c lib/rt/string.c
LIB_VEC := $(BUILD)/libs/vec-string.o
LIB_HEAD := $(BUILD)/libs/lib/libhead.o
LIB_BINS := $(patsubst %,$(BUILD)/libs/%.library.bin,$(USER_LIBS))

$(BUILD)/libs/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/libs/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -MMD -MP -c $< -o $@

$(LIB_VEC): arch/$(ARCH)/user/string.S
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -Dmemcpy=vec_memcpy -Dmemmove=vec_memmove -Dmemset=vec_memset \
	  -MMD -MP -c $< -o $@

# A library: $(1) its name.
define user_lib
$(1)_LIB_OBJS := $(LIB_HEAD) $$(patsubst %,$(BUILD)/libs/%.o,$$(basename $$(LIB_$(1)_SRCS))) \
	$(if $(filter exec,$(1)),$(LIB_VEC))
$(BUILD)/libs/$(1).library.elf: $$($(1)_LIB_OBJS) $(USER_BUILTINS) arch/$(ARCH)/user/lib.ld
	$$(CC) $(LIB_LDFLAGS) $$($(1)_LIB_OBJS) $(USER_BUILTINS) -o $$@
-include $$(patsubst %.o,%.d,$$($(1)_LIB_OBJS))
endef
$(foreach l,$(USER_LIBS),$(eval $(call user_lib,$(l))))

$(BUILD)/libs/%.bin: $(BUILD)/libs/%.elf
	$(OBJCOPY) -O binary $< $@

# Commands (cmd/NAME.c): programs that live on storage, in C: (step 6),
# not in the image. Each links dos.library, and the build or the board's
# loader puts its .elf on the disk.
CMD_PROGS := $(basename $(notdir $(wildcard cmd/*.c)))
CMD_ELFS  := $(patsubst %,$(BUILD)/user/cmd/%.elf,$(CMD_PROGS))

$(BUILD)/user/cmd/%.o: cmd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/cmd/%.elf: $(BUILD)/user/cmd/%.o $(USER_RT) $(USER_DOS) $(USER_UTILLIB) $(USER_EXECLIB) \
		$(USER_BUILTINS) arch/$(ARCH)/user/user.ld
	$(CC) $(USER_LDFLAGS) $(USER_RT) $< $(USER_DOS) $(USER_UTILLIB) $(USER_EXECLIB) $(USER_BUILTINS) -o $@
	$(check_tls)

-include $(patsubst %,$(BUILD)/user/cmd/%.d,$(CMD_PROGS))

# Programs, then the libraries (NAME.library), which the kernel tells
# apart by their magic.
$(BUILD)/user_images.S: $(USER_BINS) $(LIB_BINS) mk/user.mk
	@{ echo '    .section .rodata'; \
	   echo '    .balign 8'; \
	   echo '    .globl user_images'; \
	   echo 'user_images:'; \
	   for p in $(subst -,_,$(USER_PROGS)) $(patsubst %,lib_%,$(USER_LIBS)); do \
	     echo "    .quad name_$$p, start_$$p, end_$$p"; done; \
	   echo '    .quad 0, 0, 0'; \
	   for p in $(USER_PROGS); do l=$$(echo $$p | tr - _); \
	     echo "name_$$l: .asciz \"$$p\""; \
	     echo '    .balign 8'; \
	     echo "start_$$l: .incbin \"$(BUILD)/user/$$p.bin\""; \
	     echo "end_$$l:"; \
	   done; \
	   for p in $(USER_LIBS); do \
	     echo "name_lib_$$p: .asciz \"$$p.library\""; \
	     echo '    .balign 8'; \
	     echo "start_lib_$$p: .incbin \"$(BUILD)/libs/$$p.library.bin\""; \
	     echo "end_lib_$$p:"; \
	   done; } > $@

$(BUILD)/user_images.o: $(BUILD)/user_images.S
	$(CC) $(ASFLAGS) -c $< -o $@

ARCH_GEN_OBJS += $(BUILD)/user_images.o
