# VRAM probes

Two questions this answers, on the board:

1. Can we hold a whole clip in VRAM at all?
   `vralloc 1800 1228800` → allocated 2109 MB of VRAM BOs, `mem_info_vram_used`
   = 2.24 GB. So capacity is *not* the problem.

2. Do Mesa/EGL-rendered GBM buffers live in VRAM?
   `gbmtest 200` → 200 × 640×480 ARGB8888 buffers moved `vram_used` by 301 MB.
   They do. `gbm_bo_get_modifier()` must be passed to `AddFB2WithModifiers`.

## Why "VRAM" is still hard

* CPU-accessible VRAM is limited to BAR1: 256 MB (`mem_info_vis_vram_total`).
  amdgpu maps a BO at its VRAM offset, so anything allocated above the first
  256 MB cannot be `cpu_map`ped.
* Resizable BAR exists on the card (extended cap 0x0015, BAR index 0) but
  Linux 5.10 has no `resource*_resize` sysfs knob (added in 5.12) and the
  32-bit MMIO window cannot hold a 2 GB BAR anyway.
* Therefore frames must reach VRAM through the GPU: render into GBM BOs with
  GL (Mesa), or submit SDMA copies. Unimplemented so far.
