# The pinned toolchain (docs/07-language-toolchain.md §7.3): Homebrew's
# llvm@22 and lld@22. Swiftly's and Xcode's clang come first on many
# PATHs, so every tool is named by absolute path. Override LLVM_BIN and
# LLD_BIN for another install of the same major version.

LLVM_MAJOR := 22
LLVM_BIN   ?= /opt/homebrew/opt/llvm@$(LLVM_MAJOR)/bin
LLD_BIN    ?= /opt/homebrew/opt/lld@$(LLVM_MAJOR)/bin

CROSS_CC := $(LLVM_BIN)/clang
OBJCOPY  := $(LLVM_BIN)/llvm-objcopy
READELF  := $(LLVM_BIN)/llvm-readelf
LD_PATH  := $(LLD_BIN)/ld.lld

ifeq ($(wildcard $(CROSS_CC)),)
$(error no clang at $(CROSS_CC): brew install llvm@$(LLVM_MAJOR), or set LLVM_BIN)
endif
ifeq ($(wildcard $(LD_PATH)),)
$(error no ld.lld at $(LD_PATH): brew install lld@$(LLVM_MAJOR), or set LLD_BIN)
endif

HAVE_MAJOR := $(shell $(CROSS_CC) -dumpversion | cut -d. -f1)
ifneq ($(HAVE_MAJOR),$(LLVM_MAJOR))
$(error $(CROSS_CC) is LLVM $(HAVE_MAJOR), the tree is pinned to $(LLVM_MAJOR))
endif

# Flags every freestanding (non-hosted) build shares.
FREESTANDING_CFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -fno-pic \
	-nostdlib
FREESTANDING_LDFLAGS := -nostdlib -static --ld-path=$(LD_PATH) -Wl,--no-dynamic-linker
