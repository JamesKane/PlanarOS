# ACPICA (third_party/acpica: upstream's source/include and nine of its
# component directories, unchanged) for acpid, on Planar and the host.
# servers/acpid/acplanar.h, force-included, stands in for its acenv.h.
# -std=gnu11, since C23's keywords are names in ACPICA; its warnings are
# its own business (-w). The debugger's rsdump.c isn't built.

ACPICA_DIRS := dispatcher events executer hardware namespace parser resources tables utilities
ACPICA_SRCS := $(filter-out %/rsdump.c,$(wildcard $(patsubst %,third_party/acpica/source/components/%/*.c,$(ACPICA_DIRS))))
ACPICA_INC  := -include servers/acpid/acplanar.h \
	-isystem third_party/acpica/source/include -isystem third_party/acpica/source/include/platform
ACPICA_CFLAGS := -std=gnu11 -O2 -fno-strict-aliasing -w $(ACPICA_INC)

# acpid's own files that are the same on both sides.
ACPID_COMMON := servers/acpid/walk.c servers/acpid/osl.c
