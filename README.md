# Galileo Gen 2 — Discrete GPU Rebuild Project

[简体中文文档](README.zh-CN.md)

Reviving an Intel Galileo Gen 2 (Quark X1000, 400 MHz i586, 256 MB RAM, no SSE2/PAE/CMOV)
as a fully usable Linux computer: LXDE desktop, GPU acceleration, browser, and games —
with any one of three discrete GPUs driving the display.

## Image Lineup

Three images. One defconfig, one overlay layer, standard names.

| defconfig | Image | Contents | Rootfs |
|---|---|---|---|
| `galileo_desktop` | desktop | X + openbox + xterm + pcmanfm, software 2D only | 800M |
| `galileo_workstation` | workstation | desktop + NetSurf + OpenTTD/Doom/OpenArena + glmark2/gtkperf/mesa-demos | 3000M |
| `galileo_dosbox` | dosbox | fbcon only (no X): DOSBox 0.74-3 + 15 DOS game entries + console menu | 3600M |

Naming convention — do not invent new suffixes:

| Layer | Path |
|---|---|
| common | `board/intel/galileo/rootfs_overlay_common` |
| desktop | `board/intel/galileo/rootfs_overlay_desktop` (X session, shared by desktop + workstation) |
| workstation | `board/intel/galileo/rootfs_overlay_workstation` (apps/bench extras on top of desktop) |
| dosbox | `board/intel/galileo/rootfs_overlay_dosbox` (overrides the X session with the fbcon menu) |

Two remotes, two histories. `buildserver` (Gitea, private) has the full tree and
the game-data stage; `github` gets a filtered export through
`tools/push-github.sh` — no game data, no old images, no vendor CI, and board
addresses/passwords replaced by placeholders. The GitHub matrix builds
`desktop` and `dosbox` on a stock runner; the Gitea runner builds `workstation`
(a cold build needs 5-6 h, over GitHub's job ceiling) and is the only one that
mounts the `galileo-games` volume.

## DOS image (`galileo_dosbox`)

Boots into a framebuffer-console menu on tty1; picking a number runs
`dosgame <key>`, which mounts the game directory and starts DOSBox fullscreen
through SDL's fbcon driver. There is no X, no window manager and no getty on
tty1: `q` drops to a shell and `exit` brings the menu back.

| key | Game | Notes |
|---|---|---|
| `doom` | Doom (Freedoom) | `DOOM.BAT` |
| `nfs1` `gta` `tomb` `quake` `duke3d` `dune2` `pop` `wolf3d` `homm2` `xys2` `jinyong` `uw2` `msfs5` `war2x` | archive games | one directory each, mounted as `C:`; the entries are listed in the `case` in `dosgame` |

The game data itself is not in git — it is multi-GB third-party media. It is
staged into `usr/share/dosgames/` on the build host and reaches the CI runner
through the `galileo-games` volume (`tools/push-dosgames.sh`); the build copies
the volume into the overlay. A key whose directory is missing exits at once with
`dosgame: <key>: <file> not found`, so an image built without the volume still
boots to the menu. That is why the partition is 3600M.

## System Stack

| Layer | Component |
|---|---|
| Kernel | Linux 5.10.29 (i586, `amdgpu` + `radeon` + `nouveau` as modules, TTM built in) |
| Base | Buildroot 2020.11.2, glibc, bash, dropbear SSH, DHCP |
| Display | Xorg 1.20 + modesetting + glamor (EGL/GBM) → GPU-accelerated 2D |
| GPU auto-detect | `S50desktop` insmods ttm → amdgpu → radeon → nouveau, X starts on `/dev/dri/card0` |
| Mesa | swrast + r600 (HD 6450) + radeonsi/LLVM (RX 580) + nouveau |
| Firmware | `amdgpu/polaris10*` (trimmed), whole `radeon/` (Caicos etc.), `nvidia/` |
| Desktop | openbox + lxpanel + pcmanfm (double-click launchers, XDG menu) |
| Browser | NetSurf 3.10 (GTK2, HTTPS) — workstation image only |
| Sound | ALSA + DOSBox SB16 (base 220h, IRQ 7, DMA 1, HDMA 5) + ALSA MIDI |
| Utilities | links2, lspci, neofetch, wqy CJK fonts, HTTP time sync |

## Key Files

- `configs/galileo_{workstation,dosbox}_defconfig` — buildroot package selection
- `board/intel/galileo/linux-rx580.config` — kernel config (non-DC radeon path; Quark has no SSE2)
- `board/intel/galileo/rootfs_overlay*/` — xorg.conf, init scripts, golden files
- `board/intel/galileo/rootfs_overlay_dosbox/usr/bin/dosgame` — per-game mount + launch table
- `board/intel/galileo/rootfs_overlay_dosbox/root/.dosbox/dosbox-0.74.conf` — DOSBox defaults
- `board/intel/galileo/post-build.sh` — gconv install (iconv), amdgpu firmware trim
- `board/intel/galileo/BENCHMARKS.md` — SuperPI / 7-Zip numbers and the full benchmark procedure
- `package/dosbox/` — DOSBox 0.74-3 (SDL detection fix, libglu dep)
- `package/netsurf/` — browser patches
- `package/openjdk-bin/`, `package/openjdk/` — aarch64 host + X11 AWT patches
- `package/llvm/` — benchmark disabled (host build fix)

## Build

```sh
./build.sh          # requires Docker + gres-builder image
```

Output: `sdcard.img` in the docker volume `gres-out`. Flash with `dd`.

Single image, locally:

```sh
docker run --rm -v "$PWD":/build -v gres-out:/out -w /build \
  -e FORCE_UNSAFE_CONFIGURE=1 gres-builder sh -c '
    make O=/out/dosbox BR2_DL_DIR=/out/dl galileo_dosbox_defconfig &&
    make O=/out/dosbox BR2_DL_DIR=/out/dl -j$(nproc)'
```

## BIOS bring-up (experimental)

The stock firmware is UEFI-only (no CSM), so DOS-family systems cannot boot on
it. `board/intel/galileo/coreboot/` contains a coreboot 4.20.1 + SeaBIOS build
that provides real-mode BIOS services; it bundles the Quark FSP-M/FSP-S and RMU
blobs that the earlier 4.7 attempt lacked. The ROM build is verified (CBFS
contains `fspm.bin`, `fsps.bin`, `rmu.bin` at `0xfff00000`, and the SeaBIOS
payload); the SPI write and boot still need hardware validation. See
`board/intel/galileo/coreboot/README.md`.

## Board Quick Reference

- SSH: `root@<board-ip>`, password in the internal record (not in this repo)
- scp is broken (dropbear): pipe files via `cat file | ssh ... "cat > /dest"`
- busybox tar has no combined flags: `zcat f.tgz | tar -x -f - -C /`
- No RTC battery: `S46timesync` syncs clock from HTTP Date headers at boot
- Timezone: `/etc/TZ` = CST-8 (Beijing time)

## Known Pitfalls (battle scars)

- uClibc pretends to be glibc (`__GLIBC__` defined) — OpenTTP's execinfo check needs `&& !defined(__UCLIBC__)`
- buildroot incremental builds: config changes don't rebuild packages — `rm -rf build/<pkg>-*`
- Writing to a running binary fails (text file busy) — kill first, then atomic `mv`
- libfm desktop launchers do nothing silently when `/usr/share/libfm/ui/` is missing
- glibc images need gconv modules or iconv/g_convert break (openbox "Unnamed Window" titles)
- XDG menus need `<DefaultAppDirs/>` in the XML and `lxde-applications.menu` naming
- MIT-SHM requires `image->data = shmaddr` after `shmat`
- DOS games may demand a CD-ROM: a plain `C:` mount is not enough, the same directory
  must also be mounted `-t cdrom` with `-label` (DOSBox 0.74-3 supports both)
- Red Alert DOS validates `REDALERT.INI` `[Sound] Card=`: an out-of-table value aborts with
  `Cannot initialize detection system (15)`
- Warcraft II DOS reads `MUDDAT.CUD`/`SNDDAT.WAR` from the disc, not from the install dir —
  ship them inside `DATA/` and keep `cdpath=d:\` in `WAR2.INI`
