#!/bin/sh
# Build a bootable Galileo Gen 2 BIOS: coreboot 4.20.1 + SeaBIOS payload,
# with the Quark FSP-M/FSP-S and RMU binaries actually present in CBFS.
#
# What changed vs. the old 4.7 script:
#   * coreboot 4.20.1 — 4.20.x is the last release that still carries
#     src/soc/intel/quark and src/mainboard/intel/galileo (4.21 dropped them).
#     The official release tarball is used because it bundles 3rdparty/vboot,
#     which cbfstool needs (the GitHub source tarball does not).
#   * CONFIG_ADD_FSP_BINARIES + CONFIG_ADD_RMU_FILE are enabled. Without them
#     romstage runs fsp_memory_init(), fails to find "fspm.bin" in CBFS and
#     calls die("FSPM not available!") before SeaBIOS is ever reached.
#   * SeaBIOS' payload Makefile emits
#     CONFIG_DEBUG_SERIAL_MEM_ADDRESS=$(CONFIG_CONSOLE_UART_BASE_ADDRESS)
#     for DRIVERS_UART_8250MEM_32. Quark has no such symbol (only TTYS0_BASE),
#     so the address is patched to $(CONFIG_TTYS0_BASE) = 0xa0019000.
#   * blobs are vendored under ./blobs, so the build is offline and pinned.
#
# Output: $OUT/coreboot.rom (8 MiB) and $OUT/cbfs.txt.
#
# NEVER flash coreboot.rom over the whole SPI chip. It has no SI_DESC/SI_ME
# content; use compose-flash.sh with a dump of the original flash instead.
set -eu

OUT="${OUT:-/out}"
WORK="${WORK:-/work}"
CB_VER="${CB_VER:-4.20.1}"
TARBALL="${COREBOOT_TARBALL:-/assets/coreboot-${CB_VER}.tar.xz}"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ ! -f "$TARBALL" ]; then
	TARBALL="/tmp/coreboot-${CB_VER}.tar.xz"
	if [ ! -f "$TARBALL" ]; then
		echo "downloading coreboot ${CB_VER} release tarball (bundles vboot)"
		curl -fL --retry 3 -C - \
			"https://coreboot.org/releases/coreboot-${CB_VER}.tar.xz" \
			-o "$TARBALL"
	fi
fi

mkdir -p "$WORK" "$OUT"
SRC="$WORK/src"
rm -rf "$SRC"
mkdir -p "$SRC"
tar -xJf "$TARBALL" -C "$SRC" --strip-components=1
cd "$SRC"

test -d src/soc/intel/quark || { echo "no Quark SoC in this coreboot tree" >&2; exit 1; }
test -f 3rdparty/vboot/firmware/include/vb2_sha.h || {
	echo "3rdparty/vboot missing: use the official coreboot release tarball" >&2
	exit 1
}

# --- blobs ----------------------------------------------------------------
BLOBS="$SCRIPT_DIR/blobs"
for f in FSP_M.fd FSP_S.fd rmu.bin; do
	[ -s "$BLOBS/$f" ] || { echo "missing blob: $BLOBS/$f" >&2; exit 1; }
done

mkdir -p 3rdparty/blobs/soc/intel/quark/Fsp2_0Pei/RELEASE
cp "$BLOBS/FSP_M.fd" "$BLOBS/FSP_S.fd" \
	3rdparty/blobs/soc/intel/quark/Fsp2_0Pei/RELEASE/
cp "$BLOBS/rmu.bin" 3rdparty/blobs/soc/intel/quark/rmu.bin

# --- SeaBIOS serial address fix ------------------------------------------
sed -i \
	's|CONFIG_DEBUG_SERIAL_MEM_ADDRESS=$(CONFIG_CONSOLE_UART_BASE_ADDRESS)|CONFIG_DEBUG_SERIAL_MEM_ADDRESS=$(CONFIG_TTYS0_BASE)|' \
	payloads/external/SeaBIOS/Makefile
grep -q 'CONFIG_DEBUG_SERIAL_MEM_ADDRESS=$(CONFIG_TTYS0_BASE)' \
	payloads/external/SeaBIOS/Makefile || {
	echo "SeaBIOS serial-address patch did not apply" >&2
	exit 1
}

# --- configuration -------------------------------------------------------
cat > .config <<'EOF'
CONFIG_VENDOR_INTEL=y
CONFIG_BOARD_INTEL_GALILEO=y
CONFIG_GALILEO_GEN2=y

CONFIG_FSP_VERSION_2_0=y
CONFIG_FSP_TYPE_2_0_PEI=y
CONFIG_FSP_BUILD_TYPE_RELEASE=y
CONFIG_ADD_FSP_BINARIES=y
CONFIG_FSP_M_FILE="3rdparty/blobs/soc/intel/quark/Fsp2_0Pei/RELEASE/FSP_M.fd"
CONFIG_FSP_S_FILE="3rdparty/blobs/soc/intel/quark/Fsp2_0Pei/RELEASE/FSP_S.fd"

CONFIG_ADD_RMU_FILE=y
CONFIG_RMU_FILE="3rdparty/blobs/soc/intel/quark/rmu.bin"

CONFIG_PAYLOAD_SEABIOS=y
CONFIG_SEABIOS_DEBUG_LEVEL=7

CONFIG_ENABLE_BUILTIN_HSUART1=y
CONFIG_CONSOLE_SERIAL=y
CONFIG_DEFAULT_CONSOLE_LOGLEVEL_8=y
CONFIG_ENABLE_SD_TESTING=n

CONFIG_ANY_TOOLCHAIN=y
CONFIG_ROM_SIZE=0x800000
CONFIG_CBFS_SIZE=0x200000
EOF

make olddefconfig

# SeaBIOS 1.16's build calls "python"; Debian bookworm only ships python3.
if ! command -v python >/dev/null 2>&1; then
	mkdir -p /tmp/pyshim
	printf '#!/bin/sh\nexec python3 "$@"\n' > /tmp/pyshim/python
	chmod +x /tmp/pyshim/python
	PATH="/tmp/pyshim:$PATH"
	export PATH
fi

# Single-threaded on purpose: SeaBIOS' Kconfig step (`make -C seabios
# olddefconfig`) inherits the jobserver and dies with "write jobserver: Bad
# file descriptor" under some container/shell combinations when the outer make
# runs with -j. The Quark build is small enough that -j1 is acceptable.
make -j1

ROM=build/coreboot.rom
test -s "$ROM"
build/cbfstool "$ROM" print > "$OUT/cbfs.txt"

# --- verification --------------------------------------------------------
fail=0
for f in fspm.bin fsps.bin rmu.bin fallback/payload; do
	if ! grep -q "^$f[[:space:]]" "$OUT/cbfs.txt"; then
		echo "MISSING in CBFS: $f" >&2
		fail=1
	fi
done

# cbfstool's default listing does not show the load address, so verify the
# RMU lands at CONFIG_RMU_LOC (0xfff00000) by looking at the image directly.
# Flash is mapped at 0xff800000, so 0xfff00000 is file offset 0x700000.
rmu_size=$(wc -c < "$BLOBS/rmu.bin")
dd if="$ROM" bs=65536 skip=112 count=1 2>/dev/null | head -c "$rmu_size" > /tmp/rmu.check
if ! cmp -s /tmp/rmu.check "$BLOBS/rmu.bin"; then
	echo "rmu.bin is not at 0xfff00000 (file offset 0x700000)" >&2
	fail=1
fi
[ "$fail" -eq 0 ] || exit 1

cp "$ROM" "$OUT/coreboot.rom"
echo "=== ROM ==="
ls -l "$OUT/coreboot.rom"
sha256sum "$OUT/coreboot.rom"
echo "=== CBFS ==="
cat "$OUT/cbfs.txt"
