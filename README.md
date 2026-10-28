# Galileo Gen 2 — Discrete GPU Rebuild Project

[简体中文文档](README.zh-CN.md)

Reviving an Intel Galileo Gen 2 (Quark X1000, 400 MHz i586, 256 MB RAM, no SSE2/PAE/CMOV)
as a fully usable Linux computer: LXDE desktop, GPU acceleration, browser, and games —
with a discrete AMD GPU driving the display.

## Image Lineup

| Image | libc | Contents |
|---|---|---|
| `galileo_terminal` | uClibc | X + xterm, ShadowFB software 2D |
| `galileo_browser` | glibc | Full LXDE desktop + NetSurf, nothing else |
| `galileo_workstation` | glibc | Everything: LXDE + GPU accel + Doom/OpenTTD/Minetest/Minecraft + radeonsi |

## System Stack (workstation)

| Layer | Component |
|---|---|
| Kernel | Linux 5.10.29 (i586, radeon + amdgpu modules) |
| Base | Buildroot 2020.11.2, bash, dropbear SSH, DHCP |
| Display | Xorg 1.20 + modesetting + glamor (EGL/GBM) → GPU-accelerated 2D |
| Desktop | openbox + lxpanel + pcmanfm (double-click launchers, XDG menu) |
| Browser | NetSurf 3.10 (GTK2, HTTPS) |
| Games | Doom (freedoom), OpenTTD (OpenGFX), Minetest, Minecraft 1.5.2 (OpenJDK 14) |
| GPU drivers | r600g (HD 6450) + radeonsi/LLVM (RX 580 ready, untested) |
| Utilities | links2, lspci, neofetch, wqy CJK fonts, HTTP time sync |

## Key Files

- `configs/galileo_{terminal,browser,workstation}_defconfig` — buildroot package selection
- `board/intel/galileo/linux-rx580.config` — kernel config (non-DC radeon path; Quark has no SSE2)
- `board/intel/galileo/rootfs_overlay*/` — xorg.conf, init scripts, golden files
- `board/intel/galileo/post-build.sh` — gconv install (iconv), cleanup
- `package/netsurf/` — browser patches
- `package/openjdk-bin/`, `package/openjdk/` — aarch64 host + X11 AWT patches
- `package/llvm/` — benchmark disabled (host build fix)

## Build

```sh
./build.sh          # requires Docker + gres-builder image
```

Output: `sdcard.img` in the docker volume `gres-out`. Flash with `dd`.

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
