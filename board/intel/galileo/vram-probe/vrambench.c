/* Measure CPU write bandwidth into a VRAM BO (CPU-mapped through BAR1).
 * Without PAT/MTRR the mapping is uncached, so this is the number that
 * decides whether CPU-side filling of VRAM is viable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>

int main(int argc, char **argv) {
	size_t mb = argc > 1 ? atoi(argv[1]) : 32;
	size_t sz = mb << 20;
	int fd = open("/dev/dri/card0", O_RDWR);
	amdgpu_device_handle dev; uint32_t ma, mi;
	if (amdgpu_device_initialize(fd, &ma, &mi, &dev)) { perror("init"); return 1; }
	struct amdgpu_bo_alloc_request req; memset(&req, 0, sizeof req);
	req.alloc_size = sz; req.phys_alignment = 4096; req.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
	amdgpu_bo_handle bo;
	if (amdgpu_bo_alloc(dev, &req, &bo)) { perror("alloc"); return 1; }
	void *p = NULL;
	if (amdgpu_bo_cpu_map(bo, &p)) { perror("cpu_map"); return 1; }
	char *src = malloc(sz); memset(src, 0x5a, sz);
	struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
	memcpy(p, src, sz);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("VRAM write: %zu MB in %.2fs = %.1f MB/s\n", mb, s, mb / s);
	/* read back */
	volatile unsigned char sink = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (size_t i = 0; i < sz; i += 4096) sink += ((unsigned char *)p)[i];
	clock_gettime(CLOCK_MONOTONIC, &t1);
	s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("VRAM read: %zu MB in %.2fs = %.1f MB/s (sink=%u)\n", mb, s, mb / s, sink);
	amdgpu_bo_cpu_unmap(bo);
	return 0;
}
