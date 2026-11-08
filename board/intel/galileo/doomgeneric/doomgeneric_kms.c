//doomgeneric for a bare Linux VirtualTerminal
// Copyright (C) 2025 Techflash
// Based on doomgeneric_sdl.c

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"
#include "i_system.h"

// XXX: HACK
// Linux's input-event-codes.h and doomkeys.h have many collisions.
// Redefine some of doomkeys.h's names here to work around this.
// I could try to redefine Linux's... but that sounds incredibly
// fragile, and is very likely not a good idea.
#undef KEY_TAB
#undef KEY_ENTER
#undef KEY_BACKSPACE
#undef KEY_MINUS
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#define DOOM_KEY_TAB		9
#define DOOM_KEY_ENTER		13
#define DOOM_KEY_MINUS		0x2d
#define DOOM_KEY_BACKSPACE	0x7f
#define DOOM_KEY_F1		(0x80+0x3b)
#define DOOM_KEY_F2		(0x80+0x3c)
#define DOOM_KEY_F3		(0x80+0x3d)
#define DOOM_KEY_F4		(0x80+0x3e)
#define DOOM_KEY_F5		(0x80+0x3f)
#define DOOM_KEY_F6		(0x80+0x40)
#define DOOM_KEY_F7		(0x80+0x41)
#define DOOM_KEY_F8		(0x80+0x42)
#define DOOM_KEY_F9		(0x80+0x43)
#define DOOM_KEY_F10		(0x80+0x44)
#define DOOM_KEY_F11		(0x80+0x57)
#define DOOM_KEY_F12		(0x80+0x58)


#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <linux/fb.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdbool.h>

#define KEYQUEUE_SIZE 16

#define MAX_INPUT_DEVS 16
#define INPUT_TYPE_KEYBOARD 0
#define INPUT_TYPE_JOYSTICK 1
#define INPUT_TYPE_MOUSE    2
#define INPUT_TYPE_TOUCH    3

// timing stuff
static struct timeval startTime;

// KMS/DRM stuff: the game frame is a 320x200 XRGB8888 dumb buffer that the
// GPU scans out scaled to the panel mode (primary plane scaling), so the CPU
// only ever writes 256 KB per frame no matter the display resolution.
static int drmFd = -1;
static int ttyFd = -1;
static int savedTtyMode;
static uint32_t drmCrtcId, drmConnId, drmPlaneId;
static drmModeModeInfo drmMode;
static uint32_t pFbId, pCrtcId, pSrcX, pSrcY, pSrcW, pSrcH, pCrtcX, pCrtcY, pCrtcW, pCrtcH;
static uint32_t cActive, cModeId, nCrtcId;
#define NUM_BUFS 3
static uint32_t fbIds[NUM_BUFS];
static uint8_t *fbMaps[NUM_BUFS];
static uint32_t fbPitch;
static int curBuf = 0, flipsPending = 0;
static uint64_t flipCount = 0, fpsMark = 0;

// input stuff
static int numInputFds = 0;
static int inputFds[MAX_INPUT_DEVS];
static bool shiftPressed = false;
static struct pollfd pollfds[MAX_INPUT_DEVS];

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

// XXX: HACK
// Linux's evdev system doesn't make it feasible to just use
// tolower(key) like the existing conversions did, so we
// use a few ranges of maps here to avoid an obscenely long switch/case
static char evdevKeysToASCII1[10] = {
	'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'
};
static char evdevKeysToASCII2[12] = {
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']'
};
static char evdevKeysToASCII3[12] = {
	'a', 's', 'd', 'f', 'g', 'h', 'i', 'j', 'k', 'l', ';', '\''
};
static char evdevKeysToASCII4[11] = {
	'\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/'
};


static char evdevShiftKeysToASCII1[12] = {
	'!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '{', '}'
};
static char evdevShiftKeysToASCII2[12] = {
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '{', '}'
};
static char evdevShiftKeysToASCII3[12] = {
	'a', 's', 'd', 'f', 'g', 'h', 'i', 'j', 'k', 'l', ':', '"'
};
static char evdevShiftKeysToASCII4[11] = {
	'|', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '<', '>', '?'
};


static unsigned char convertToDoomKey(unsigned int key){
	switch (key) {
		case KEY_ENTER:
			key = DOOM_KEY_ENTER;
			break;
		case KEY_ESC:
			key = KEY_ESCAPE;
			break;
		case KEY_LEFT:
			key = KEY_LEFTARROW;
			break;
		case KEY_RIGHT:
			key = KEY_RIGHTARROW;
			break;
		case KEY_UP:
			key = KEY_UPARROW;
			break;
		case KEY_DOWN:
			key = KEY_DOWNARROW;
			break;
		case KEY_LEFTCTRL:
		case KEY_RIGHTCTRL:
			key = KEY_FIRE;
			break;
		case KEY_SPACE:
			key = KEY_USE;
			break;
		case KEY_LEFTSHIFT:
		case KEY_RIGHTSHIFT:
			key = KEY_RSHIFT;
			break;
		case KEY_LEFTALT:
		case KEY_RIGHTALT:
			key = KEY_LALT;
			break;
		case KEY_F2:
			key = DOOM_KEY_F2;
			break;
		case KEY_F3:
			key = DOOM_KEY_F3;
			break;
		case KEY_F4:
			key = DOOM_KEY_F4;
			break;
		case KEY_F5:
			key = DOOM_KEY_F5;
			break;
		case KEY_F6:
			key = DOOM_KEY_F6;
			break;
		case KEY_F7:
			key = DOOM_KEY_F7;
			break;
		case KEY_F8:
			key = DOOM_KEY_F8;
			break;
		case KEY_F9:
			key = DOOM_KEY_F9;
			break;
		case KEY_F10:
			key = DOOM_KEY_F10;
			break;
		case KEY_F11:
			key = DOOM_KEY_F11;
			break;
		case KEY_EQUAL:
			key = KEY_EQUALS;
			break;
		case KEY_MINUS:
			key = DOOM_KEY_MINUS;
			break;
		case KEY_BACKSPACE:
			key = DOOM_KEY_BACKSPACE;
			break;
		case KEY_TAB:
			key = DOOM_KEY_TAB;
			break;

		// sadly, yes, we need to handle every single alphanumeric
		// key here, since evdev doesn't spit out keys in anything
		// remotely resembling ASCII.....
		// though, we can take many shortcuts
		case KEY_1:
		case KEY_2:
		case KEY_3:
		case KEY_4:
		case KEY_5:
		case KEY_6:
		case KEY_7:
		case KEY_8:
		case KEY_9:
		case KEY_0:
			if (shiftPressed)
				key = evdevShiftKeysToASCII1[key - KEY_1];
			else
				key = evdevKeysToASCII1[key - KEY_1];
			break;
		case KEY_Q:
		case KEY_W:
		case KEY_E:
		case KEY_R:
		case KEY_T:
		case KEY_Y:
		case KEY_U:
		case KEY_I:
		case KEY_O:
		case KEY_P:
		case KEY_LEFTBRACE:
		case KEY_RIGHTBRACE:
			if (shiftPressed)
				key = evdevShiftKeysToASCII2[key - KEY_Q];
			else
				key = evdevKeysToASCII2[key - KEY_Q];
			break;
		case KEY_A:
		case KEY_S:
		case KEY_D:
		case KEY_F:
		case KEY_G:
		case KEY_H:
		case KEY_J:
		case KEY_K:
		case KEY_L:
		case KEY_SEMICOLON:
		case KEY_APOSTROPHE:
			if (shiftPressed)
				key = evdevShiftKeysToASCII3[key - KEY_A];
			else
				key = evdevKeysToASCII3[key - KEY_A];
			break;
		case KEY_BACKSLASH:
		case KEY_Z:
		case KEY_X:
		case KEY_C:
		case KEY_V:
		case KEY_B:
		case KEY_N:
		case KEY_M:
		case KEY_COMMA:
		case KEY_DOT:
		case KEY_SLASH:
			if (shiftPressed)
				key = evdevShiftKeysToASCII4[key - KEY_BACKSLASH];
			else
				key = evdevKeysToASCII4[key - KEY_BACKSLASH];
			break;
		default:
			key = 0xFF;
			break;
	}

	return key;
}

static void addKeyToQueue(int pressed, unsigned char keyCode) {
	if ((keyCode == KEY_LEFTSHIFT || keyCode == KEY_RIGHTSHIFT) &&
		(pressed == 1 || pressed == 0))
		shiftPressed = pressed;

		
	unsigned char key = convertToDoomKey(keyCode);
	if (key == 0xFF) // unknown, don't process it
		return;

	if (pressed > 1 || pressed < 0) // bogus value
		return;

	unsigned short keyData = (pressed << 8) | key;

	s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
	s_KeyQueueWriteIndex++;
	s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}



static void checkKeys() {
	int ret, i;
	bool keepGoing;
	struct input_event ev;

	keepGoing = true;
	while (keepGoing) {
		keepGoing = false; // exit if we haven't gotten anything
		ret = poll(pollfds, numInputFds, 0);  // don't wait at all, just give anything we've got

		if (ret < 0) {
			// borked
			return;
		}

		for (i = 0; i < MAX_INPUT_DEVS; i++) {
			if (pollfds[i].revents & POLLIN) {
				read(inputFds[i], &ev, sizeof(ev));
				keepGoing = true; // we read something, so keep trying
				addKeyToQueue(ev.value, ev.code);
			}
		}
	}

	// we read the entire backlog, get back to doom
	return;
}


#define TEST_KEY(k) (keybits[(k)/8] & (1 << ((k)%8)))
static int isKeyboard(const char *devPath) {
	unsigned long evbits;
	unsigned char keybits[KEY_MAX/8 + 1];
	int fd = open(devPath, O_RDONLY);
	if (fd < 0) {
		perror("Failed to open device");
		return 0;
	}

	evbits = 0;
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0) {
		close(fd);
		return 0;
	}

	/* Must support EV_KEY */
	if (!(evbits & (1 << EV_KEY))) {
		close(fd);
		return 0;
	}


	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
		close(fd);
		return 0;
	}

	if (TEST_KEY(KEY_A) && TEST_KEY(KEY_ENTER)) {
		close(fd);
		return 1;  /* looks like a keyboard */
	}

	close(fd);
	return 0;
}

static void checkInputDevs() {
	struct dirent *dp;
	DIR *dir;

	/* check for devices */
	dir = opendir("/dev/input");

	while ((dp = readdir(dir)) != NULL) {
		char fullpath[268]; /* d_name is 256 bytes */
		int fd;

		if (numInputFds >= MAX_INPUT_DEVS)
			I_Error("Out of room in input devices array");

		printf("checking %s\n", dp->d_name);
		if (strncmp(dp->d_name, "event", 5) != 0)
			continue;

		sprintf(fullpath, "/dev/input/%s", dp->d_name);

		printf("%s is a valid event device\n", fullpath);
		if (!isKeyboard(fullpath)) {
			printf("%s is not a keyboard, moving on\n", fullpath);
			continue;
		}

		fd = open(fullpath, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			// not necessarily fatal
			perror("Failed to open device");
			continue;
		}

		pollfds[numInputFds].fd = fd;
		pollfds[numInputFds].events = POLLIN;
		inputFds[numInputFds++] = fd;
		printf("adding %s keyboard\n", fullpath);
		ioctl(fd, EVIOCGRAB, 1); // grab exclusive access to the device
	}
	closedir(dir);
}


static void RestoreVtMode(void)
{
	if (ttyFd < 0)
		return;

	ioctl(ttyFd, KDSETMODE, savedTtyMode);
	close(ttyFd);
	ttyFd = -1;
}

static void signal_handler(int sig)
{
	RestoreVtMode();
	signal(sig, SIG_DFL);
	raise(sig);
}

static uint32_t get_prop(uint32_t obj, uint32_t type, const char *name)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(drmFd, obj, type);
	uint32_t id = 0;
	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !id; i++) {
		drmModePropertyPtr p = drmModeGetProperty(drmFd, props->props[i]);
		if (p) {
			if (!strcmp(p->name, name))
				id = p->prop_id;
			drmModeFreeProperty(p);
		}
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static int make_fb(uint32_t *fb_out, uint8_t **map_out)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;

	memset(&creq, 0, sizeof(creq));
	creq.width = DOOMGENERIC_RESX;
	creq.height = DOOMGENERIC_RESY;
	creq.bpp = 32;
	if (drmIoctl(drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &creq))
		return -1;
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = creq.handle;
	if (drmIoctl(drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mreq))
		return -1;
	uint8_t *p = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, drmFd, mreq.offset);
	if (p == MAP_FAILED)
		return -1;
	if (drmModeAddFB(drmFd, DOOMGENERIC_RESX, DOOMGENERIC_RESY, 24, 32, creq.pitch, creq.handle, fb_out))
		return -1;
	fbPitch = creq.pitch;
	*map_out = p;
	return 0;
}

static void flip_handler(int fd, unsigned int seq, unsigned int sec,
			 unsigned int usec, void *data)
{
	(void)fd; (void)seq; (void)sec; (void)usec; (void)data;
	if (flipsPending > 0)
		flipsPending--;
	flipCount++;
}

static drmEventContext evctx;

static void kms_wait_flip(void)
{
	while (flipsPending > NUM_BUFS - 1) {
		struct pollfd pfd = { .fd = drmFd, .events = POLLIN };
		if (poll(&pfd, 1, 1000) > 0)
			drmHandleEvent(drmFd, &evctx);
	}
}

void DG_Init()
{
	drmModeRes *res;
	drmModePlaneRes *pres;
	int conn_id = -1, crtc_idx = -1;

	drmFd = open("/dev/dri/card0", O_RDWR);
	if (drmFd < 0)
		I_Error("kms: open /dev/dri/card0: %s", strerror(errno));
	drmSetClientCap(drmFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	drmSetClientCap(drmFd, DRM_CLIENT_CAP_ATOMIC, 1);

	res = drmModeGetResources(drmFd);
	if (!res)
		I_Error("kms: drmModeGetResources: %s", strerror(errno));

	for (int i = 0; i < res->count_connectors && conn_id < 0; i++) {
		drmModeConnector *c = drmModeGetConnector(drmFd, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			int pick = 0;
			for (int m = 0; m < c->count_modes; m++)
				if (c->modes[m].hdisplay == 1280 && c->modes[m].vdisplay == 1024 &&
				    c->modes[m].vrefresh == 60)
					pick = m;
			drmMode = c->modes[pick];
			conn_id = c->connector_id;
			for (int e = 0; e < c->count_encoders && crtc_idx < 0; e++) {
				drmModeEncoder *enc = drmModeGetEncoder(drmFd, c->encoders[e]);
				if (!enc)
					continue;
				for (int k = 0; k < res->count_crtcs; k++)
					if (enc->possible_crtcs & (1 << k)) {
						crtc_idx = k;
						break;
					}
				drmModeFreeEncoder(enc);
			}
		}
		drmModeFreeConnector(c);
	}
	if (conn_id < 0 || crtc_idx < 0)
		I_Error("kms: no connected display");
	drmConnId = conn_id;
	drmCrtcId = res->crtcs[crtc_idx];
	printf("kms: connector %u crtc %u mode %s %dx%d@%d\n", drmConnId, drmCrtcId,
	       drmMode.name, drmMode.hdisplay, drmMode.vdisplay, drmMode.vrefresh);

	pres = drmModeGetPlaneResources(drmFd);
	for (uint32_t i = 0; i < pres->count_planes && !drmPlaneId; i++) {
		drmModePlane *p = drmModeGetPlane(drmFd, pres->planes[i]);
		if (!p)
			continue;
		uint32_t type_id = get_prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "type");
		drmModeObjectProperties *props =
			drmModeObjectGetProperties(drmFd, p->plane_id, DRM_MODE_OBJECT_PLANE);
		uint32_t type = 99;
		for (uint32_t k = 0; props && k < props->count_props; k++)
			if (props->props[k] == type_id)
				type = props->prop_values[k];
		drmModeFreeObjectProperties(props);
		if (type == DRM_PLANE_TYPE_PRIMARY && (p->possible_crtcs & (1 << crtc_idx)))
			drmPlaneId = p->plane_id;
		drmModeFreePlane(p);
	}
	if (!drmPlaneId)
		I_Error("kms: no primary plane for crtc %u", drmCrtcId);

	for (int i = 0; i < NUM_BUFS; i++)
		if (make_fb(&fbIds[i], &fbMaps[i]))
			I_Error("kms: cannot create scanout buffer %d: %s", i, strerror(errno));

	pFbId   = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "FB_ID");
	pCrtcId = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
	pSrcX   = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_X");
	pSrcY   = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_Y");
	pSrcW   = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_W");
	pSrcH   = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_H");
	pCrtcX  = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_X");
	pCrtcY  = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
	pCrtcW  = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_W");
	pCrtcH  = get_prop(drmPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_H");
	cActive = get_prop(drmCrtcId, DRM_MODE_OBJECT_CRTC, "ACTIVE");
	cModeId = get_prop(drmCrtcId, DRM_MODE_OBJECT_CRTC, "MODE_ID");
	nCrtcId = get_prop(drmConnId, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

	/* keep the 320x200 aspect ratio: 1280x800 centred, black bars top/bottom */
	uint32_t cw = drmMode.hdisplay;
	uint32_t ch = cw * DOOMGENERIC_RESY / DOOMGENERIC_RESX;
	if (ch > drmMode.vdisplay) {
		ch = drmMode.vdisplay;
		cw = ch * DOOMGENERIC_RESX / DOOMGENERIC_RESY;
	}

	uint32_t blob = 0;
	if (drmModeCreatePropertyBlob(drmFd, &drmMode, sizeof(drmMode), &blob))
		I_Error("kms: mode blob: %s", strerror(errno));

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	drmModeAtomicAddProperty(req, drmCrtcId, cActive, 1);
	drmModeAtomicAddProperty(req, drmCrtcId, cModeId, blob);
	drmModeAtomicAddProperty(req, drmConnId, nCrtcId, drmCrtcId);
	drmModeAtomicAddProperty(req, drmPlaneId, pFbId, fbIds[0]);
	drmModeAtomicAddProperty(req, drmPlaneId, pCrtcId, drmCrtcId);
	drmModeAtomicAddProperty(req, drmPlaneId, pSrcX, 0);
	drmModeAtomicAddProperty(req, drmPlaneId, pSrcY, 0);
	drmModeAtomicAddProperty(req, drmPlaneId, pSrcW, DOOMGENERIC_RESX << 16);
	drmModeAtomicAddProperty(req, drmPlaneId, pSrcH, DOOMGENERIC_RESY << 16);
	drmModeAtomicAddProperty(req, drmPlaneId, pCrtcX, (drmMode.hdisplay - cw) / 2);
	drmModeAtomicAddProperty(req, drmPlaneId, pCrtcY, (drmMode.vdisplay - ch) / 2);
	drmModeAtomicAddProperty(req, drmPlaneId, pCrtcW, cw);
	drmModeAtomicAddProperty(req, drmPlaneId, pCrtcH, ch);
	if (drmModeAtomicCommit(drmFd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
		I_Error("kms: atomic modeset: %s", strerror(errno));
	drmModeAtomicFree(req);
	printf("kms: %dx%d framebuffer scaled to %ux%u on %dx%d\n",
	       DOOMGENERIC_RESX, DOOMGENERIC_RESY, cw, ch, drmMode.hdisplay, drmMode.vdisplay);

	evctx.version = 2;
	evctx.page_flip_handler = flip_handler;
	flipsPending = 0;
	fpsMark = 0;

	ttyFd = open("/dev/tty0", O_RDWR);
	if (ttyFd < 0)
		ttyFd = open("/dev/console", O_RDWR);
	if (ttyFd >= 0) {
		if (ioctl(ttyFd, KDGETMODE, &savedTtyMode) == 0) {
			atexit(RestoreVtMode);
			signal(SIGINT, signal_handler);
			signal(SIGTERM, signal_handler);
			if (ioctl(ttyFd, KDSETMODE, KD_GRAPHICS) < 0)
				fprintf(stderr, "kms: KD_GRAPHICS: %s\n", strerror(errno));
		}
	}

	checkInputDevs();
	if (numInputFds == 0)
		I_Error("kms: no input device (see log above)");

	gettimeofday(&startTime, NULL);
}

void DG_DrawFrame()
{
	/* 320x200 XRGB8888: pitch is exactly the frame width, so one memcpy */
	memcpy(fbMaps[curBuf], DG_ScreenBuffer, DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);

	kms_wait_flip();

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	drmModeAtomicAddProperty(req, drmPlaneId, pFbId, fbIds[curBuf]);
	if (drmModeAtomicCommit(drmFd, req,
				DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0)
		flipsPending++;
	drmModeAtomicFree(req);

	curBuf = (curBuf + 1) % NUM_BUFS;

	struct pollfd pfd = { .fd = drmFd, .events = POLLIN };
	if (poll(&pfd, 1, 0) > 0)
		drmHandleEvent(drmFd, &evctx);

	uint64_t now = DG_GetTicksMs();
	if (!fpsMark)
		fpsMark = now;
	else if (now - fpsMark >= 5000) {
		printf("kms: %.1f fps\n", (double)flipCount * 1000.0 / (double)(now - fpsMark));
		fflush(stdout);
		flipCount = 0;
		fpsMark = now;
	}

	checkKeys();
}

void DG_SleepMs(uint32_t ms) {
	usleep(ms * 1000);
}

uint32_t DG_GetTicksMs() {
	struct timeval curTime;
	long seconds, usec;

	gettimeofday(&curTime, NULL);
	seconds = curTime.tv_sec - startTime.tv_sec;
	usec = curTime.tv_usec - startTime.tv_usec;

	return (seconds * 1000) + (usec / 1000);
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
	checkKeys();

	if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) {
		//key queue is empty

		return 0;
	}
	else {
		unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
		s_KeyQueueReadIndex++;
		s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

		*pressed = keyData >> 8;
		*doomKey = keyData & 0xFF;

		return 1;
	}

}

void DG_SetWindowTitle(const char * title) {
	printf("Window Title: %s\n", title);
}

int main(int argc, char **argv) {
	doomgeneric_Create(argc, argv);

	while (1)
	{
		doomgeneric_Tick();
	}


	return 0;
}
