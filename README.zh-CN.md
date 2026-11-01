# Galileo Gen 2 — 独立显卡重制计划

[English README](README.md)

把一块 Intel Galileo Gen 2(Quark X1000,400MHz i586,256MB 内存,无 SSE2/PAE/CMOV)
重制为一台功能齐全的 Linux 电脑:LXDE 桌面、GPU 加速、浏览器、游戏——
由三张候选独显中的任意一张驱动显示。

## 镜像阵容

只构建、只发布两个镜像(CI matrix:`workstation`、`dosbox`)。

| 镜像 | 内容 | 根文件系统 |
|---|---|---|
| `galileo_workstation` | 全功能 Linux:LXDE + GPU 加速 + NetSurf + Doom/OpenTTD/Minetest/Minecraft + radeonsi | 2400M |
| `galileo_dosbox` | DOS 游戏机:X + DOSBox 0.74-3 + 8 个可玩 DOS 游戏,桌面一键图标 | 3600M |

`galileo_terminal` / `galileo_browser` / `galileo_nvidia` 三个 defconfig 只作参考保留,不再构建。

## DOS 镜像(`galileo_dosbox`)

开机直接进 LXDE,每个游戏一个桌面图标;图标执行 `dosgame <key>`,由它挂载目录并全屏启动 DOSBox。

| key | 游戏 | 说明 |
|---|---|---|
| `pal95` | 仙剑奇侠传 95 | `PAL.EXE` |
| `civ1` | 文明 1 | 从两张软盘镜像启动 |
| `nfs` | 极品飞车 SE | `NFSSE/NFS.EXE` |
| `sc2000` | 模拟城市 2000 | `C_SC2000/SC2000.EXE` |
| `sgz5` | 三国志 V PK | 把 `san5.cue`(数据 ISO + 20 条 MP3 CDDA 音轨)挂成 D: |
| `doom` | Doom(Freedoom) | `DOOM.BAT` |
| `ra1` | 红色警戒 1 | DOS v1.04:`RA.EXE` + `GAME.DAT` + `MAIN.MIX`/`REDALERT.MIX` |
| `war2` | 魔兽争霸 II:黑潮 | DOS v1.50:`WAR2.EXE` + 光盘侧 `DATA/` |

红警和魔兽都要求"光盘在场",所以同一个目录挂两次:`C:` 当游戏盘,`D:` 当带卷标的 CD-ROM:

```
dosbox -conf ~/.dosbox/dosbox-0.74.conf \
       -c "mount c /usr/share/dosgames/RA1" \
       -c "mount d /usr/share/dosgames/RA1 -t cdrom -label CD1" \
       -c "c:" -c "RA.EXE"
```

红警的运行配置在 `RA1/REDALERT.INI`(`[Sound] Card=13` 即 SB16,`Port=220h/IRQ=7/DMA=1`);
魔兽用 `WAR2.INI`(`cdpath=d:\`、`cdaudio=0`)和 `WAR2/DRIVERS/` 下的 Miles 驱动映射。
游戏数据合计 880MB,所以 ext2/分区尺寸提到 3600M。

## 系统栈

| 层 | 组件 |
|---|---|
| 内核 | Linux 5.10.29(i586;`amdgpu`/`radeon`/`nouveau` 编译为模块,TTM 内置) |
| 基础 | Buildroot 2020.11.2、glibc、bash、dropbear SSH、DHCP |
| 显示 | Xorg 1.20 + modesetting + glamor(EGL/GBM)→ GPU 加速 2D |
| 显卡自检 | `S50desktop` 依序 insmod ttm → amdgpu → radeon → nouveau,出现 `/dev/dri/card0` 才起 X |
| Mesa | swrast + r600(HD 6450)+ radeonsi/LLVM(RX 580)+ nouveau |
| 固件 | `amdgpu/polaris10*`(裁剪)、完整 `radeon/`(Caicos 等)、`nvidia/` |
| 桌面 | openbox + lxpanel + pcmanfm(双击启动器、XDG 菜单) |
| 浏览器 | NetSurf 3.10(GTK2,HTTPS)—— 仅 workstation 镜像 |
| 声音 | ALSA + DOSBox SB16(base 220h、IRQ 7、DMA 1、HDMA 5)+ ALSA MIDI |
| 工具 | links2、lspci、neofetch、wqy 中文字体、HTTP 对时 |

## 关键文件

- `configs/galileo_{workstation,dosbox}_defconfig` — buildroot 包选择
- `board/intel/galileo/linux-rx580.config` — 内核配置(非 DC radeon 路径;Quark 无 SSE2)
- `board/intel/galileo/rootfs_overlay*/` — xorg.conf、启动脚本、黄金文件
- `board/intel/galileo/rootfs_overlay_dosbox/usr/bin/dosgame` — 每个游戏的挂载 + 启动表
- `board/intel/galileo/rootfs_overlay_dosbox/root/.dosbox/dosbox-0.74.conf` — DOSBox 默认配置
- `board/intel/galileo/post-build.sh` — gconv 安装(iconv)、amdgpu 固件裁剪
- `board/intel/galileo/BENCHMARKS.md` — SuperPI / 7-Zip 跑分结果与完整测试流程
- `package/dosbox/` — DOSBox 0.74-3(SDL 探测修复、libglu 依赖)

## 构建

```sh
./build.sh          # 需要 Docker + gres-builder 镜像
```

产出 docker 卷 `gres-out` 中的 `sdcard.img`,dd 刷入 SD 卡。

单独构建某一个镜像:

```sh
docker run --rm -v "$PWD":/build -v gres-out:/out -w /build \
  -e FORCE_UNSAFE_CONFIGURE=1 gres-builder sh -c '
    make O=/out/dosbox BR2_DL_DIR=/out/dl galileo_dosbox_defconfig &&
    make O=/out/dosbox BR2_DL_DIR=/out/dl -j$(nproc)'
```

## 板子速查

- SSH:`root@<板子IP>`,密码 `<board-password>`
- scp 坏了(dropbear):用 `cat file | ssh ... "cat > /dest"` 管道传文件
- busybox tar 不认组合参数:`zcat f.tgz | tar -x -f - -C /`
- 无 RTC 电池:`S46timesync` 开机从 HTTP Date 头同步时间
- 时区:`/etc/TZ` = CST-8(北京时间)

## 已知坑(血泪史)

- uClibc 自称 glibc(定义 `__GLIBC__`),OpenTTD 的 execinfo 检测要加 `&& !defined(__UCLIBC__)`
- buildroot 增量构建:改 defconfig 后包不重编,必须 `rm -rf build/<pkg>-*`
- 往运行中的二进制写入会 text file busy,先杀进程再原子 `mv`
- libfm 桌面启动器双击无反应 = `/usr/share/libfm/ui/` 未部署(静默取消)
- glibc 镜像缺 gconv 模块则 iconv/g_convert 全崩(openbox 标题变 "Unnamed Window")
- XDG 菜单 XML 必须含 `<DefaultAppDirs/>`,文件名必须是 `lxde-applications.menu`
- MIT-SHM 在 `shmat` 后必须设置 `image->data = shmaddr`
- DOS 游戏可能要"光盘":只挂 `C:` 不够,同一目录还要 `-t cdrom -label` 挂成 D:
  (DOSBox 0.74-3 两者都支持)
- 红警 DOS 会校验 `REDALERT.INI` 的 `[Sound] Card=`:越界值直接
  `Cannot initialize detection system (15)` 退出
- 魔兽 DOS 从光盘读 `MUDDAT.CUD`/`SNDDAT.WAR`,不在安装目录里 —— 要放进 `DATA/`
  并保持 `WAR2.INI` 的 `cdpath=d:\`
