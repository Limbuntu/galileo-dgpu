/*
 * dosdrm - DRM/KMS probe and scaled-plane presenter for the Galileo DOS image.
 *
 * Build (host or buildroot staging):
 *   gcc -O2 -Wall -Wextra -std=gnu99 -o dosdrm dosdrm.c \
 *       -I/usr/include/libdrm -ldrm
 *
 * Why this exists: on this kernel amdgpu is built without Display Core
 * (CONFIG_DRM_AMD_DC=n, Quark has no SSE2), so the DRM driver is a legacy,
 * non-atomic KMS driver whose primary plane comes from drm_crtc_init()'s
 * compatibility helper - it cannot be scaled and must cover the whole CRTC.
 * The only zero-CPU upscale path on this image is therefore "CRTC mode ==
 * emulator frame, monitor scales".  This tool measures and selects that mode,
 * and it carries the GPU-plane upscaling prototype that becomes usable the
 * moment the kernel grows a scaler-capable plane (DC or a patched dce_v11_0).
 *
 * Commands:
 *   dosdrm probe                diagnose the pipeline; empirically test whether
 *                              a 320x200 fb can be scaled onto the active mode
 *   dosdrm scale [--src WxH]    present a small fb through the primary plane
 *                              (hardware upscale when supported), animated
 *                              test pattern, reports per-frame CPU cost
 *   dosdrm fit [--src WxH]      pick the connector mode to run the CRTC at so
 *                              that fb0 == emulator frame (monitor scales)
 *   dosdrm bench [--src WxH]    write bandwidth into the fb0 mapping vs. into a
 *                              DRM dumb buffer (per-frame CPU cost comparison)
 *
 * Exit codes: 0 ok, 1 usage, 2 no usable DRM device, 3 GPU scaling unsupported,
 *             4 ioctl failed.
 *
 * Plain ASCII only: this runs on a framebuffer console with no CJK glyphs.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include <drm_fourcc.h>		/* -I/usr/include/libdrm */
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DEFAULT_SRC_W 320
#define DEFAULT_SRC_H 200
#define DEFAULT_DEVICE "/dev/dri/card0"

struct disp {
	int fd;
	int master;
	uint32_t crtc_id;
	int crtc_index;
	drmModeCrtc *crtc;
	drmModeModeInfo mode;
	uint32_t conn_id;
	char conn_name[64];
	uint32_t plane_id;
	/* dumb buffer backing the small framebuffer */
	uint32_t fb_id;
	uint32_t handle;
	uint32_t pitch;
	uint64_t size;
	uint8_t *map;
	int sw;
	int sh;
	volatile sig_atomic_t stop;
};

static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
	va_list ap;

	fflush(stdout);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fflush(stderr);
}

static struct disp *g_disp;

static void on_stop(int sig)
{
	(void)sig;
	if (g_disp)
		g_disp->stop = 1;
	signal(sig, on_stop);
}

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int parse_size(const char *s, int *w, int *h)
{
	char *end;

	if (!s)
		return -1;
	errno = 0;
	*w = (int)strtol(s, &end, 10);
	if (errno || end == s || (*end != 'x' && *end != 'X'))
		return -1;
	errno = 0;
	*h = (int)strtol(end + 1, &end, 10);
	if (errno || *end != '\0' || *w <= 0 || *h <= 0 || *w > 8192 || *h > 8192)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ DRM --- */

static int open_card(const char *want)
{
	static const char *fallbacks[] = {
		"/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2", NULL
	};
	const char *env = getenv("DOSDRM_DEVICE");
	const char *path = want ? want : (env ? env : NULL);
	int fd, i;

	if (path) {
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			fail("dosdrm: open %s: %s\n", path, strerror(errno));
		return fd;
	}
	for (i = 0; fallbacks[i]; i++) {
		fd = open(fallbacks[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			return fd;
	}
	fail("dosdrm: no /dev/dri/cardN (%s)\n", strerror(errno));
	return -1;
}

/*
 * Find the CRTC that the kernel fbdev emulation already drives: it has a mode
 * and a connected connector.  Everything else (modes, panel timing) is left
 * exactly as the kernel/fbcon set it up, so this tool never re-times the
 * display unless asked to.
 */
static const char *conn_name(unsigned int type)
{
	/* libdrm 2.4.102 (the target's version) has no
	 * drmModeGetConnectorTypeName(), so keep a local table. */
	static const char *names[] = {
		"Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite",
		"SVIDEO", "LVDS", "Component", "DIN", "DP", "HDMI", "HDMI-B",
		"TV", "eDP", "Virtual", "DSI", "DPI"
	};

	return type < sizeof(names) / sizeof(names[0]) ? names[type] : "Unknown";
}

static int find_active_crtc(struct disp *d, drmModeRes *res)
{
	int i;

	for (i = 0; i < res->count_crtcs; i++) {
		drmModeCrtc *c = drmModeGetCrtc(d->fd, res->crtcs[i]);
		int j, found = 0;

		if (!c)
			continue;
		if (!c->mode_valid || c->buffer_id == 0) {
			drmModeFreeCrtc(c);
			continue;
		}
		for (j = 0; j < res->count_connectors; j++) {
			drmModeConnector *k = drmModeGetConnector(d->fd, res->connectors[j]);
			drmModeEncoder *enc;

			if (!k)
				continue;
			enc = k->encoder_id ? drmModeGetEncoder(d->fd, k->encoder_id)
					    : NULL;
			if (enc && enc->crtc_id == c->crtc_id &&
			    k->connection == DRM_MODE_CONNECTED) {
				found = 1;
				d->conn_id = k->connector_id;
				snprintf(d->conn_name, sizeof(d->conn_name),
					 "%s-%u", conn_name(k->connector_type),
					 k->connector_type_id);
			}
			if (enc)
				drmModeFreeEncoder(enc);
			drmModeFreeConnector(k);
			if (found)
				break;
		}
		if (found) {
			d->crtc = c;
			d->crtc_id = c->crtc_id;
			d->crtc_index = i;
			d->mode = c->mode;
			return 0;
		}
		drmModeFreeCrtc(c);
	}
	return -1;
}

static int plane_type_of(int fd, uint32_t id)
{
	drmModeObjectProperties *props;
	drmModePropertyRes *prop;
	unsigned int i;
	int type = -1;

	props = drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
	if (!props)
		return -1;
	for (i = 0; i < props->count_props; i++) {
		prop = drmModeGetProperty(fd, props->props[i]);
		if (!prop)
			continue;
		if (!strcmp(prop->name, "type"))
			type = (int)props->prop_values[i];
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
	return type;
}

static int plane_has_format(int fd, uint32_t id, uint32_t format)
{
	drmModePlane *p = drmModeGetPlane(fd, id);
	uint32_t i;
	int found = 0;

	if (!p)
		return 0;
	for (i = 0; i < p->count_formats; i++)
		if (p->formats[i] == format)
			found = 1;
	drmModeFreePlane(p);
	return found;
}

/* Primary plane usable on our CRTC, preferring one that can take XRGB8888. */
static uint32_t find_primary_plane(struct disp *d)
{
	drmModePlaneRes *pres;
	uint32_t i, fallback = 0;

	pres = drmModeGetPlaneResources(d->fd);
	if (!pres)
		return 0;
	for (i = 0; i < pres->count_planes; i++) {
		uint32_t id = pres->planes[i];
		drmModePlane *p = drmModeGetPlane(d->fd, id);
		int ok;

		if (!p)
			continue;
		ok = (p->possible_crtcs & (1u << d->crtc_index)) != 0;
		drmModeFreePlane(p);
		if (!ok)
			continue;
		if (plane_type_of(d->fd, id) != DRM_PLANE_TYPE_PRIMARY)
			continue;
		if (plane_has_format(d->fd, id, DRM_FORMAT_XRGB8888)) {
			fallback = id;
			break;
		}
		if (!fallback)
			fallback = id;
	}
	drmModeFreePlaneResources(pres);
	return fallback;
}

static int create_dumb(struct disp *d, int w, int h)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	int ret;

	memset(&creq, 0, sizeof(creq));
	creq.width = (uint32_t)w;
	creq.height = (uint32_t)h;
	creq.bpp = 32;
	/* Raw ioctl: libdrm 2.4.102 on the target has no
	 * drmModeCreateDumbBuffer()/MapDumbBuffer()/DestroyDumbBuffer(). */
	ret = drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret) {
		fail("dosdrm: create_dumb %dx%d: %s\n", w, h, strerror(errno));
		return -1;
	}
	d->handle = creq.handle;
	d->pitch = creq.pitch;
	d->size = creq.size;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = d->handle;
	ret = drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fail("dosdrm: map_dumb: %s\n", strerror(errno));
		return -1;
	}
	d->map = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      d->fd, (off_t)mreq.offset);
	if (d->map == MAP_FAILED) {
		d->map = NULL;
		fail("dosdrm: mmap dumb buffer: %s\n", strerror(errno));
		return -1;
	}

	handles[0] = d->handle;
	pitches[0] = d->pitch;
	offsets[0] = 0;
	ret = drmModeAddFB2(d->fd, (uint32_t)w, (uint32_t)h,
			    DRM_FORMAT_XRGB8888, handles, pitches, offsets,
			    &d->fb_id, 0);
	if (ret) {
		fail("dosdrm: addfb2 %dx%d pitch %u: %s\n", w, h, d->pitch,
		     strerror(errno));
		return -1;
	}
	d->sw = w;
	d->sh = h;
	return 0;
}

static void destroy_dumb(struct disp *d)
{
	if (d->fb_id) {
		drmModeRmFB(d->fd, d->fb_id);
		d->fb_id = 0;
	}
	if (d->map) {
		munmap(d->map, d->size);
		d->map = NULL;
	}
	if (d->handle) {
		struct drm_mode_destroy_dumb dreq;

		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = d->handle;
		drmIoctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		d->handle = 0;
	}
}

static int set_plane(struct disp *d, uint32_t fb, int x, int y, int w, int h,
		     int sx, int sy, int sw, int sh)
{
	return drmModeSetPlane(d->fd, d->plane_id, d->crtc_id, fb, 0,
			       x, y, (uint32_t)w, (uint32_t)h,
			       (uint32_t)sx << 16, (uint32_t)sy << 16,
			       (uint32_t)sw << 16, (uint32_t)sh << 16);
}

static void disp_close(struct disp *d)
{
	destroy_dumb(d);
	if (d->crtc) {
		drmModeFreeCrtc(d->crtc);
		d->crtc = NULL;
	}
	if (d->fd >= 0) {
		if (d->master)
			drmDropMaster(d->fd);
		close(d->fd);
		d->fd = -1;
	}
}

static const char *errno_hint(int err)
{
	switch (err) {
	case ERANGE:
		return "scaling factor outside the plane's [min,max] range "
		       "(DRM_PLANE_HELPER_NO_SCALING on the legacy primary plane)";
	case EINVAL:
		return "plane geometry rejected (destination must cover the CRTC, "
		       "or the driver refused the position)";
	case EOPNOTSUPP:
		return "driver has no atomic modeset support";
	case EACCES:
		return "not DRM master (run as root / another master holds the card)";
	case ENOSPC:
		return "source rectangle does not fit inside the framebuffer";
	default:
		return "ioctl failed";
	}
}

/* -------------------------------------------------------------- geometry --- */

/*
 * Source pixel aspect: VGA 320x200 (and its 640x400/720x400 relatives) is
 * displayed on a 4:3 raster, i.e. pixels are 1.2x taller than wide.  Scaling
 * to a 16:9 panel without this correction stretches the picture horizontally.
 */
static void source_extent(int sw, int sh, int *ew, int *eh)
{
	*ew = sw;
	*eh = sh;
	if (sh == 200 || sh == 400)
		*eh = sh * 6 / 5;
}

/* Largest w:h rect of the source's aspect that fits the mode, centred. */
static void fit_rect(int mw, int mh, int sw, int sh, int stretch,
		     int *rx, int *ry, int *rw, int *rh)
{
	int ew, eh;
	long long w, h;

	if (stretch) {
		*rx = 0;
		*ry = 0;
		*rw = mw;
		*rh = mh;
		return;
	}
	source_extent(sw, sh, &ew, &eh);
	if ((long long)mw * eh <= (long long)mh * ew) {
		w = mw;
		h = (long long)mw * eh / ew;
	} else {
		h = mh;
		w = (long long)mh * ew / eh;
	}
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	*rw = (int)w;
	*rh = (int)h;
	*rx = (mw - *rw) / 2;
	*ry = (mh - *rh) / 2;
}

/* ------------------------------------------------------------- rendering --- */

static inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
	return (r << 16) | (g << 8) | b;
}

static void put_px(struct disp *d, int x, int y, uint32_t px)
{
	uint8_t *row = d->map + (size_t)y * d->pitch;

	*(uint32_t *)(row + (size_t)x * 4) = px;
}

static void fill_rows(struct disp *d, int y0, int y1, uint32_t px)
{
	int y;

	for (y = y0; y < y1; y++) {
		uint8_t *row = d->map + (size_t)y * d->pitch;
		uint32_t *p = (uint32_t *)row;
		int x;

		for (x = 0; x < d->sw; x++)
			p[x] = px;
	}
}

/*
 * Test pattern: 8 vertical colour bars drifting horizontally, a moving white
 * band, a 1px red frame, and a 1:1 checkerboard patch in the top-left corner.
 * The checkerboard is 1 source pixel wide per cell: if the GPU/compositor is
 * scaling, he corners blur; if you see exactly 1-pixel cells, the buffer is
 * being scanned out 1:1 and nothing is scaled.
 */
static void draw_pattern(struct disp *d, unsigned frame)
{
	static const uint32_t bars[8] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0x000000
	};
	int x, y, bar_w = d->sw / 8;
	int shift = (int)(frame % (unsigned)(bar_w ? bar_w : 1));

	fill_rows(d, 0, d->sh, rgb(24, 24, 24));

	for (y = 0; y < d->sh; y++) {
		uint8_t *row = d->map + (size_t)y * d->pitch;
		uint32_t *p = (uint32_t *)row;

		for (x = 0; x < d->sw; x++) {
			int idx = ((x + shift) / (bar_w ? bar_w : 1)) & 7;

			p[x] = bars[idx];
		}
	}

	/* moving band: 1/8 of the height, bouncing */
	{
		int band_h = d->sh / 8;
		int span = d->sh - band_h;
		int period = span > 0 ? span * 2 : 1;
		int pos = (int)((frame * 2) % (unsigned)period);

		if (pos > span)
			pos = period - pos;
		fill_rows(d, pos, pos + band_h, rgb(255, 255, 255));
	}

	/* 1:1 checkerboard patch, 64x40 source pixels */
	for (y = 0; y < 40 && y < d->sh; y++)
		for (x = 0; x < 64 && x < d->sw; x++)
			put_px(d, x, y, (((x ^ y) & 1) ? rgb(255, 255, 255) : 0));

	/* red frame */
	for (x = 0; x < d->sw; x++) {
		put_px(d, x, 0, rgb(255, 0, 0));
		put_px(d, x, d->sh - 1, rgb(255, 0, 0));
	}
	for (y = 0; y < d->sh; y++) {
		put_px(d, 0, y, rgb(255, 0, 0));
		put_px(d, d->sw - 1, y, rgb(255, 0, 0));
	}
}

/* ------------------------------------------------------------ disp setup --- */

static int disp_open(struct disp *d, const char *device, int need_plane)
{
	drmModeRes *res;
	int ret;

	memset(d, 0, sizeof(*d));
	d->fd = -1;
	d->handle = 0;
	d->fb_id = 0;

	d->fd = open_card(device);
	if (d->fd < 0)
		return -2;

	ret = drmSetClientCap(d->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret)
		info("universal planes: %s\n", strerror(errno ? errno : ENOTSUP));

	res = drmModeGetResources(d->fd);
	if (!res) {
		fail("dosdrm: DRM resources: %s (not a KMS device?)\n", strerror(errno));
		disp_close(d);
		return -2;
	}
	if (find_active_crtc(d, res) < 0) {
		fail("dosdrm: no active CRTC with a mode and a connected connector.\n"
		     "        load amdgpu (modprobe amdgpu) / let fbcon take over first.\n");
		drmModeFreeResources(res);
		disp_close(d);
		return -2;
	}
	drmModeFreeResources(res);

	if (!need_plane)
		return 0;

	d->plane_id = find_primary_plane(d);
	if (!d->plane_id) {
		fail("dosdrm: no primary plane for CRTC %u\n", d->crtc_id);
		disp_close(d);
		return -2;
	}

	ret = drmSetMaster(d->fd);
	if (ret) {
		fail("dosdrm: drmSetMaster: %s (%s)\n", strerror(errno),
		     errno_hint(errno));
		disp_close(d);
		return -2;
	}
	d->master = 1;
	return 0;
}

/* ----------------------------------------------------------------- probe --- */

static int cmd_probe(const char *device)
{
	struct disp d;
	drmVersion *ver;
	uint64_t cap = 0;
	drmModePlaneRes *pres;
	unsigned int i;
	int ret, have_atomic = 0, exit_code = 3;

	if (disp_open(&d, device, 0) < 0)
		return 2;
	/* best effort: the plane test below reprograms the CRTC */
	if (drmSetMaster(d.fd) == 0)
		d.master = 1;
	else
		info("master      : not acquired (%s), plane test may fail with EACCES\n",
		     strerror(errno));

	info("device      : %s\n",
	     device ? device : (getenv("DOSDRM_DEVICE") ? getenv("DOSDRM_DEVICE")
						       : DEFAULT_DEVICE));
	ver = drmGetVersion(d.fd);
	if (ver) {
		info("driver      : %s %d.%d.%d\n", ver->name,
		     ver->version_major, ver->version_minor, ver->version_patchlevel);
		drmFreeVersion(ver);
	}
	if (drmGetCap(d.fd, DRM_CAP_DUMB_BUFFER, &cap) == 0)
		info("dumb buffer : %s\n", cap ? "yes" : "no");
	info("crtc        : %u (index %d), mode %dx%d@%d\n", d.crtc_id,
	     d.crtc_index, d.mode.hdisplay, d.mode.vdisplay, d.mode.vrefresh);
	info("connector   : %u (%s)\n", d.conn_id, d.conn_name[0] ? d.conn_name : "?");

	pres = drmModeGetPlaneResources(d.fd);
	if (pres) {
		for (i = 0; i < pres->count_planes; i++) {
			uint32_t id = pres->planes[i];
			int type = plane_type_of(d.fd, id);

			info("plane %-3u   : type %s, XRGB8888 %s\n", id,
			     type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" :
			     type == DRM_PLANE_TYPE_CURSOR ? "CURSOR" :
			     type == DRM_PLANE_TYPE_OVERLAY ? "OVERLAY" : "?",
			     plane_has_format(d.fd, id, DRM_FORMAT_XRGB8888) ? "yes" : "no");
		}
		drmModeFreePlaneResources(pres);
	}

	/* Atomic modeset?  A driver without DRIVER_ATOMIC answers EOPNOTSUPP. */
	if (drmSetClientCap(d.fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0) {
		drmModeAtomicReq *req = drmModeAtomicAlloc();

		if (req) {
			int r = drmModeAtomicCommit(d.fd, req, 0, NULL);

			if (r == 0)
				have_atomic = 1;
			else if (errno == EOPNOTSUPP)
				have_atomic = -1;
			else
				have_atomic = -2;
			drmModeAtomicFree(req);
		}
	}
	info("atomic      : %s\n",
	     have_atomic == 1 ? "yes" :
	     have_atomic == -1 ? "no (DRIVER_ATOMIC not set by the driver)" :
	     have_atomic == -2 ? "probe failed (driver answered an error)" :
	     "cannot probe");

	/* The question that matters: can a 320x200 fb be scaled to the mode? */
	if (create_dumb(&d, DEFAULT_SRC_W, DEFAULT_SRC_H) < 0) {
		disp_close(&d);
		return 4;
	}

	ret = set_plane(&d, d.fb_id, 0, 0, d.mode.hdisplay, d.mode.vdisplay,
			0, 0, DEFAULT_SRC_W, DEFAULT_SRC_H);
	if (ret == 0) {
		info("scale probe : OK - %dx%d scaled to %dx%d by the plane\n",
		     DEFAULT_SRC_W, DEFAULT_SRC_H, d.mode.hdisplay,
		     d.mode.vdisplay);
		info("verdict     : GPU plane scaling SUPPORTED\n");
		/* hand the display back to fbcon */
		set_plane(&d, 0, 0, 0, 0, 0, 0, 0, 0, 0);
		exit_code = 0;
	} else {
		int err = errno ? errno : EIO;

		info("scale probe : FAILED (%d: %s)\n", err, strerror(err));
		info("              %s\n", errno_hint(err));
		info("verdict     : GPU plane scaling NOT AVAILABLE on this kernel\n");
		info("fallback    : run the CRTC at the emulator's frame size and\n"
		     "              let the monitor scale:  dosdrm fit --src %dx%d\n",
		     DEFAULT_SRC_W, DEFAULT_SRC_H);
		destroy_dumb(&d);
	}
	disp_close(&d);
	return exit_code;
}

/* ----------------------------------------------------------------- scale --- */

static int cmd_scale(const char *device, int sw, int sh, int stretch, int frames,
		     int fps, int dirty)
{
	struct disp d;
	uint64_t t_start, t_end, filled = 0, worst = 0;
	int rx, ry, rw, rh;
	unsigned frame = 0;
	int ret;

	if (disp_open(&d, device, 1) < 0)
		return 2;
	g_disp = &d;

	if (create_dumb(&d, sw, sh) < 0) {
		disp_close(&d);
		return 4;
	}

	fit_rect(d.mode.hdisplay, d.mode.vdisplay, sw, sh, stretch,
		 &rx, &ry, &rw, &rh);

	info("dosdrm: %dx%d fb -> %dx%d at %d,%d (mode %dx%d, %s)\n",
	     sw, sh, rw, rh, rx, ry, d.mode.hdisplay, d.mode.vdisplay,
	     stretch ? "stretch" : "aspect corrected");

	ret = set_plane(&d, d.fb_id, rx, ry, rw, rh, 0, 0, sw, sh);
	if (ret) {
		int err = errno;

		fail("dosdrm: drmModeSetPlane failed: %d: %s\n", err, strerror(err));
		fail("        %s\n", errno_hint(err));
		fail("fallback: dosdrm fit --src %dx%d  (CRTC mode == frame, "
		     "monitor scales)\n", sw, sh);
		destroy_dumb(&d);
		disp_close(&d);
		return 3;
	}

	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	signal(SIGHUP, on_stop);

	t_start = now_us();
	while (!d.stop && (frames <= 0 || (int)frame < frames)) {
		uint64_t f0, f1;

		f0 = now_us();
		draw_pattern(&d, frame);
		f1 = now_us();
		if (f1 > f0) {
			uint64_t dt = f1 - f0;

			filled += dt;
			if (dt > worst)
				worst = dt;
		}
		if (dirty) {
			drmModeClip clip = { 0, 0, (uint32_t)sw, (uint32_t)sh };
			int r = drmModeDirtyFB(d.fd, d.fb_id, &clip, 1);

			if (r) {
				info("dosdrm: dirtyfb: %s (harmless for VRAM dumb buffers)\n",
				     strerror(errno));
				dirty = 0;
			}
		}
		frame++;
		if (fps > 0) {
			uint64_t budget = 1000000ull / (uint64_t)fps;
			uint64_t spent = now_us() - f0;

			if (spent < budget)
				usleep((useconds_t)(budget - spent));
		}
	}
	t_end = now_us();
	g_disp = NULL;

	info("dosdrm: %u frames in %.1f ms (%s), per-frame CPU fill avg %.1f us, "
	     "max %llu us, %.1f Mpx/s\n",
	     frame, (double)(t_end - t_start) / 1000.0,
	     frames > 0 ? "fixed" : "interrupted",
	     frame ? (double)filled / frame : 0.0,
	     (unsigned long long)worst,
	     filled ? (double)frame * sw * sh / (double)filled : 0.0);
	info("dosdrm: plane commits per frame: 0 (scanout reads the buffer "
	     "directly)\n");

	set_plane(&d, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	destroy_dumb(&d);
	disp_close(&d);
	return 0;
}

/* ------------------------------------------------------------------- fit --- */

static int mode_aspect_ok(const drmModeModeInfo *m, int num, int den)
{
	long long lhs = (long long)m->hdisplay * den;
	long long rhs = (long long)m->vdisplay * num;
	long long diff = lhs > rhs ? lhs - rhs : rhs - lhs;

	/* accept within 0.5% */
	return diff * 200 <= rhs;
}

static int cmd_fit(const char *device, int sw, int sh, int list_only,
		   const char *conf_path)
{
	struct disp d;
	drmModeConnector *conn;
	drmModeRes *res;
	drmModeModeInfo best;
	int i, want_num, want_den, ew, eh, have_best = 0;
	long long best_area = 0;
	char fbdev[128] = "";
	int fb_w = 0, fb_h = 0, conf_w = 0, conf_h = 0;
	uint32_t fb_pitch = 0;

	if (disp_open(&d, device, 0) < 0)
		return 2;

	res = drmModeGetResources(d.fd);
	if (!res || d.conn_id == 0) {
		fail("dosdrm: no connector\n");
		if (res)
			drmModeFreeResources(res);
		disp_close(&d);
		return 2;
	}

	conn = drmModeGetConnector(d.fd, d.conn_id);
	if (!conn) {
		fail("dosdrm: connector %u: %s\n", d.conn_id, strerror(errno));
		drmModeFreeResources(res);
		disp_close(&d);
		return 2;
	}

	source_extent(sw, sh, &ew, &eh);
	want_num = ew;
	want_den = eh;

	if (list_only) {
		for (i = 0; i < conn->count_modes; i++)
			info("mode %dx%d@%d%s\n", conn->modes[i].hdisplay,
			     conn->modes[i].vdisplay, conn->modes[i].vrefresh,
			     (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED)
				     ? " (preferred)" : "");
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		disp_close(&d);
		return 0;
	}

	/*
	 * Rule: smallest mode with the source's (aspect-corrected) shape that is
	 * at least 2x the source width.  320x200 -> 640x480 exactly, and 640x480
	 * -> 2560x1440 is 4.00x/3.00x, i.e. integer-ish upscale in the monitor.
	 */
	for (i = 0; i < conn->count_modes; i++) {
		drmModeModeInfo *m = &conn->modes[i];
		long long area;

		if (m->hdisplay < sw || m->vdisplay < eh)
			continue;
		if (m->vdisplay < 400 || m->hdisplay < 640)
			continue;
		if (!mode_aspect_ok(m, want_num, want_den))
			continue;
		area = (long long)m->hdisplay * m->vdisplay;
		if (m->hdisplay >= sw * 2 && (!have_best || area < best_area)) {
			best = *m;
			best_area = area;
			have_best = 1;
		}
	}
	if (!have_best) {
		/* fall back to the preferred mode */
		for (i = 0; i < conn->count_modes; i++) {
			if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
				best = conn->modes[i];
				have_best = 1;
				break;
			}
		}
	}
	if (!have_best && conn->count_modes > 0) {
		best = conn->modes[0];
		have_best = 1;
	}
	if (!have_best) {
		fail("dosdrm: connector %u exposes no modes\n", d.conn_id);
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		disp_close(&d);
		return 2;
	}

	/* current fb0, to show the mismatch the guest is hitting right now */
	{
		int fd = open("/dev/fb0", O_RDWR);

		if (fd >= 0) {
			struct fb_fix_screeninfo fix;
			struct fb_var_screeninfo var;

			if (!ioctl(fd, FBIOGET_VSCREENINFO, &var) &&
			    !ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
				snprintf(fbdev, sizeof(fbdev),
					 "%ux%u pitch %u", var.xres, var.yres,
					 fix.line_length);
				fb_w = (int)var.xres;
				fb_h = (int)var.yres;
				fb_pitch = fix.line_length;
			}
			close(fd);
		}
	}

	/* DOSBox's [sdl] fullresolution, if the conf is readable */
	if (conf_path) {
		FILE *f = fopen(conf_path, "r");
		char line[256];

		while (f && fgets(line, sizeof(line), f)) {
			if (sscanf(line, " fullresolution = %dx%d", &conf_w,
				   &conf_h) == 2 ||
			    sscanf(line, " fullresolution=%dx%d", &conf_w,
				   &conf_h) == 2)
				break;
		}
		if (f)
			fclose(f);
	}

	info("source      : %dx%d (aspect corrected %dx%d)\n", sw, sh, ew, eh);
	info("crtc today  : %dx%d@%d\n", d.mode.hdisplay, d.mode.vdisplay,
	     d.mode.vrefresh);
	if (fbdev[0])
		info("fb0 today   : %s\n", fbdev);
	if (conf_w > 0)
		info("conf today  : fullresolution=%dx%d\n", conf_w, conf_h);
	info("best mode   : %dx%d@%d\n", best.hdisplay, best.vdisplay,
	     best.vrefresh);
	info("scale by monitor: %.2fx horizontal, %.2fx vertical\n",
	     (double)best.hdisplay / (double)sw, (double)best.vdisplay / (double)eh);

	/*
	 * The failure mode this tool exists to catch: SDL 1.2's fbcon driver
	 * takes the surface pitch from FBIOGET_FSCREENINFO (the fb pitch) but
	 * the width from the requested mode, so a request smaller than the fb
	 * yields a squashed image, and a request larger than the fb makes
	 * SDL_SetVideoMode fail (DOSBox then E_Exit's).
	 */
	if (fb_w > 0 && (fb_w * 4 != (int)fb_pitch))
		info("MISMATCH    : fb0 pitch %u != %d (width*4): SDL will write "
		     "at the wrong stride\n", fb_pitch, fb_w * 4);
	if (conf_w > 0 && fb_w > 0 && (conf_w != fb_w || conf_h != fb_h))
		info("MISMATCH    : fullresolution=%dx%d but fb0 is %dx%d: "
		     "DOSBox will %s\n", conf_w, conf_h, fb_w, fb_h,
		     conf_w > fb_w ? "E_Exit (mode rejected)" :
				     "draw squashed at the fb pitch");

	info("\nkernel cmdline (board/intel/galileo/grub.cfg):\n"
	     "  video=%dx%d@%d\n", best.hdisplay, best.vdisplay, best.vrefresh);
	info("DOSBox conf [sdl] (/root/.dosbox/dosbox-0.74.conf):\n"
	     "  fullresolution=%dx%d\n", best.hdisplay, best.vdisplay);
	info("\nWith the CRTC at that mode the fbdev fb is exactly the emulator\n"
	     "frame, SDL writes 1:1 (correct pitch) and the monitor's scaler does\n"
	     "the upscale - no CPU stretch, no DRM plane scaling needed.\n");

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	disp_close(&d);
	return 0;
}

/* ----------------------------------------------------------------- bench --- */

static int cmd_bench(const char *device, int sw, int sh, int frames)
{
	struct disp d;
	uint8_t *src;
	size_t src_len;
	uint64_t t0, t1;
	int i, fdfb = -1;
	uint8_t *fbmap = NULL;
	size_t fblen = 0;
	uint32_t fbpitch = 0;
	double mb;

	if (frames <= 0)
		frames = 600;
	src_len = (size_t)sw * 4;
	src = malloc(src_len);
	if (!src)
		return 1;
	for (i = 0; i < (int)src_len; i++)
		src[i] = (uint8_t)(i * 7);

	if (disp_open(&d, device, 0) < 0) {
		free(src);
		return 2;
	}
	if (create_dumb(&d, sw, sh) < 0) {
		free(src);
		disp_close(&d);
		return 4;
	}

	/* (a) the fbcon path: what SDL/DOSBox writes into today */
	fdfb = open("/dev/fb0", O_RDWR);
	if (fdfb >= 0) {
		struct fb_fix_screeninfo fix;

		if (!ioctl(fdfb, FBIOGET_FSCREENINFO, &fix)) {
			fbpitch = fix.line_length;
			fblen = (size_t)fix.line_length * sh;
			fbmap = mmap(NULL, fblen, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fdfb, 0);
			if (fbmap == MAP_FAILED)
				fbmap = NULL;
		}
	}

	if (fbmap) {
		t0 = now_us();
		for (i = 0; i < frames; i++) {
			int y;

			for (y = 0; y < sh; y++)
				memcpy(fbmap + (size_t)y * fbpitch, src,
				       src_len < fbpitch ? src_len : fbpitch);
		}
		t1 = now_us();
		mb = (double)frames * (double)sh * (double)src_len / 1024.0 / 1024.0;
		info("fb0  path   : pitch %u, %dx%d, %.1f MB in %llu us -> "
		     "%.1f us/frame, %.1f MB/s\n",
		     fbpitch, sw, sh, mb, (unsigned long long)(t1 - t0),
		     (double)(t1 - t0) / frames, mb * 1000000.0 / (double)(t1 - t0));
	} else {
		info("fb0  path   : /dev/fb0 not available, skipped\n");
	}

	t0 = now_us();
	for (i = 0; i < frames; i++) {
		int y;

		for (y = 0; y < sh; y++)
			memcpy(d.map + (size_t)y * d.pitch, src,
			       src_len < d.pitch ? src_len : d.pitch);
	}
	t1 = now_us();
	mb = (double)frames * (double)sh * (double)src_len / 1024.0 / 1024.0;
	info("dumb path   : pitch %u, %dx%d, %.1f MB in %llu us -> "
	     "%.1f us/frame, %.1f MB/s\n",
	     d.pitch, sw, sh, mb, (unsigned long long)(t1 - t0),
	     (double)(t1 - t0) / frames, mb * 1000000.0 / (double)(t1 - t0));
	info("note: both mappings are PCIe write-combined (amdgpu dumb buffers use\n"
	     "      AMDGPU_GEM_CREATE_CPU_GTT_USWC), so the numbers should match:\n"
	     "      moving DOSBox to a DRM dumb buffer costs nothing extra.\n");

	if (fbmap)
		munmap(fbmap, fblen);
	if (fdfb >= 0)
		close(fdfb);
	free(src);
	destroy_dumb(&d);
	disp_close(&d);
	return 0;
}

/* ------------------------------------------------------------------ main --- */

static void usage(void)
{
	info(
"dosdrm - DRM/KMS probe and scaled-plane presenter (Galileo DOS image)\n"
"\n"
"usage: dosdrm <command> [options]\n"
"\n"
"  probe                     diagnose the display pipeline and test whether\n"
"                            a %dx%d fb can be scaled onto the active mode\n"
"  scale [--src WxH] [--stretch] [--frames N] [--fps N] [--dirty]\n"
"                            show an animated test pattern through the primary\n"
"                            plane at the CRTC mode; prints per-frame CPU cost\n"
"  fit [--src WxH] [--list] [--conf PATH]\n"
"                            choose the connector mode that makes fb0 equal to\n"
"                            the emulator frame (monitor does the upscaling);\n"
"                            --conf also checks DOSBox's fullresolution\n"
"                            (default /root/.dosbox/dosbox-0.74.conf)\n"
"  bench [--src WxH] [--frames N]\n"
"                            write-bandwidth comparison: /dev/fb0 vs dumb buffer\n"
"\n"
"global: --device PATH (default $DOSDRM_DEVICE or /dev/dri/card0)\n"
"exit  : 0 ok, 1 usage, 2 no usable DRM device (missing, no active CRTC,\n"
"        or not master), 3 scaling unsupported, 4 ioctl error\n",
	       DEFAULT_SRC_W, DEFAULT_SRC_H);
}

int main(int argc, char **argv)
{
	const char *device = NULL;
	const char *conf = "/root/.dosbox/dosbox-0.74.conf";
	const char *cmd;
	int sw = DEFAULT_SRC_W, sh = DEFAULT_SRC_H;
	int stretch = 0, frames = 240, fps = 0, dirty = 0, list_only = 0;
	int i;

	if (argc < 2) {
		usage();
		return 1;
	}
	cmd = argv[1];

	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			device = argv[++i];
		} else if (!strcmp(argv[i], "--src") && i + 1 < argc) {
			if (parse_size(argv[++i], &sw, &sh)) {
				fail("dosdrm: bad --src (want WxH)\n");
				return 1;
			}
		} else if (!strcmp(argv[i], "--stretch")) {
			stretch = 1;
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
			fps = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--dirty")) {
			dirty = 1;
		} else if (!strcmp(argv[i], "--conf") && i + 1 < argc) {
			conf = argv[++i];
		} else if (!strcmp(argv[i], "--list")) {
			list_only = 1;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else {
			fail("dosdrm: unknown option '%s'\n", argv[i]);
			usage();
			return 1;
		}
	}

	if (!strcmp(cmd, "probe"))
		return cmd_probe(device);
	if (!strcmp(cmd, "scale"))
		return cmd_scale(device, sw, sh, stretch, frames, fps, dirty);
	if (!strcmp(cmd, "fit"))
		return cmd_fit(device, sw, sh, list_only, conf);
	if (!strcmp(cmd, "bench"))
		return cmd_bench(device, sw, sh, frames);
	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage();
		return 0;
	}
	fail("dosdrm: unknown command '%s'\n", cmd);
	usage();
	return 1;
}
