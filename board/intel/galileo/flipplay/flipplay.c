/* flipplay: play pre-baked RGB frames by page-flipping KMS framebuffers.
 *
 * Frames are streamed in (file, stdin, or tcp:PORT), copied into per-frame
 * VRAM BOs by the SDMA engine, and then presented by flipping.  The copy
 * path is what makes >256 MB usable: CPU can only map the first 256 MB of
 * VRAM (BAR1 window), but SDMA can write anywhere in VRAM.
 *
 * usage: flipplay FILE|tcp:PORT W H [seconds] [fps]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>

static long long now_ns(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static amdgpu_device_handle dev;
static amdgpu_context_handle ctx;
static uint64_t va_ib, va_stg[4];
static amdgpu_bo_handle stg[4], ibbo;
static void *pstg[4], *pib;

static amdgpu_bo_handle bo_alloc(uint32_t dom, size_t sz, void **cpu) {
	struct amdgpu_bo_alloc_request r; memset(&r, 0, sizeof r);
	r.alloc_size = sz; r.phys_alignment = 4096; r.preferred_heap = dom;
	amdgpu_bo_handle b;
	if (amdgpu_bo_alloc(dev, &r, &b)) return NULL;
	if (cpu && amdgpu_bo_cpu_map(b, cpu)) return NULL;
	return b;
}

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: %s FILE|tcp:PORT W H [seconds] [fps]\n", argv[0]); return 2; }
	const char *path = argv[1];
	int W = atoi(argv[2]), H = atoi(argv[3]);
	double seconds = argc > 4 ? atof(argv[4]) : 0;
	double fps = argc > 5 ? atof(argv[5]) : 30.0;

	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("/dev/dri/card0"); return 1; }
	FILE *tty = fopen("/dev/tty1", "w");

	drmModeRes *res = drmModeGetResources(fd);
	uint32_t conn_id = 0, crtc_id = 0;
	drmModeModeInfo mode; memset(&mode, 0, sizeof mode);
	for (int i = 0; i < res->count_connectors && !conn_id; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c) continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes) {
			int pick = 0;
			for (int j = 0; j < c->count_modes; j++)
				if (c->modes[j].hdisplay == W && c->modes[j].vdisplay == H) { pick = j; break; }
			drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
			if (e) { memcpy(&mode, &c->modes[pick], sizeof mode); conn_id = c->connector_id; crtc_id = e->crtc_id; drmModeFreeEncoder(e); }
		}
		drmModeFreeConnector(c);
	}
	if (!conn_id) { fprintf(stderr, "no connected connector\n"); return 1; }
	fprintf(stderr, "connector %u crtc %u mode %dx%d\n", conn_id, crtc_id, mode.hdisplay, mode.vdisplay);

	if (amdgpu_device_initialize(fd, (uint32_t[]){0}, (uint32_t[]){0}, &dev)) { perror("amdgpu init"); return 1; }
	if (amdgpu_cs_ctx_create(dev, &ctx)) { perror("ctx"); return 1; }

	size_t fsz = (size_t)W * H * 4;
	for (int k = 0; k < 4; k++) {
		stg[k] = bo_alloc(AMDGPU_GEM_DOMAIN_GTT, fsz, &pstg[k]);
		if (!stg[k]) { perror("stg"); return 1; }
		va_stg[k] = 0x800000000ULL + (uint64_t)k * 0x200000ULL;
		amdgpu_bo_va_op(stg[k], 0, fsz, va_stg[k], 0, AMDGPU_VA_OP_MAP);
	}
	ibbo = bo_alloc(AMDGPU_GEM_DOMAIN_GTT, 4096, &pib);
	va_ib = 0x900000000ULL;
	amdgpu_bo_va_op(ibbo, 0, 4096, va_ib, 0, AMDGPU_VA_OP_MAP);

	FILE *f;
	if (!strncmp(path, "tcp:", 4)) {
		int port = atoi(path + 4);
		int srv = socket(AF_INET, SOCK_STREAM, 0), one = 1, rcv = 1 << 20;
		setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		setsockopt(srv, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
		struct sockaddr_in a; memset(&a, 0, sizeof a);
		a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
		if (bind(srv, (struct sockaddr *)&a, sizeof a) || listen(srv, 1)) { perror("bind/listen"); return 1; }
		fprintf(stderr, "listening on tcp/%d\n", port);
		if (tty) { fprintf(tty, "waiting for stream on port %d...\n", port); fflush(tty); }
		int c = accept(srv, NULL, NULL);
		if (c < 0) { perror("accept"); return 1; }
		f = fdopen(c, "rb");
	} else if (!strcmp(path, "-")) {
		f = stdin;
	} else {
		f = fopen(path, "rb");
		if (!f) { perror(path); return 1; }
	}

	int cap = 4096, n = 0;
	uint32_t *fbs = calloc(cap, sizeof(uint32_t));
	amdgpu_bo_handle *bos = calloc(cap, sizeof(*bos));
	unsigned char *frame = malloc(fsz);
	long long t0 = now_ns();
	while (n < cap && fread(frame, 1, fsz, f) == fsz) {
		amdgpu_bo_handle bo = bo_alloc(AMDGPU_GEM_DOMAIN_VRAM, fsz, NULL);
		if (!bo) { perror("vram alloc"); break; }
		uint64_t va = 0x100000000ULL + (uint64_t)n * 0x200000ULL;
		if (amdgpu_bo_va_op(bo, 0, fsz, va, 0, AMDGPU_VA_OP_MAP)) { perror("va"); break; }
		int k = n & 3;
		memcpy(pstg[k], frame, fsz);
		uint32_t *ib = (uint32_t *)pib;
		ib[0] = 1; ib[1] = (uint32_t)fsz; ib[2] = 0;
		ib[3] = (uint32_t)va_stg[k]; ib[4] = (uint32_t)(va_stg[k] >> 32);
		ib[5] = (uint32_t)va;        ib[6] = (uint32_t)(va >> 32);
		amdgpu_bo_handle rs[3] = { ibbo, stg[k], bo };
		amdgpu_bo_list_handle list;
		if (amdgpu_bo_list_create(dev, 3, rs, NULL, &list)) break;
		struct amdgpu_cs_ib_info ii; memset(&ii, 0, sizeof ii);
		ii.ib_mc_address = va_ib; ii.size = 7 * 4;
		struct amdgpu_cs_request rq; memset(&rq, 0, sizeof rq);
		rq.ip_type = AMDGPU_HW_IP_DMA; rq.resources = list; rq.number_of_ibs = 1; rq.ibs = &ii;
		if (amdgpu_cs_submit(ctx, 0, &rq, 1)) { perror("submit"); break; }
		struct amdgpu_cs_fence fe; memset(&fe, 0, sizeof fe);
		fe.context = ctx; fe.ip_type = AMDGPU_HW_IP_DMA; fe.fence = rq.seq_no;
		uint32_t exp = 0;
		for (int t = 0; t < 500; t++) { if (!amdgpu_cs_query_fence_status(&fe, 5000000ULL, 0, &exp) && exp) break; }
		amdgpu_bo_list_destroy(list);
		uint32_t h = 0, pitch = (uint32_t)W * 4, off = 0;
		if (amdgpu_bo_export(bo, amdgpu_bo_handle_type_kms, &h)) { perror("bo_export"); break; }
		if (drmModeAddFB2(fd, W, H, DRM_FORMAT_ARGB8888, &h, &pitch, &off, &fbs[n], 0)) { perror("addfb2"); break; }
		bos[n] = bo;
		n++;
		if (n % 100 == 0) {
			FILE *m = fopen("/proc/meminfo", "r"); char k[64]; long v, fr = -1, sw = -1, sh = -1;
			while (m && fscanf(m, "%63s %ld", k, &v) == 2) {
				if (!strcmp(k, "MemFree:")) fr = v;
				if (!strcmp(k, "SwapFree:")) sw = v;
				if (!strcmp(k, "Shmem:")) sh = v;
			}
			if (m) fclose(m);
			fprintf(stderr, "  [%d] memfree=%ldkB swapfree=%ldkB shmem=%ldkB\n", n, fr, sw, sh);
		}
		if (n % 30 == 0) {
			fprintf(stderr, "  loaded %d frames...\n", n);
			if (tty) { fprintf(tty, "loading %d frames...\n", n); fflush(tty); }
		}
	}
	double ldt = (now_ns() - t0) / 1e9;
	fprintf(stderr, "loaded %d frames (%.0f MB) in %.1fs\n", n, n * fsz / 1048576.0, ldt);
	if (tty) { fprintf(tty, "loaded %d frames - press any key to return\n", n); fflush(tty); }
	if (n < 2) { fprintf(stderr, "need at least 2 frames\n"); return 1; }

	if (drmModeSetCrtc(fd, crtc_id, fbs[0], 0, 0, &conn_id, 1, &mode)) perror("setcrtc");

	long long period = (long long)(1e9 / fps), start = now_ns();
	int i = 0, busy = 0;
	for (;;) {
		if (seconds > 0 && (now_ns() - start) / 1e9 >= seconds) break;
		long long target = start + (long long)i * period;
		long long d = target - now_ns();
		if (d > 0) usleep((useconds_t)(d / 1000));
		else if (d < -period) i += (int)((-d) / period);
		if (drmModePageFlip(fd, crtc_id, fbs[i % n], 0, NULL) == 0) i++;
		else { busy++; usleep(2000); }
	}
	fprintf(stderr, "played %d flips, %d busy\n", i, busy);
	return 0;
}
