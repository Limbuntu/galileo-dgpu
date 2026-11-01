# Galileo Gen 2 — Discrete GPU Rebuild Project

[简体中文文档](README.zh-CN.md)

Reviving an Intel Galileo Gen 2 (Quark X1000, 400 MHz i586, 256 MB RAM, no SSE2/PAE/CMOV)
as a fully usable Linux computer: LXDE desktop, GPU acceleration, browser, and games —
with any one of three discrete GPUs driving the display.

## Image Lineup

Two images are built and shipped (CI matrix: `workstation`, `dosbox`).

| Image | Contents | Rootfs |
|---|---|---|
| `galileo_workstation` | Full Linux: LXDE + GPU accel + NetSurf + Doom/OpenTTD/Minetest/Minecraft + radeonsi | 2400M |
| `galileo_dosbox` | DOS console: X + DOSBox 0.74-3 + 8 playable DOS games, one-click desktop icons | 3600M |

`galileo_terminal` / `galileo_browser` / `galileo_nvidia` defconfigs are kept in `configs/`
for reference but are no longer built.

## DOS image (`galileo_dosbox`)

Boots straight into LXDE with a desktop icon per game; every icon runs
`dosgame <key>`, which mounts the game directory and starts DOSBox fullscreen.

| key | Game | Notes |
|---|---|---|
| `pal95` | 仙剑奇侠传 95 | runs `PAL.EXE` |
| `civ1` | 文明 1 | boots from two floppy images |
| `nfs` | 极品飞车 SE | `NFSSE/NFS.EXE` |
| `sc2000` | 模拟城市 2000 | `C_SC2000/SC2000.EXE` |
| `sgz5` | 三国志 V PK | mounts `san5.cue` (data ISO + 20 MP3 CDDA tracks) as D: |
| `doom` | Doom (Freedoom) | `DOOM.BAT` |
| `ra1` | 红色警戒 1 | DOS v1.04: `RA.EXE` + `GAME.DAT` + `MAIN.MIX`/`REDALERT.MIX` |
| `war2` | 魔兽争霸 II：黑潮 | DOS v1.50: `WAR2.EXE` + CD-side `DATA/` |

Two games insist on seeing their disc, so their directory is mounted twice —
`C:` as the game drive and `D:` as a CD-ROM with an explicit volume label:

```
dosbox -conf ~/.dosbox/dosbox-0.74.conf \
       -c "mount c /usr/share/dosgames/RA1" \
       -c "mount d /usr/share/dosgames/RA1 -t cdrom -label CD1" \
       -c "c:" -c "RA.EXE"
```

Red Alert's runtime config lives in `RA1/REDALERT.INI` (`[Sound] Card=13` = SB16,
`Port=220h/IRQ=7/DMA=1`). Warcraft II uses `WAR2.INI` (`cdpath=d:\`, `cdaudio=0`)
plus the Miles driver maps under `WAR2/DRIVERS/`. Game data is 880 MB in total,
hence the 3600M ext2/partition size.

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

## Board Quick Reference

- SSH: `root@<board-ip>`, password `<board-password>`
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
