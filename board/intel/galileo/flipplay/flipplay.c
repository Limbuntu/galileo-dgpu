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
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
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

static int read_full(int fd, void *buf, size_t n) {
	size_t off = 0;
	while (off < n) {
		ssize_t r = read(fd, (char *)buf + off, n - off);
		if (r < 0) { if (errno == EINTR) continue; return -1; }
		if (r == 0) return off ? -2 : 0;
		off += (size_t)r;
	}
	return 1;
}

/* UDP framed transport: [u32 frame][u32 idx][u32 total][u32 len] + payload */
static int udp_setup(int port) {
	int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1, rcv = 4 << 20;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
	struct timeval tv = { .tv_sec = 0, .tv_usec = 40000 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	struct sockaddr_in a; memset(&a, 0, sizeof a);
	a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
	if (bind(s, (struct sockaddr *)&a, sizeof a)) { perror("udp bind"); return -1; }
	return s;
}

static void udp_send_nack(int s, struct sockaddr_in *to, uint32_t frame,
			  unsigned char *got, uint32_t total) {
	unsigned char buf[2048];
	uint32_t *h = (uint32_t *)buf;
	h[0] = 0x4b43414e;   /* "NACK" little endian */
	h[1] = frame;
	uint32_t cnt = 0;
	uint16_t *idx = (uint16_t *)(buf + 12);
	for (uint32_t i = 0; i < total && cnt < 900; i++)
		if (!got[i]) idx[cnt++] = (uint16_t)i;
	h[2] = cnt;
	sendto(s, buf, 12 + cnt * 2, 0, (struct sockaddr *)to, sizeof *to);
}

static int udp_recv_frame(int s, unsigned char *dst, size_t fsz, size_t payload) {
	static unsigned char pkt[2048];
	static unsigned char got[2048];
	uint32_t want = 0xffffffff, total = 0, have = 0;
	struct sockaddr_in from; socklen_t fl = sizeof from;
	int nacks = 0;
	memset(&from, 0, sizeof from);
	for (;;) {
		ssize_t r = recvfrom(s, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
		if (r < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (want == 0xffffffff) return -2;      /* mid-frame / nothing */
				if (nacks < 12) { udp_send_nack(s, &from, want, got, total); nacks++; continue; }
				return -2;                              /* give up on this frame */
			}
			return -1;
		}
		if (r < 16) continue;
		uint32_t *h = (uint32_t *)pkt;
		uint32_t fr = h[0], idx = h[1], tot = h[2], len = h[3];
		if (len > (uint32_t)(r - 16) || len > payload || tot > 2048) continue;
		if (want == 0xffffffff) { want = fr; total = tot; have = 0; memset(got, 0, sizeof got); }
		if (fr != want) continue;
		size_t off = (size_t)idx * payload;
		if (off + len > fsz) continue;
		if (!got[idx]) { got[idx] = 1; have++; }
		memcpy(dst + off, pkt + 16, len);
		if (have >= total) return 1;
	}
}

static void memsnap(const char *tag) {
	FILE *m = fopen("/proc/meminfo", "r"); char line[128];
	long fr = -1, ca = -1, sl = -1, pt = -1, sh = -1;
	while (m && fgets(line, sizeof line, m)) {
		if (!strncmp(line, "MemFree:", 8)) fr = atol(line + 8);
		else if (!strncmp(line, "Cached:", 7)) ca = atol(line + 7);
		else if (!strncmp(line, "Slab:", 5)) sl = atol(line + 5);
		else if (!strncmp(line, "PageTables:", 11)) pt = atol(line + 11);
		else if (!strncmp(line, "Shmem:", 6)) sh = atol(line + 6);
	}
	if (m) fclose(m);
	fprintf(stderr, "  <%s> free=%ld cached=%ld slab=%ld pgtbl=%ld shmem=%ld\n", tag, fr, ca, sl, pt, sh);
	fflush(stderr);
}

static int flip_done;
static void flip_handler(int fd, unsigned int frame, unsigned int sec,
			 unsigned int usec, void *data) {
	(void)fd; (void)frame; (void)sec; (void)usec;
	*(int *)data = 1;
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

	FILE *f = NULL;
	int udp_fd = -1;
	if (!strncmp(path, "udp:", 4)) {
		udp_fd = udp_setup(atoi(path + 4));
		if (udp_fd < 0) return 1;
		fprintf(stderr, "listening on udp/%d\n", atoi(path + 4));
		if (tty) { fprintf(tty, "waiting for udp stream on port %s...\n", path + 4); fflush(tty); }
	} else if (!strncmp(path, "tcp:", 4)) {
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

	int cap = 4096, n = 0, skipped = 0, idle = 0;
	uint32_t *fbs = calloc(cap, sizeof(uint32_t));
	amdgpu_bo_handle *bos = calloc(cap, sizeof(*bos));
	long long t0 = now_ns();
	for (;;) {
		if (n >= cap) break;
		int k = n & 3;
		int rd;
		if (udp_fd >= 0) {
			rd = udp_recv_frame(udp_fd, pstg[k], fsz, 1400);
			if (rd == -2) {
				skipped++; idle++;
				/* UDP has no EOF: after ~9 s of silence, start playing what we have */
				if (idle > 30 && n > 1) break;
				continue;
			}
			idle = 0;
		} else rd = read_full(fileno(f), pstg[k], fsz);
		if (rd != 1) break;
		amdgpu_bo_handle bo = bo_alloc(AMDGPU_GEM_DOMAIN_VRAM, fsz, NULL);
		if (!bo) { perror("vram alloc"); break; }
		uint64_t va = 0x100000000ULL + (uint64_t)n * 0x200000ULL;
		if (amdgpu_bo_va_op(bo, 0, fsz, va, 0, AMDGPU_VA_OP_MAP)) { perror("va"); break; }
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
			FILE *m = fopen("/proc/meminfo", "r"); char line[128];
			long fr = -1, ca = -1, sl = -1, pt = -1, sh = -1;
			while (m && fgets(line, sizeof line, m)) {
				if (!strncmp(line, "MemFree:", 8)) fr = atol(line + 8);
				else if (!strncmp(line, "Cached:", 7)) ca = atol(line + 7);
				else if (!strncmp(line, "Slab:", 5)) sl = atol(line + 5);
				else if (!strncmp(line, "PageTables:", 11)) pt = atol(line + 11);
				else if (!strncmp(line, "Shmem:", 6)) sh = atol(line + 6);
			}
			if (m) fclose(m);
			fprintf(stderr, "  [%d] free=%ldkB cached=%ldkB slab=%ldkB pgtbl=%ldkB shmem=%ldkB\n",
				n, fr, ca, sl, pt, sh);
			if (n % 300 == 0) {
				int c = open("/proc/sys/vm/drop_caches", O_WRONLY);
				if (c >= 0) { if (write(c, "1", 1) < 0) {} close(c); }
			}
		}
		if (n % 30 == 0) {
			fprintf(stderr, "  loaded %d frames...\n", n);
			if (tty) { fprintf(tty, "loading %d frames...\n", n); fflush(tty); }
		}
	}
	double ldt = (now_ns() - t0) / 1e9;
	fprintf(stderr, "loaded %d frames (%.0f MB) in %.1fs, skipped %d incomplete\n", n, n * fsz / 1048576.0, ldt, skipped);
	if (tty) { fprintf(tty, "loaded %d frames - press any key to return\n", n); fflush(tty); }
	if (n < 2) { fprintf(stderr, "need at least 2 frames\n"); return 1; }

	/* free the page cache the network stream left behind before playback */
	{ int c = open("/proc/sys/vm/drop_caches", O_WRONLY); if (c >= 0) { if (write(c, "3", 1) < 0) {} close(c); } }

	memsnap("before setcrtc");
	if (drmModeSetCrtc(fd, crtc_id, fbs[0], 0, 0, &conn_id, 1, &mode)) perror("setcrtc");
	memsnap("after setcrtc");
	drmEventContext evctx; memset(&evctx, 0, sizeof evctx);
	evctx.version = 2;
	evctx.page_flip_handler = flip_handler;

	long long period = (long long)(1e9 / fps), start = now_ns();
	int i = 0, busy = 0;
	for (;;) {
		if (seconds > 0 && (now_ns() - start) / 1e9 >= seconds) break;
		long long target = start + (long long)i * period;
		long long d = target - now_ns();
		if (d > 0) usleep((useconds_t)(d / 1000));
		else if (d < -period) i += (int)((-d) / period);
		flip_done = 0;
		if (drmModePageFlip(fd, crtc_id, fbs[i % n], DRM_MODE_PAGE_FLIP_EVENT, &flip_done) == 0) {
			while (!flip_done) drmHandleEvent(fd, &evctx);   /* wait for vblank */
			i++;
			if (i == 1) memsnap("after flip 1");
			else if (i == 10) memsnap("after flip 10");
			else if (i == 60) memsnap("after flip 60");
		} else { busy++; usleep(2000); }
	}
	fprintf(stderr, "played %d flips, %d busy\n", i, busy);
	return 0;
}
