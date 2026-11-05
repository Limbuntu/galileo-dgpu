/* flipplay: play pre-baked RGB frames by page-flipping KMS framebuffers.
 * Frames are copied into VRAM-backed dumb buffers ONCE at load time; the
 * playback loop only issues drmModePageFlip, so per-frame CPU is ~0.
 * usage: flipplay FILE W H [seconds] [fps]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static long long now_ns(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: %s FILE W H [seconds] [fps]\n", argv[0]); return 2; }
	const char *path = argv[1];
	int W = atoi(argv[2]), H = atoi(argv[3]);
	double seconds = argc > 4 ? atof(argv[4]) : 10.0;
	double fps = argc > 5 ? atof(argv[5]) : 30.0;

	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("/dev/dri/card0"); return 1; }
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) { fprintf(stderr, "no resources\n"); return 1; }

	uint32_t conn_id = 0, crtc_id = 0;
	drmModeModeInfo mode;
	memset(&mode, 0, sizeof(mode));
	for (int i = 0; i < res->count_connectors && !conn_id; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c) continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes) {
			int pick = 0;
			for (int j = 0; j < c->count_modes; j++)
				if (c->modes[j].hdisplay == W && c->modes[j].vdisplay == H) { pick = j; break; }
			drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
			if (e) {
				memcpy(&mode, &c->modes[pick], sizeof(mode));
				conn_id = c->connector_id; crtc_id = e->crtc_id;
				drmModeFreeEncoder(e);
			}
		}
		drmModeFreeConnector(c);
	}
	if (!conn_id) { fprintf(stderr, "no connected connector with a mode\n"); return 1; }
	fprintf(stderr, "connector %u crtc %u mode %dx%d\n", conn_id, crtc_id, mode.hdisplay, mode.vdisplay);

	FILE *tty = fopen("/dev/tty1", "w");   /* progress is visible on the VT */
	size_t fsz = (size_t)W * H * 4;
	unsigned char *frame = malloc(fsz);
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }

	int cap = 4096, n = 0;
	uint32_t *fbs = calloc(cap, sizeof(uint32_t));
	long long t0 = now_ns();
	while (n < cap && fread(frame, 1, fsz, f) == fsz) {
		struct drm_mode_create_dumb cr = { .width = W, .height = H, .bpp = 32 };
		if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr)) { perror("create_dumb"); break; }
		struct drm_mode_map_dumb mr = { .handle = cr.handle };
		if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mr)) { perror("map_dumb"); break; }
		unsigned char *p = mmap(0, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mr.offset);
		if (p == MAP_FAILED) { perror("mmap"); break; }
		memcpy(p, frame, fsz);
		uint32_t hd[4] = { cr.handle }, pt[4] = { cr.pitch }, of[4] = { 0 };
		if (drmModeAddFB2(fd, W, H, DRM_FORMAT_ARGB8888, hd, pt, of, &fbs[n], 0)) {
			perror("addfb2"); break;
		}
		n++;
		if (n % 30 == 0) {
			fprintf(stderr, "  loading %d frames...\n", n);
			if (tty) { fprintf(tty, "loading %d frames...\n", n); fflush(tty); }
		}
	}
	fprintf(stderr, "loaded %d frames (%.0f MB) in %.1fs\n", n, n * fsz / 1048576.0, (now_ns() - t0) / 1e9);
	if (tty) { fprintf(tty, "loaded %d frames - press any key to return\n", n); fflush(tty); }
	if (n < 2) { fprintf(stderr, "need at least 2 frames\n"); return 1; }

	if (drmModeSetCrtc(fd, crtc_id, fbs[0], 0, 0, &conn_id, 1, &mode))
		perror("setcrtc");

	long long period = (long long)(1e9 / fps);
	long long start = now_ns();
	int i = 0, busy = 0;
	while (seconds <= 0 || (now_ns() - start) / 1e9 < seconds) {
		long long target = start + (long long)i * period;
		long long d = target - now_ns();
		if (d > 0) usleep((useconds_t)(d / 1000));
		if (drmModePageFlip(fd, crtc_id, fbs[i % n], 0, NULL))
			busy++;
		i++;
	}
	fprintf(stderr, "played %d flips, %d busy\n", i, busy);
	return 0;
}
