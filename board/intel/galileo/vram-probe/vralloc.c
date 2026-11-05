/* Allocate N VRAM BOs (no CPU mapping) and report how much VRAM the driver
 * accepts. Tests whether "frames live in VRAM" is even possible on this card. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>

int main(int argc, char **argv) {
	int n = argc > 1 ? atoi(argv[1]) : 1800;
	size_t sz = argc > 2 ? (size_t)atol(argv[2]) : 1280 * 960;
	int fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	amdgpu_device_handle dev;
	uint32_t maj, min;
	if (amdgpu_device_initialize(fd, &maj, &min, &dev)) { perror("amdgpu_init"); return 1; }
	amdgpu_bo_handle *bos = calloc(n, sizeof(*bos));
	struct amdgpu_bo_alloc_request req;
	int ok = 0;
	for (int i = 0; i < n; i++) {
		memset(&req, 0, sizeof(req));
		req.alloc_size = sz;
		req.phys_alignment = 4096;
		req.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
		if (amdgpu_bo_alloc(dev, &req, &bos[i])) { printf("alloc failed at %d (%.0f MB)\n", i, i * sz / 1048576.0); break; }
		ok++;
	}
	printf("allocated %d x %zu B = %.0f MB\n", ok, sz, (double)ok * sz / 1048576.0);
	FILE *f = fopen("/sys/class/drm/card0/device/mem_info_vram_used", "r");
	if (f) { char b[64]; if (fgets(b, sizeof b, f)) printf("vram_used=%s", b); fclose(f); }
	fflush(stdout);
	sleep(8);
	return 0;
}
