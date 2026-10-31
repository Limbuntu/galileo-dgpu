#!/bin/sh
# Builds an 8 MB coreboot 4.7 flash image for the Intel Galileo Gen 2 with a
# SeaBIOS payload. Runs inside the toolchain image built from ./Dockerfile.
#
# The coreboot source is the official 4.7 release tarball: it bundles
# 3rdparty/vboot, which cbfstool needs, and it needs no git at build time.
set -e

TARBALL="${COREBOOT_TARBALL:-/assets/coreboot-4.7.tar.xz}"

if [ ! -f "$TARBALL" ]; then
	echo "no tarball at $TARBALL, downloading from coreboot.org (slow)"
	curl -fsSL https://coreboot.org/releases/coreboot-4.7.tar.xz -o /tmp/coreboot-4.7.tar.xz
	TARBALL=/tmp/coreboot-4.7.tar.xz
fi

work=/work
rm -rf "$work"
mkdir -p "$work"
tar xf "$TARBALL" -C "$work"
cd "$work/coreboot-4.7"

# coreboot 4.7 appends -Werror unconditionally; gcc 12 turned several harmless
# diagnostics into build failures. Keep the warnings, drop the hard failure.
sed -i 's|^CFLAGS_common += -Werror$|CFLAGS_common += -Werror -Wno-error|' Makefile.inc

# gcc 10+ defaults to -fno-common; coreboot 4.7's sconfig relies on common
# symbols ("multiple definition of `head`"). Compile the old way.
sed -i 's|^CFLAGS_common += -Werror -Wno-error$|CFLAGS_common += -Werror -Wno-error|' Makefile.inc
sed -i 's|^SCONFIGFLAGS += |SCONFIGFLAGS += -fcommon |' util/sconfig/Makefile.inc

# LDFLAGS_common carries gcc-driver flags straight to ld; binutils >= 2.39
# rejects the short form with "unable to disambiguate: -nostartfiles".
sed -i 's| -nostartfiles||' Makefile.inc

# gcc 12 flags a false positive in the Quark storage test (its struct pointer is
# used before storage_media is swapped in) and the ROM build treats it as an error.
sed -i '1i #pragma GCC diagnostic ignored "-Warray-bounds"' src/soc/intel/quark/storage_test.c

# Galileo's console is HSUART1 at 0xa0019000, but coreboot only defines
# CONFIG_CONSOLE_UART_BASE_ADDRESS for Apollolake/Denverton/AMD, so the SeaBIOS
# payload rule would write an empty DEBUG_SERIAL_MEM_ADDRESS and SeaBIOS kconfig
# rejects it.
sed -i 's|CONFIG_DEBUG_SERIAL_MEM_ADDRESS=$(CONFIG_CONSOLE_UART_BASE_ADDRESS)|CONFIG_DEBUG_SERIAL_MEM_ADDRESS=0xa0019000|g' \
	payloads/external/SeaBIOS/Makefile

printf '%s\n' \
	CONFIG_VENDOR_INTEL=y \
	CONFIG_BOARD_INTEL_GALILEO=y \
	CONFIG_GALILEO_GEN2=y \
	CONFIG_PAYLOAD_SEABIOS=y \
	CONFIG_SEABIOS_REVISION=y \
	CONFIG_SEABIOS_REVISION_ID=rel-1.16.3 \
	CONFIG_ANY_TOOLCHAIN=y > .config
make olddefconfig

echo "=== selected config ==="
grep -E '^CONFIG_(BOARD_INTEL_GALILEO|GALILEO_GEN2|PAYLOAD_SEABIOS|ROM_SIZE|CBFS_SIZE|SEABIOS_STABLE)=' .config

echo "=== build with $(make --version | head -1) ==="
make -j"$(nproc)"

test -s build/coreboot.rom
mkdir -p /out
cp build/coreboot.rom /out/coreboot.rom

echo "=== result ==="
ls -l /out/coreboot.rom
md5sum /out/coreboot.rom
