# Hosted build: the host process stands in for the machine. Uses the
# pinned clang, targeting the host.

ARCH := host
CC   := $(CROSS_CC)

ARCH_SRCS := \
	arch/host/start.c \
	arch/host/arch.c

IMAGE := $(BUILD)/planar

$(IMAGE): $$(OBJS)
	$(CC) $(OBJS) -o $@
