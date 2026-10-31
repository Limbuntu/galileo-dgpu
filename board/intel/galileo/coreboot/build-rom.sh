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

printf '%s\n' \
	CONFIG_VENDOR_INTEL=y \
	CONFIG_BOARD_INTEL_GALILEO=y \
	CONFIG_GALILEO_GEN2=y \
	CONFIG_PAYLOAD_SEABIOS=y \
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
