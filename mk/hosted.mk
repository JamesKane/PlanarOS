# Hosted build: a host executable. Kernel logic runs as a process for fast
# iteration and sanitizers (docs §6.4).

include mk/arch-host.mk

PLAT_SRCS := \
	plat/hosted/plat.c

RUN_CMD := $(IMAGE)

run: $(IMAGE)
	$(RUN_CMD)
