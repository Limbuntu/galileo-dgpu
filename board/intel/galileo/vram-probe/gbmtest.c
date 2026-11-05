/* Do GBM buffers (the ones Mesa/EGL render into) live in VRAM? */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <gbm.h>

static long vram(void) {
	FILE *f = fopen("/sys/class/drm/card0/device/mem_info_vram_used", "r");
	long v = -1; if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); } return v;
}

int main(int argc, char **argv) {
	int n = argc > 1 ? atoi(argv[1]) : 200;
	int fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	struct gbm_device *dev = gbm_create_device(fd);
	if (!dev) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
	printf("backend: %s\n", gbm_device_get_backend_name(dev));
	long v0 = vram();
	struct gbm_bo **bos = calloc(n, sizeof(*bos));
	int ok = 0;
	for (int i = 0; i < n; i++) {
		bos[i] = gbm_bo_create(dev, 640, 480, GBM_FORMAT_ARGB8888,
				       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		if (!bos[i]) { printf("gbm_bo_create failed at %d\n", i); break; }
		ok++;
	}
	printf("created %d bo x %u B, vram delta = %.0f MB (v0=%ld v1=%ld)\n",
	       ok, ok ? gbm_bo_get_stride(bos[0]) : 0, (vram() - v0) / 1048576.0, v0, vram());
	fflush(stdout);
	sleep(5);
	return 0;
}
