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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <gbm.h>
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
	FILE *f;
	if (!strncmp(path, "tcp:", 4)) {
		int port = atoi(path + 4);
		int srv = socket(AF_INET, SOCK_STREAM, 0), one = 1;
		setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		int rcv = 1 << 20;
		setsockopt(srv, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
		if (bind(srv, (struct sockaddr *)&a, sizeof(a)) || listen(srv, 1)) { perror("bind/listen"); return 1; }
		fprintf(stderr, "listening on tcp/%d\n", port);
		if (tty) { fprintf(tty, "waiting for stream on port %d...\n", port); fflush(tty); }
		int c = accept(srv, NULL, NULL);
		if (c < 0) { perror("accept"); return 1; }
		f = fdopen(c, "rb");
		fprintf(stderr, "stream connected\n");
	} else if (!strcmp(path, "-")) {
		f = stdin;
	} else {
		f = fopen(path, "rb");
		if (!f) { perror(path); return 1; }
	}

	int cap = 4096, n = 0;
	uint32_t *fbs = calloc(cap, sizeof(uint32_t));
	long long t0 = now_ns();
	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
	fprintf(stderr, "gbm backend: %s\n", gbm_device_get_backend_name(gbm));
	struct gbm_bo **bos = calloc(cap, sizeof(*bos));

	while (n < cap && fread(frame, 1, fsz, f) == fsz) {
		/* Legacy (non-DC) KMS cannot scan out tiled buffers: ask for linear. */
		struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ARGB8888,
						  GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
		if (!bo) { perror("gbm_bo_create"); break; }
		uint32_t stride = 0;
		void *map_data = NULL;
		void *p = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
		if (!p) { perror("gbm_bo_map"); break; }
		memcpy(p, frame, (size_t)stride < fsz ? (size_t)stride * H : fsz);
		gbm_bo_unmap(bo, map_data);
		uint32_t handle = gbm_bo_get_handle(bo).u32;
		uint32_t hd[4] = { handle }, pt[4] = { stride }, of[4] = { 0 };
		if (drmModeAddFB2(fd, W, H, DRM_FORMAT_ARGB8888, hd, pt, of, &fbs[n], 0)) {
			perror("addfb2"); break;
		}
		bos[n] = bo;
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
	for (;;) {
		if (seconds > 0 && (now_ns() - start) / 1e9 >= seconds) break;
		long long target = start + (long long)i * period;
		long long d = target - now_ns();
		if (d > 0) {
			usleep((useconds_t)(d / 1000));
		} else if (d < -period) {
			/* behind schedule: skip frames instead of bursting */
			i += (int)((-d) / period);
		}
		if (drmModePageFlip(fd, crtc_id, fbs[i % n], 0, NULL) == 0) {
			i++;
		} else {
			busy++;
			usleep(2000);   /* queue full: wait for the pending flip */
		}
	}
	fprintf(stderr, "played %d flips, %d busy\n", i, busy);
	return 0;
}
