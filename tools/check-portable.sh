#!/bin/sh
# Portable code must not test which architecture or platform it is built
# for: that belongs in a compile unit under arch/ or plat/ instead
# (docs §2.6). Exits non-zero and lists the offending lines if any are
# found.

cd "$(dirname "$0")/.." || exit 2

PORTABLE_DIRS="include kernel lib servers drivers conformance"
PATTERN='^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif)[[:space:]].*(__riscv|__APPLE__|__linux__|__x86_64__|__aarch64__|__arm__|__m68k__|HELLFIRE|(^|[^A-Za-z0-9_])(ARCH|PLAT)_)'

hits=$(grep -rnE "$PATTERN" $PORTABLE_DIRS 2>/dev/null)
if [ -n "$hits" ]; then
    echo "check-portable: architecture or platform conditionals in portable code:"
    echo "$hits"
    exit 1
fi
echo "check-portable: ok"
