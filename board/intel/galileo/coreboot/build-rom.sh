#!/bin/sh
# Builds coreboot 4.7 for the Intel Galileo Gen 2 with a SeaBIOS payload.
# Output: /out/coreboot.rom (8 MB)
set -e

cd /src/coreboot

# coreboot 4.7 declares a variable whose name is a single space; GNU make >= 4.3
# rejects that ("empty variable name"). coreboot 4.8+ dropped the construct, so
# remove those two lines to build this tag with a modern make.
sed -i -e '/^\$(spc) :=$/d' -e '/^\$(spc) +=$/d' Makefile.inc

# Quark X1000 boots through a legacy BIOS only because SeaBIOS is the payload.
printf '%s\n' \
	CONFIG_VENDOR_INTEL=y \
	CONFIG_BOARD_INTEL_GALILEO=y \
	CONFIG_GALILEO_GEN2=y \
	CONFIG_PAYLOAD_SEABIOS=y \
	CONFIG_ANY_TOOLCHAIN=y > .config
make olddefconfig

echo "=== selected config ==="
grep -E '^CONFIG_(BOARD_INTEL_GALILEO|GALILEO_GEN2|PAYLOAD_SEABIOS|ROM_SIZE|CBFS_SIZE|SEABIOS_STABLE)=' .config

echo "=== build ==="
make -j"$(nproc)"

test -s build/coreboot.rom
mkdir -p /out
cp build/coreboot.rom /out/coreboot.rom

echo "=== result ==="
ls -l /out/coreboot.rom
md5sum /out/coreboot.rom 2>/dev/null || md5 -r /out/coreboot.rom
