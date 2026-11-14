# Galileo Gen 2 跑分记录与测试流程

板子型号: Intel Galileo Gen 2 (Quark X1000) — 单核 400 MHz, i586, 无 SSE2/PAE/CMOV, 256 MB DDR3(可用 203–209 MB)。
记录日期: 2026-10-30。数据来源: omp 会话 `--private-tmp--/2026-09-23T08-16-03-401Z_01a0cd56-0b49-744b-a908-3e7a7e114033`
(搜 `Tot:` / `Total real time`),对应提交 `ac39ecf`(nvidia 镜像内置 SuperPI + 7-Zip + `bench` 启动器)。

## 1. 结论

| 项目 | Galileo Gen 2 (Quark X1000 400 MHz) | 对照机 i5-14400F | 倍差 |
|---|---|---|---|
| 7-Zip `7zr b`(Tot 综合评分) | **111 MIPS**(干净环境) | 78 201 MIPS(7-Zip 24.09,GIPS×1000) | **705×** |
| SuperPI 1M `pi_css5 1048576` | **306.63 秒**(5 分 07 秒) | 1.01 秒(同源码 win 版) | **304×** |

等效年代感:整数 ≈ 1997 年 Pentium II 266,浮点 ≈ 1995 年 Pentium Pro 200。

## 2. 测试环境

| 层 | 内容 |
|---|---|
| CPU | Intel Quark X1000,1 核 1 线程,400 MHz,无 SSE/SSE2,无 CMOV |
| 内存 | 256 MB 板载,内核报告可用 203 MB(`7-Zip` 自报 `RAM size: 203 MB`) |
| 系统 | 本仓库 `galileo_nvidia` 镜像(Linux 5.10.29 i586 + glibc + LXDE),Xorg 跑在 ShadowFB |
| GPU(当次) | HD 6450 (r600g) 与 TITAN X (nouveau) 均试过,跑分与 GPU 无关(纯 CPU) |
| 存储 | microSD(SPI) |

## 3. 工具与可比性

| 工具 | 版本 | 板上路径 | md5 |
|---|---|---|---|
| 7-Zip(p7zip) | 16.02,32-bit,`7zr` | `/usr/bin/7zr` | `bc0a0bb22df393904e97f4c46662c336` |
| SuperPI | Ooura `pi_fftcs`(FFT+AGM) | `/usr/bin/pi_css5` | `6775a1f61aced0bcf0c1bacb14f91ed7` |
| 一键脚本 | `bench`(pi 1M + 7zr b) | `/usr/bin/bench` | `65bd0532ac7b563099509334f725292c` |
| Windows 对照 | `pi_css5_x64.exe` | `~/Downloads/pi_css5_x64.exe` | `dffe4dced65d137eaee617814e5d0043` |

为什么要这两样:

- **7-Zip 内置 benchmark**:同一份 C 代码,Linux/Windows 都跑 `7z b`,输出 MIPS/Rating,直接可比。注意 7-Zip 24.09 的 Windows 版显示 **GIPS**,要 ×1000 换算成 MIPS;版本不同(16.02 vs 24.09)会有几个百分点差异,对比时说明版本即可。
- **SuperPI**:Kanada 原版闭源且只有 Windows;这里用 Ooura 的 `pi_fftcs`(同样 FFT+AGM 算法),源码同一份,Windows/Linux 各自编译,秒数 100% 可比。

## 4. 原始输出(逐字)

7-Zip(板子上一次干净运行):

```
7-Zip (a) [32] 16.02 : Copyright (c) 1999-2016 Igor Pavlov : 2016-05-21
p7zip Version 16.02 (locale=C,Utf16=off,HugeFiles=on,32 bits,1 CPU x86)

x86
CPU Freq: 16000000 21333333 21333333 21333333 64000000 85333333 170666666 512000000 1024000000

RAM size:     203 MB,  # CPU hardware threads:   1
RAM usage:    111 MB,  # Benchmark threads:      1

                       Compressing  |                  Decompressing
Dict     Speed Usage    R/U Rating  |      Speed Usage    R/U Rating
         KiB/s     %   MIPS   MIPS  |      KiB/s     %   MIPS   MIPS

22:         95    99     94     93  |       1449    99    125    124
23:         93    98     96     95  |       1443    99    126    125
----------------------------------  | ------------------------------
Avr:              99     95     94  |               99    126    124
Tot:              99    110    109
```

三次运行的 `Tot` 行(同机不同负载):

```
Tot:              49    110     54     ← 有后台任务,不计
Tot:              99    110    109     ← 复测
Tot:              99    112    111     ← 干净环境,99% CPU 满载,采用
```

SuperPI(1M 位):

```
Total real time: 306.63 seconds (5 minutes 7 seconds)
```

## 5. 详细测试流程

### 5.1 前置条件

1. 板子接电、接网线(或用串口),确认能 `ping` 通;默认 SSH `root@<board-ip>`,密码见内部记录(本仓库不保存)。
2. 记录环境:`uname -a`、`free -m`、`cat /proc/cpuinfo | head -20`、`uptime`。
3. **清空负载**:关掉浏览器、Minetest、OpenTTD 等;`ps w` 确认没有大进程。后台任务会让 7-Zip 掉到 54 MIPS(见上表),这是最常见的"跑分异常"。
4. 板上 `scp` 是坏的(dropbear),传文件用管道:`cat 文件 | ssh root@<ip> "cat > /目标路径"`。

### 5.2 一键跑分(推荐)

板上已内置:

```sh
/usr/bin/bench
```

等价于:

```sh
/usr/bin/pi_css5 1048576     # SuperPI 1M
/usr/bin/7zr b               # 7-Zip 内置 benchmark
```

### 5.3 结果落盘

```sh
mkdir -p /root/bench
/usr/bin/bench > /root/bench/$(date +%F-%H%M).txt 2>&1
sync
```

取回本机(本机执行):

```sh
ssh root@<ip> "cat /root/bench/<文件名>" > ./<文件名>
```

### 5.4 判读规则

- **7-Zip**:看最后一行 `Tot:` 的**第三列**(综合 Rating,MIPS);同时记录 `Avr:` 行的 CPU 占用(<95% 说明有干扰)。
  表格含义:`Avr/Tot` 三列 = 压缩 MIPS / 解压 MIPS / 综合 Rating;`Dict 22/23` 分别对应 16 MB / 8 MB 字典的两轮。
- **SuperPI**:记 `Total real time` 的秒数,四舍五入到 0.01 s。
- 每个项目跑 ≥2 次,取干净环境下最稳定的一次;两次差异 >10% 时先查后台负载再下结论。

### 5.5 对照机(Windows)怎么跑

```bat
:: 7-Zip 24.09,默认参数
"C:\Program Files\7-Zip\7z.exe" b
:: 输出 Tot 行;若单位是 GIPS,则 ×1000 换成 MIPS

:: SuperPI(与本仓库同源码编译)
pi_css5_x64.exe 1048576
```

`~/Downloads/pi_css5_x64.exe` 就是当时给对照机编的版本(md5 `dffe4dced65d137eaee617814e5d0043`),用同一份源码,秒数可直接比。

### 5.6 板上重新部署二进制(镜像里没有时)

```sh
# 本机 → 板子(scp 坏了,用管道)
cat board/intel/galileo/rootfs_overlay_nvidia/usr/bin/pi_css5 | ssh root@<ip> "cat > /usr/bin/pi_css5"
cat board/intel/galileo/rootfs_overlay_nvidia/usr/bin/7zr     | ssh root@<ip> "cat > /usr/bin/7zr"
cat board/intel/galileo/rootfs_overlay_nvidia/usr/bin/bench   | ssh root@<ip> "cat > /usr/bin/bench"
ssh root@<ip> "chmod +x /usr/bin/pi_css5 /usr/bin/7zr /usr/bin/bench"
```

## 6. 已知坑

- **后台负载**是首要误差源:`Tot` 从 111 掉到 54 只需要一个编译/浏览器进程。
- **版本不同**:板上 p7zip 16.02、对照机 24.09;新版算法有优化,同机对比时务必注明版本。
- **单位**:7-Zip 24.09 Windows 显示 GIPS,旧版/`7zr` 显示 MIPS,别把 1.6 GIPS 当成 1.6 MIPS。
- **单核**:`7zr b` 只会用 1 线程(`# Benchmark threads: 1`),对照机多核要除以线程数再做单核比较。
- **内存**:可用 203 MB,`7zr b` 默认吃 111 MB;别在跑分时开 swap 压力测试。
- **SSE2 缺失**:7-Zip 走的是 C 回退路径,这也是 111 MIPS 这么低的主因之一;`pi_css5` 用 x87 标量 FFT。
- **SuperPI 不要用 Kanada 原版做跨平台对比**——那是闭源 Windows 专用,数字不可比。

## 7. 复现记录

| 日期 | 镜像 | 7-Zip Tot | SuperPI 1M | 备注 |
|---|---|---|---|---|
| 2026-10-30 | `galileo_nvidia`(TITAN X / nouveau) | 111 MIPS | 306.63 s | 干净环境,99% CPU |
| 2026-10-30 | 同上 | 109 MIPS | — | 复测 |
| 2026-10-30 | 同上 | 54 MIPS | — | 有后台任务,作废 |

## 8. 相关文件

- 跑分工具与启动器:`board/intel/galileo/rootfs_overlay_nvidia/usr/bin/{bench,pi_css5,7zr}`
- 对应提交:`ac39ecf` "nvidia image: include SuperPI (pi_css5) + 7-Zip (7zr) + bench launcher"
- p7zip 包定义:`package/p7zip/`

## 9. GPU 加速后的 2D / 3D 基线（2026-11-05，RX 580 + radeonsi）

环境：Galileo Gen 2 (Quark X1000 400 MHz / 203 MB)，RX 580 2048SP，
内核 5.10.29（amdgpu + Display Core 3.2.104，`dmesg` 有 `Display Core initialized`），Mesa 20.2.2 + LLVM 9.0.1，
`glxinfo`：`Accelerated: yes`、`GL 4.6 (Compatibility Profile)`、
`AMD POLARIS10 (DRM 3.40.0...)`，Xorg `glamor initialized`，显示 1280×1024。

驱动是热拷贝进镜像的（未重刷）：
`radeonsi_dri.so`、`libLLVM-9.so`、`libdrm_amdgpu.so.1`、`libdrm_nouveau.so.2`。

### 3D — glmark2（`--fullscreen`，默认套件）

| 场景 | FPS | 场景 | FPS |
|---|---|---|---|
| build (use-vbo) | 130–136 | effect2d (kernel) | 114–131 |
| texture (filter) | 137 | pulsar | 107 |
| shading (gouraud/phong/cel) | 122–138 | desktop (blur/shadow) | 53–54 |
| bump (high-poly/normals/height) | 123–137 | buffer (map/subdata) | 5–10 |

**glmark2 Score: 101**

解读：GPU 侧帧率普遍 120–137 FPS，`desktop`/`buffer` 两个场景掉到 5–54 FPS ——
瓶颈是 400 MHz Quark 的 CPU 提交与纹理更新，不是 GPU。

### 2D — gtkperf（`-a`，应用级）

| 项目 | 软件渲染 | GPU（glamor） |
|---|---|---|
| Total | 588.23 s | **320.64 s** |
| GtkComboBox | 38.10 | 22.68 |
| GtkComboBoxEntry | 27.60 | 15.67 |
| GtkTextView - Add text | 32.91 | 14.75 |
| GtkTextView - Scroll | 17.48 | 9.29 |
| GtkDrawingArea - Lines | 272.33 | 97.31 |
| GtkDrawingArea - Circles | 145.77 | 102.61 |
| GtkDrawingArea - Text | 30.29 | 24.26 |
| GtkDrawingArea - Pixbufs | 4.07 | 10.07 |

GTK 的绘制大头在 cairo 的 CPU 光栅化（image backend），glamor 只加速最后的
blit/合成，所以总时间约 1.8×，不会更接近 0。

### 2D — x11perf（X 原语，最直接反映 glamor）

| 测试 | 软件（ShadowFB） | GPU（glamor） | 提升 |
|---|---|---|---|
| `-rect500` | 13.4 /s | **1330 /s** | 99× |
| `-copywinwin500` | 10.9 /s | **143 /s** | 13× |
| `-compwinwin500` | — | **139 /s** | — |
| `-putimage500` | — | **9.5 /s** | 瓶颈在客户端→服务端上传 |

### 复现

```sh
DISPLAY=:0 glmark2 --fullscreen
DISPLAY=:0 gtkperf -a
DISPLAY=:0 x11perf -rect500 -copywinwin500 -compwinwin500 -putimage500 -repeat 20
```

### CPU — SuperPI / 7-Zip（同一镜像，GPU 加速后复测）

| 项目 | 结果 | 对照 |
|---|---|---|
| SuperPI 1M (`pi_css5 1000000`) | **302.67 s** | nvidia 镜像 306.63 s（同一 CPU，属噪声范围） |
| 7-Zip `7zr b` 压缩 | 95 KiB/s，95 MIPS，rating 91 | — |
| 7-Zip `7zr b` 解压 | 1421 KiB/s，127 MIPS，rating 121 | — |
| 7-Zip Tot | **112 MIPS**（历史记录 111） | 单线程，CPU 受限 |

说明：这两项只测 CPU，与 GPU/radeonsi 无关；放在这里是为了确认整机在
GL 4.6 + glamor 环境下没有因为驱动栈而退化。`7zr b` 固定单线程
（`# Benchmark threads: 1`），Tot 行的 112 是压缩/解压的合计。

终端窗口参数（录屏用）：`xterm -fa "Menlo:size=16" -geometry 80x30+120+180`；
镜像原本没有等宽字体（`fc-match Monospace` → DejaVu Sans 比例字体），
录屏前需把 Menlo 之类等宽字体放进 `/usr/share/fonts/` 并 `fc-cache -f`。

## 10. Doom 帧率：为什么 X 里只有 2 fps，改成 KMS 后有 ~25 fps（2026-11-07）

### 10.1 X11 (xlib) 后端：2 fps + 逐行刷新

镜像里原来的 `/root/doom/doomgeneric` 是 xlib 后端：`DG_Init` 调
`XOpenDisplay(NULL)` 后立即解引用返回值，所以旧启动器（先 `killall Xorg`）
开机第一帧就 segfault（`segfault at 84 ... ip 0806cda2`，即 `XOpenDisplay`
返回 NULL 后读 `+0x84`）。在 X 里跑起来之后：

| 观测 | 数值 |
|---|---|
| 每帧 X 流量 | 1280x1024x4 = **5.24 MB**（`XPutImage` 整窗，不是 320x200） |
| 客户端到 X 的吞吐上限 | **10.5 MB/s**（`/proc/<pid>/io` 的 `wchar`，与 `-scaling` 无关） |
| 实际帧率 | 10.5 / 5.24 = **2.0 fps** |
| 对照 `x11perf -putimage500` | 1 MB / 192.9 ms ≈ 5.2 MB/s |

`-scaling 1/2` 只改绘制内容不改流量，所以帧率恒为 2 fps；`-scaling 3/4`
会 segfault（rc=139）。

"从上到下一行行刷"的来源：一帧 5.24 MB 要写 500 ms，HDMI-1 是
1280x1024@120 Hz（每 8.3 ms 扫一遍），写缓冲期间显示器已经扫了 60 次，
看到的就是从上往下推进的刷新带。`modesetting` 驱动不支持 `TearFree`
（加了会报 `(WW) modeset(0): Option "TearFree" is not used`），Polaris 也
用不了带 TearFree 的 radeon 驱动。

### 10.2 KMS 后端：320x200 → GPU 放大到 1280x800，~25 fps

`board/intel/galileo/doomgeneric/doomgeneric_kms.c`（基于 doomgeneric
`doomgeneric_linuxvt.c`，换掉 framebuffer 部分）：

- 画进 320x200 XRGB8888 dumb buffer，**每帧 CPU 只写 256 KB**；
- 主 plane 的 `SRC=320x200 → CRTC=1280x800`（保持 16:10，上下黑边），
  放大是显示控制器（DCN plane scaler）做的，**CPU 零成本**，而且有双线性
  过滤，所以看起来比原来的整数 2x 邻域放大平滑；
- 3 个 buffer 轮转 + `drmModePageFlip`（`PAGE_FLIP_EVENT`）→ vblank 整帧切换，无撕裂。

| 配置 | 每帧 CPU 写入 | 实测帧率 |
|---|---|---|
| 320x200 → GPU 放大 1280x800 | 256 KB | **23–28 fps** |
| 640x400 → GPU 放大 1280x800 | 1 MB（外加 `i_scale` 软件放大） | **9 fps** |
| 1280x1024（X 里整窗 XPutImage） | 5.24 MB | **2 fps** |

Doom 的循环本身限速 35 Hz，所以 320x200 这一档已经接近上限；剩下的时间
花在 256 KB 非缓存 framebuffer 写入（板上实测约 12 MB/s）和 Doom 自身渲染。

### 10.3 编译与启动

```sh
docker run --rm -v gres-out:/out -v "$PWD":/src debian:12 bash -c '
  export PATH=/out/workstation/host/bin:$PATH
  SYS=/out/workstation/host/i586-buildroot-linux-gnu/sysroot
  cd /src/doomgeneric                      # 上游 git clone 后放进 doomgeneric_kms.c
  SRCS=$(sed -n "s/^SRC_DOOM = //p" Makefile.linuxvt | sed "s/\.o/.c/g" |
         sed "s/doomgeneric_linuxvt\.c/doomgeneric_kms.c/")
  i586-buildroot-linux-gnu-gcc -O2 -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
    -I$SYS/usr/include -I$SYS/usr/include/libdrm \
    -o /src/doomgeneric-kms $SRCS -L$SYS/usr/lib -ldrm -lm'
```

`/usr/bin/doom` 会先停桌面（DRM master 是独占的），跑 `doomgeneric-kms`，
退出后 `/etc/init.d/S50desktop start` 把 X + LXDE 整个拉回来。
`doomgeneric-kms` 每 5 s 往 stdout 打一行 `kms: NN.N fps`（按 page flip 计数）。

## 11. minplayer 4K 播放实测（2026-11-14，RX 580 + DC，scanout 2560x1440@60）

`minplayer-demo <file> <secs>`：X 停掉后走 VA-API → dmabuf → EGL → KMS page flip，
结果读 `/root/minplayer-result.txt`；`connector 80 crtc 54 mode 2560x1440@60`。

| 源 | 稳态 | CPU |
|---|---|---|
| `bbb_hw.mp4` 640×360 H.264 30p | **30.0 fps**（150 帧/5 s，卡在源帧率） | 27% |
| `bbb_4k30_30s.mp4` 3840×2160 H.264 30p | **30.0 fps**（900 帧/30 s，满帧） | 46% |
| `Bad Apple … v4.mp4` 3840×2160 HEVC 60p | **27.4 fps**（411 帧/15 s） | 80% |

两个坑：

- **别用 5 秒窗口测**。日志第一行就是 `render 840 ms`（EGL/BO/着色器冷启动），
  5 秒平均会被首帧吃掉三分之一，4K60 会误测成 14.6 fps；15 秒才是稳态 27.4。
- 4K30 能满帧、4K60 只有 ~27 fps（约 45% 实时），瓶颈是**每帧 CPU**（4K 下 ~50 ms/帧），
  不是解码器：同一份 4K 换成 H.264 也只在 30 fps 档，HEVC 60p 才压不住。

## 12. 4K 30 / 60 帧的编码选择（2026-11-14，RX 580 + VA-API，2560x1440@60 scanout）

同一段 Bad Apple（4K）分别用 H.264 与 HEVC 编成 30p / 60p，走 `minplayer-demo`
（VA-API → dmabuf → EGL → page flip），板上实测：

| 文件 | 编码 | 8–10 s 出帧 | fps | 相对实时 | CPU |
|---|---|---|---|---|---|
| 4K30 | H.264 High | 300/10 s | **30.0** | **100%** | 38% |
| 4K30 | HEVC（x265，32/64 CTU） | 202–215/8 s | 25–27 | 84–90% | 70% |
| 4K60 | H.264 High | 396/10 s | 39.6 | 66% | 47% |
| 4K60 | HEVC（原 v4） | 356/8 s | 44.5 | 74% | 55% |

结论：

- **能满帧的只有 4K30**，而且要用 H.264：每帧预算 33 ms，H.264 解 4K 只要～13 ms CPU，
  HEVC 同一帧要多花 7–10 ms（UVD + 解析），刚好把 30 fps 顶掉。
- 60 fps 这一档 H.264 反而略慢于 HEVC（39.6 vs 44.5），说明 4K60 已经不是解码器选择问题，
  而是每帧固定开销（vaRenderPicture 5.4 ms + flip 3.2 ms + GLES ~5 ms）挤爆 16.7 ms 预算。
- 编码参数（`-preset ultrafast`）会把 CTU 降到 32×32，4K 帧的 CTU 数从 2040 涨到 8160，
  FFmpeg 侧解析开销随之上升；要压成本至少显式给 `-x265-params ctu=64`。
