#!/bin/sh
# Builds coreboot 4.7 (Galileo Gen 2, SeaBIOS payload) from the official release
# tarball and drops the 8 MB flash image in /out/coreboot.rom.
set -e

cd /src/coreboot-4.7

# Real-mode BIOS on the Quark: SeaBIOS is the payload, and the board's single
# core rules out anything that needs a spare CPU. gcc from the distro is fine
# for building the ROM itself.
printf '%s\n' \
	CONFIG_VENDOR_INTEL=y \
	CONFIG_BOARD_INTEL_GALILEO=y \
	CONFIG_GALILEO_GEN2=y \
	CONFIG_PAYLOAD_SEABIOS=y \
	CONFIG_ANY_TOOLCHAIN=y > .config
make olddefconfig

echo "=== selected config ==="
grep -E '^CONFIG_(BOARD_INTEL_GALILEO|GALILEO_GEN2|PAYLOAD_SEABIOS|ROM_SIZE|CBFS_SIZE|SEABIOS_STABLE)=' .config

echo "=== build ($(make --version | head -1)) ==="
make -j"$(nproc)"

test -s build/coreboot.rom
mkdir -p /out
cp build/coreboot.rom /out/coreboot.rom

echo "=== result ==="
ls -l /out/coreboot.rom
md5sum /out/coreboot.rom
