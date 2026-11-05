#!/bin/sh
# Compose a flashable 8 MiB SPI image for Galileo Gen 2 without touching the
# vendor SI_DESC / SI_ME region (flash offset 0x000000-0x1fffff).
#
#   compose-flash.sh dump.bin coreboot.rom flash.bin
#
# dump.bin      full 8 MiB read of the original SPI flash (flashrom -r)
# coreboot.rom  output of build-rom.sh (8 MiB)
# flash.bin     image to write with an SPI programmer
#
# Only the top 2 MiB (CBFS, flash offset 0x600000-0x7fffff) is replaced. The
# first 2 MiB keeps the flash descriptor and SI_ME, so a failed boot can be
# recovered by writing dump.bin back.
set -eu

DUMP=${1:?usage: compose-flash.sh DUMP COREBOOT_ROM OUT}
ROM=${2:?usage: compose-flash.sh DUMP COREBOOT_ROM OUT}
OUT=${3:?usage: compose-flash.sh DUMP COREBOOT_ROM OUT}

ROM_SIZE=8388608
CBFS_SIZE=2097152
BIOS_OFF=$((ROM_SIZE - CBFS_SIZE))

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RMU="$SCRIPT_DIR/blobs/rmu.bin"

[ -f "$DUMP" ] || { echo "missing dump: $DUMP" >&2; exit 1; }
[ -f "$ROM" ]  || { echo "missing ROM: $ROM" >&2; exit 1; }
[ -f "$RMU" ]  || { echo "missing $RMU" >&2; exit 1; }

[ "$(wc -c < "$DUMP")" -eq "$ROM_SIZE" ] || { echo "dump must be exactly 8 MiB" >&2; exit 1; }
[ "$(wc -c < "$ROM")"  -eq "$ROM_SIZE" ] || { echo "ROM must be exactly 8 MiB" >&2; exit 1; }

cp "$DUMP" "$OUT"

# Replace only the CBFS tail.
dd if="$ROM" of="$OUT" bs=65536 \
	skip=$((BIOS_OFF / 65536)) seek=$((BIOS_OFF / 65536)) \
	count=$((CBFS_SIZE / 65536)) conv=notrunc 2>/dev/null

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# The descriptor/ME region must be byte-identical to the original dump.
dd if="$DUMP" of="$TMP/si_all_dump" bs=65536 count=$((BIOS_OFF / 65536)) 2>/dev/null
dd if="$OUT"  of="$TMP/si_all_out"  bs=65536 count=$((BIOS_OFF / 65536)) 2>/dev/null
cmp "$TMP/si_all_dump" "$TMP/si_all_out" || {
	echo "SI_ALL region changed; refusing to produce the image" >&2
	exit 1
}

# coreboot places rmu.bin at CONFIG_RMU_LOC = 0xfff00000, i.e. flash offset
# 0x700000 (flash is mapped at 0xff800000). Verify it lands there.
rmu_size=$(wc -c < "$RMU")
dd if="$OUT" bs=65536 skip=112 count=1 2>/dev/null | head -c "$rmu_size" > "$TMP/rmu"
cmp "$TMP/rmu" "$RMU" || {
	echo "rmu.bin is not at flash offset 0x700000" >&2
	exit 1
}

echo "OK: $OUT"
echo "  size:            $(wc -c < "$OUT") bytes"
echo "  SI_ALL:          preserved (0x000000-0x1fffff)"
echo "  CBFS:            replaced (0x600000-0x7fffff)"
echo "  rmu.bin:         verified at 0xfff00000"
sha256sum "$OUT"
