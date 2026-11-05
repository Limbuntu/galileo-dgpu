# Galileo Gen 2 BIOS bring-up (coreboot + SeaBIOS)

This directory builds a **replacement firmware** for the Galileo Gen 2
(Quark X1000): coreboot 4.20.1 as the firmware and SeaBIOS as its payload. The
purpose is to give DOS-family systems the real-mode BIOS services (INT 10h
video, INT 13h disk, INT 16h keyboard) that the stock UEFI-only firmware does
not provide.

It is **not** a CSM inside UEFI, and it does **not** touch the Linux SD-card
image in `images/`. It replaces the SPI flash contents.

## Status (built and verified structurally)

A complete build has been produced and checked:

* `coreboot.rom`: 8 388 608 bytes, SHA-256
  `f1afd83049319486d4be51c87353406780fe5ec974ea18301eb4172acd1287ca`
  (the SHA-256 changes between builds because CBFS carries a build timestamp;
  the shipped copy is `images/bios/coreboot.rom`)
* CBFS: `fspm.bin` (192 512), `fsps.bin` (4 096), `rmu.bin` (8 192, at
  `0xfff00000`) and `fallback/payload` (77 846, SeaBIOS
  `rel-1.16.1-0-g3208b098`).
* `compose-flash.sh` verified against the real ROM: SI_ALL untouched, CBFS
  replaced, `rmu.bin` byte-identical at flash offset `0x700000`.

Booting it on hardware has **not** been tested here (no board attached). The
first open item is PCIe legacy VGA forwarding; see "Known limits".

## What is in here

| File | Purpose |
|---|---|
| `Dockerfile` | Debian bookworm toolchain (gcc 12, iasl, nasm, …) |
| `build-rom.sh` | Builds `coreboot.rom` + `cbfs.txt`, verifies CBFS contents |
| `blobs/` | FSP-M, FSP-S, RMU (see `blobs/README.md` for provenance/licenses) |
| `compose-flash.sh` | Merges `coreboot.rom` into a vendor dump, preserving SI_ALL |

## Why 4.20.1 and why blobs

* **4.20.x is the last coreboot release with `src/soc/intel/quark` and
  `src/mainboard/intel/galileo`**; 4.21 removed them.
* The official release tarball (not the GitHub source archive) bundles
  `3rdparty/vboot`, which `cbfstool` needs (`vb2_sha.h`).
* Without `CONFIG_ADD_FSP_BINARIES`, romstage's `fsp_memory_init()` cannot find
  `fspm.bin` in CBFS and calls `die("FSPM not available!")`.
* Without `CONFIG_ADD_RMU_FILE`, the Quark's RMU firmware is absent; Kconfig
  says it is "required to bring the Quark processor out of reset".
* SeaBIOS' payload Makefile writes
  `CONFIG_DEBUG_SERIAL_MEM_ADDRESS=$(CONFIG_CONSOLE_UART_BASE_ADDRESS)`, a
  symbol Quark does not define. `build-rom.sh` patches it to
  `$(CONFIG_TTYS0_BASE)` (0xa0019000).

## Build

```sh
docker build -t galileo-coreboot .

mkdir -p out
docker run --rm \
  -v /path/to/coreboot-4.20.1.tar.xz:/assets/coreboot-4.20.1.tar.xz:ro \
  -v "$PWD/out:/out" \
  galileo-coreboot
```

Or, with the repository mounted read-only:

```sh
docker run --rm \
  -v /path/to/coreboot-4.20.1.tar.xz:/assets/coreboot-4.20.1.tar.xz:ro \
  -v "$PWD:/scripts:ro" -v "$PWD/out:/out" \
  galileo-coreboot sh /scripts/build-rom.sh
```

Output: `out/coreboot.rom` (8 MiB) and `out/cbfs.txt`. The build fails unless
`fspm.bin`, `fsps.bin`, `rmu.bin` and `fallback/payload` are all present and
`rmu.bin` is at `0xfff00000`.

## Compose a flashable image

`coreboot.rom` is **not** a complete SPI image: the first 2 MiB
(`SI_DESC` + `SI_ME`) come from the board. Never write the 8 MiB ROM over the
whole chip.

```sh
flashrom -p ch341a_spi -r dump1.bin
flashrom -p ch341a_spi -r dump2.bin
sha256sum dump1.bin dump2.bin      # must match; keep both

./compose-flash.sh dump1.bin out/coreboot.rom flash.bin
```

The script writes only `0x600000-0x7fffff` (the CBFS tail), verifies that
`0x000000-0x1fffff` is byte-identical to the dump, and verifies that `rmu.bin`
landed at flash offset `0x700000` (`0xfff00000` in the memory map).

## Flash

First time (and every recovery) needs an external SPI programmer — the stock
UEFI capsule updater will not accept a coreboot image:

```sh
flashrom -p ch341a_spi -w flash.bin
flashrom -p ch341a_spi -v flash.bin
```

Keep `dump1.bin`; writing it back is the only recovery path.

## First boot: what to expect on the serial console

HSUART1, **115200 8N1**:

1. `coreboot` banner — bootblock ran.
2. FSP-M memory-init log — 256 MiB detected.
3. postcar/ramstage messages, PCIe root port init.
4. `SeaBIOS` banner — payload started.
5. PCI scan; the VGA option ROM runs; SeaBIOS prints its boot menu.
6. Boot device: SD card (SeaBIOS `sdcard.c`) or USB storage.

If it stops:

| Symptom | Meaning |
|---|---|
| no coreboot banner | SPI image/reset problem; re-check `compose-flash.sh` output |
| banner, then `FSPM not available!` | FSP not in CBFS; check `out/cbfs.txt` |
| SeaBIOS banner, but no video | PCIe legacy VGA not forwarded; see below |
| no boot device | SDHCI not recognised by SeaBIOS; try a USB stick |

## Known limits

* **Video is the open question.** SeaBIOS must execute the GPU's legacy VBIOS
  and Quark's PCIe root port must forward legacy VGA I/O. Use an **HD 6450**
  (1 GiB, 32-bit BAR) for the first test; an RX 580's 8 GiB VRAM cannot be
  mapped below 4 GiB on a 32-bit firmware.
* **No sound**: there is no ISA bus and DOS has no HD-Audio driver.
* **No mouse** (no PS/2; USB HID in DOS needs a driver).
* Keyboard depends on SeaBIOS' USB stack.
* `ENABLE_SD_TESTING` is disabled in the build (it only reads block 0, but it
  adds boot noise).

## Recovery

```sh
flashrom -p ch341a_spi -w dump1.bin
```

If the board does not respond on SPI, check the clip orientation and that the
board is unpowered before attaching the programmer.
