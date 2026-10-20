# Hosted build: the host process stands in for the machine. Uses the
# pinned clang, targeting the host.

ARCH := host
CC   := $(CROSS_CC)

# ucontext stands in for the context switch; macOS wants these to show it.
CPPFLAGS += -D_XOPEN_SOURCE=600 -D_DARWIN_C_SOURCE
CFLAGS += -Wno-deprecated-declarations

ARCH_SRCS := \
	arch/host/start.c \
	arch/host/arch.c

IMAGE := $(BUILD)/planar

$(IMAGE): $$(OBJS)
	$(CC) $(OBJS) -o $@
