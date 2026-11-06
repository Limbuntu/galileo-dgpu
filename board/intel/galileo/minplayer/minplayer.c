/* minplayer: minimal VA-API + EGL/GLES + KMS page-flip video player.
 *
 * Pipeline (benchmarks mpv's zero-copy VAAPI path):
 *   1. FFmpeg + VAAPI hwaccel decodes H.264 to VA surfaces (zero-copy).
 *   2. vaExportSurfaceHandle() exports each decoded surface as a
 *      DRM PRIME dmabuf (NV12, separate layers: luma R8 + chroma GR88).
 *   3. eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) turns each plane into
 *      an EGLImage; a trivial GLES2 shader samples luma/chroma textures
 *      (YUV -> RGB) into an XRGB8888 GBM scanout BO via an FBO.
 *   4. Each scanout BO (GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING) is
 *      AddFB2'd and presented with drmModePageFlip + PAGE_FLIP_EVENT.
 *
 * CPU accounting: samples /proc/self/stat utime+stime once per second,
 * prints CPU% for the previous window; at exit appends the average CPU%
 * and frames presented to the file given in argv[2] (default
 * /root/minplayer-result.txt).
 *
 * The sysroot has no libGLESv2 (Mesa built with gles2 disabled); the GLES2
 * client API dispatch lives in Mesa's shared glapi, so every GLES2 entry
 * point is fetched through eglGetProcAddress() into a function pointer.
 *
 * Cross build (gres-builder docker image):
 *   SYS=/out/workstation/host/i586-buildroot-linux-gnu/sysroot
 *   CC=/out/workstation/host/bin/i586-buildroot-linux-gnu-gcc
 *   $CC --sysroot=$SYS -O2 -I$SYS/usr/include/libdrm -I$SYS/usr/include \
 *       minplayer.c -o minplayer \
 *       -L$SYS/usr/lib -lavcodec -lavformat -lavutil -lva -lva-drm \
 *       -lEGL -lGL -lgbm -ldrm -lpthread -ldl -lm
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>

#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_drm.h>

/* ---------------------------------------------------------------- utils */

static long long now_ns(void)
{
	/* monotonic via /proc/uptime (kernel jiffies; gettimeofday is frozen
	 * on this board - no RTC/NTP, so wall clock never advances) */
	static int fd = -1;
	char buf[64];
	if (fd < 0) fd = open("/proc/uptime", O_RDONLY);
	if (fd < 0) return 0;
	if (lseek(fd, 0, SEEK_SET) < 0) return 0;
	int n = read(fd, buf, sizeof buf - 1);
	if (n <= 0) return 0;
	buf[n] = 0;
	double up = atof(buf);
	return (long long)(up * 1e9);
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
					const enum AVPixelFormat *pix_fmts)
{
	const enum AVPixelFormat *p;
	for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
		if (*p == AV_PIX_FMT_VAAPI)
			return AV_PIX_FMT_VAAPI;
	fprintf(stderr, "get_hw_format: VAAPI not offered\n");
	return pix_fmts[0];
}

static int die(const char *what)
{
	fprintf(stderr, "minplayer: %s: %s\n", what, strerror(errno));
	return 1;
}

static volatile int flip_pending;

static void flip_handler(int fd, unsigned int frame, unsigned int sec,
			 unsigned int usec, void *data)
{
	(void)fd; (void)frame; (void)sec; (void)usec;
	flip_pending = 0;
}

/* --------------------------------------------------------------- GLES2 */

/* Minimal GLES2 client types/constants (the sysroot's GL headers clash
 * with the function pointers we resolve dynamically). */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef double GLdouble;
typedef long GLsizeiptr;
typedef long GLintptr;
typedef void *GLeglImageOES_dat;

/* GLES2 functions are not exported by the sysroot's libGL; Mesa's shared
 * glapi dispatch serves them through eglGetProcAddress(). */

#define DECL_GL(ret, name, args) \
	typedef ret (*PFN_##name) args; \
	static PFN_##name name = NULL;
#define GET_GL(name) name = (PFN_##name)eglGetProcAddress(#name)

DECL_GL(void, glEGLImageTargetTexture2DOES, (GLenum target, GLeglImageOES_dat image))
DECL_GL(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))
DECL_GL(void, glUseProgram, (GLuint program))
DECL_GL(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type,
				      GLboolean normalized, GLsizei stride,
				      const void *pointer))
DECL_GL(void, glEnableVertexAttribArray, (GLuint index))
DECL_GL(GLuint, glCreateShader, (GLenum type))
DECL_GL(void, glShaderSource, (GLuint shader, GLsizei count,
			       const char **string, const GLint *length))
DECL_GL(void, glCompileShader, (GLuint shader))
DECL_GL(GLuint, glCreateProgram, (void))
DECL_GL(void, glAttachShader, (GLuint program, GLuint shader))
DECL_GL(void, glLinkProgram, (GLuint program))
DECL_GL(void, glGenTextures, (GLsizei n, GLuint *textures))
DECL_GL(void, glBindTexture, (GLenum target, GLuint texture))
DECL_GL(void, glDeleteTextures, (GLsizei n, const GLuint *textures))
DECL_GL(void, glUniform1i, (GLint location, GLint v0))
DECL_GL(GLint, glGetUniformLocation, (GLuint program, const char *name))
DECL_GL(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params))
DECL_GL(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params))
DECL_GL(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize,
				   GLsizei *length, char *infoLog))
DECL_GL(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize,
				    GLsizei *length, char *infoLog))
DECL_GL(GLenum, glGetError, (void))
DECL_GL(void, glGenBuffers, (GLsizei n, GLuint *buffers))
DECL_GL(void, glBindBuffer, (GLenum target, GLuint buffer))
DECL_GL(void, glBufferData, (GLenum target, GLsizeiptr size,
			     const void *data, GLenum usage))
DECL_GL(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers))
DECL_GL(void, glClear, (GLbitfield mask))
DECL_GL(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height))
DECL_GL(void, glActiveTexture, (GLenum texture))
DECL_GL(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))
DECL_GL(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers))
DECL_GL(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))
DECL_GL(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers))
DECL_GL(void, glFramebufferTexture2D, (GLenum target, GLenum attachment,
				       GLenum textarget, GLuint texture,
				       GLint level))
DECL_GL(void, glGetIntegerv, (GLenum pname, GLint *data))

/* GL constants (GLES2 subset) */
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_LINEAR 0x2601
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40

struct img_cache { VASurfaceID id; EGLImage y, uv; GLuint tex_y, tex_uv; int fd0, fd1; };
static struct img_cache g_cache[64];
static int g_cache_n;
static GLuint gl_program, gl_tex_y, gl_tex_uv;
static GLint gl_loc_tex_y, gl_loc_tex_uv;

static const char *vert_src =
	"attribute vec2 pos;\n"
	"varying vec2 vuv;\n"
	"void main(void) {\n"
	"  vuv = pos * 0.5 + 0.5;\n"
	"  vuv.y = 1.0 - vuv.y;\n"
	"  gl_Position = vec4(pos, 0.0, 1.0);\n"
	"}\n";

static const char *frag_src =
	"precision mediump float;\n"
	"uniform sampler2D tex_y;\n"
	"uniform sampler2D tex_uv;\n"
	"varying vec2 vuv;\n"
	"void main(void) {\n"
	"  vec3 yuv;\n"
	"  yuv.x = texture2D(tex_y, vuv).r;\n"
	"  yuv.yz = texture2D(tex_uv, vuv).rg;\n"
	"  yuv.x -= 0.0625;\n"
	"  yuv.yz -= 0.5;\n"
	"  gl_FragColor = vec4(1.1643 * yuv.x + 1.5958 * yuv.z,\n"
	"                       1.1643 * yuv.x - 0.39173 * yuv.y - 0.81290 * yuv.z,\n"
	"                       1.1643 * yuv.x + 2.017 * yuv.y, 1.0);\n"
	"}\n";

static int compile_shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	if (!s) { fprintf(stderr, "glCreateShader failed\n"); return 0; }
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "shader compile (%s): %s\n",
			type == GL_VERTEX_SHADER ? "vert" : "frag", log);
	}
	return s;
}

static int init_gl(void)
{
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	gl_program = glCreateProgram();
	glAttachShader(gl_program, vs);
	glAttachShader(gl_program, fs);
	glLinkProgram(gl_program);
	GLint ok = 0;
	glGetProgramiv(gl_program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(gl_program, sizeof log, NULL, log);
		fprintf(stderr, "link: %s\n", log);
		return -1;
	}
	glUseProgram(gl_program);
	gl_loc_tex_y = glGetUniformLocation(gl_program, "tex_y");
	gl_loc_tex_uv = glGetUniformLocation(gl_program, "tex_uv");
	glUniform1i(gl_loc_tex_y, 0);
	glUniform1i(gl_loc_tex_uv, 1);

	glGenTextures(1, &gl_tex_y);
	glBindTexture(GL_TEXTURE_2D, gl_tex_y);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenTextures(1, &gl_tex_uv);
	glBindTexture(GL_TEXTURE_2D, gl_tex_uv);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return 0;
}

/* ------------------------------------------------------------ GBM/EGL */

static struct gbm_device *gbm_dev;
static EGLDisplay egl_dpy;
static EGLContext egl_ctx;
static int g_dri_fd;

static struct fbdev {
	struct gbm_bo *bo;
	uint32_t fb;
	EGLImage img;   /* XRGB8888 image of the scanout BO */
	GLuint tex;     /* texture wrapped around that image (FBO target) */
} fbs[3];
static int n_fbs;

static int video_w, video_h;
static GLuint vbo;

/* CPU accounting state */
static unsigned long long first_cpu = 0;
static long long first_wall = 0;

/* EGL extension entry points (not exported as KHR names by Mesa 20.2:
 * fetch them through eglGetProcAddress) */
static EGLImage (EGLAPIENTRYP eglCreateImageKHR_fp)(EGLDisplay dpy, EGLContext ctx,
						     EGLenum target,
						     EGLClientBuffer buffer,
						     const EGLint *attrib_list);
static EGLBoolean (EGLAPIENTRYP eglDestroyImageKHR_fp)(EGLDisplay dpy, EGLImage image);

static int init_gbm_egl(int dri_fd)
{
	gbm_dev = gbm_create_device(dri_fd);
	if (!gbm_dev) { fprintf(stderr, "gbm_create_device failed\n"); return -1; }

	egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev, NULL);
	if (egl_dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "eglGetPlatformDisplay(gbm) failed (0x%x)\n",
			eglGetError());
		return -1;
	}
	if (!eglInitialize(egl_dpy, NULL, NULL)) {
		fprintf(stderr, "eglInitialize failed\n");
		return -1;
	}
	fprintf(stderr, "EGL %s\n", eglQueryString(egl_dpy, EGL_VERSION));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "eglBindAPI(ES) failed\n");
		return -1;
	}

	eglCreateImageKHR_fp = (EGLImage (EGLAPIENTRYP)(EGLDisplay, EGLContext,
							EGLenum, EGLClientBuffer,
							const EGLint *))
		eglGetProcAddress("eglCreateImageKHR");
	eglDestroyImageKHR_fp = (EGLBoolean (EGLAPIENTRYP)(EGLDisplay, EGLImage))
		eglGetProcAddress("eglDestroyImageKHR");
	if (!eglCreateImageKHR_fp) {
		fprintf(stderr, "eglGetProcAddress(eglCreateImageKHR) failed\n");
		return -1;
	}

const EGLint cfg_attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};
	EGLConfig cfg;
	EGLint ncfg = 0;
	if (!eglChooseConfig(egl_dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
		fprintf(stderr, "eglChooseConfig failed (0x%x)\n", eglGetError());
		return -1;
	}
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
	if (egl_ctx == EGL_NO_CONTEXT) {
		fprintf(stderr, "eglCreateContext failed (0x%x)\n", eglGetError());
		return -1;
	}
	/* surfaceless context: no window/pbuffer needed for FBO rendering */
	if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
		fprintf(stderr, "eglMakeCurrent(surfaceless) failed (0x%x)\n", eglGetError());
		return -1;
	}

	GET_GL(glEGLImageTargetTexture2DOES);
	GET_GL(glDrawArrays);
	GET_GL(glUseProgram);
	GET_GL(glVertexAttribPointer);
	GET_GL(glEnableVertexAttribArray);
	GET_GL(glCreateShader);
	GET_GL(glShaderSource);
	GET_GL(glCompileShader);
	GET_GL(glCreateProgram);
	GET_GL(glAttachShader);
	GET_GL(glLinkProgram);
	GET_GL(glGenTextures);
	GET_GL(glBindTexture);
	GET_GL(glDeleteTextures);
	GET_GL(glUniform1i);
	GET_GL(glGetUniformLocation);
	GET_GL(glGetShaderiv);
	GET_GL(glGetProgramiv);
	GET_GL(glGetShaderInfoLog);
	GET_GL(glGetProgramInfoLog);
	GET_GL(glGetError);
	GET_GL(glGenBuffers);
	GET_GL(glBindBuffer);
	GET_GL(glBufferData);
	GET_GL(glDeleteBuffers);
	GET_GL(glClear);
	GET_GL(glViewport);
	GET_GL(glActiveTexture);
	GET_GL(glTexParameteri);
	GET_GL(glGenFramebuffers);
	GET_GL(glBindFramebuffer);
	GET_GL(glDeleteFramebuffers);
	GET_GL(glFramebufferTexture2D);
	GET_GL(glGetIntegerv);

	if (!glEGLImageTargetTexture2DOES || !glDrawArrays || !glCreateShader ||
	    !glUseProgram || !glUniform1i || !glGenTextures) {
		fprintf(stderr, "GLES2 dispatch incomplete\n");
		return -1;
	}
	return 0;
}

/* create scanout BO k, its EGLImage and FBO-attachable texture */
static int make_scanout(int k, int w, int h)
{
	fbs[k].bo = gbm_bo_create(gbm_dev, w, h, GBM_FORMAT_XRGB8888,
				  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!fbs[k].bo) {
		fprintf(stderr, "gbm_bo_create XRGB8888 scanout failed (k=%d)\n", k);
		return -1;
	}
	int fd = gbm_bo_get_fd(fbs[k].bo);
	if (fd < 0) { fprintf(stderr, "gbm_bo_get_fd failed (k=%d)\n", k); return -1; }
	EGLint attrs[] = {
		EGL_WIDTH, w,
		EGL_HEIGHT, h,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, gbm_bo_get_stride(fbs[k].bo),
		EGL_NONE
	};
	fbs[k].img = eglCreateImageKHR_fp(egl_dpy, EGL_NO_CONTEXT,
					 EGL_LINUX_DMA_BUF_EXT,
					 (EGLClientBuffer)NULL, attrs);
	close(fd);
	if (fbs[k].img == EGL_NO_IMAGE) {
		fprintf(stderr, "eglCreateImageKHR(scanout) failed 0x%x (k=%d)\n",
			eglGetError(), k);
		return -1;
	}
	glGenTextures(1, &fbs[k].tex);
	glBindTexture(GL_TEXTURE_2D, fbs[k].tex);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES_dat)fbs[k].img);
	uint32_t handle = gbm_bo_get_handle(fbs[k].bo).u32;
	uint32_t pitch = gbm_bo_get_stride(fbs[k].bo);
	uint32_t handles[4] = { handle, 0, 0, 0 };
	uint32_t pitches[4] = { pitch, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	if (drmModeAddFB2(g_dri_fd, w, h, DRM_FORMAT_XRGB8888,
			  handles, pitches, offsets, &fbs[k].fb, 0)) {
		fprintf(stderr, "drmModeAddFB2 XRGB8888 failed: %s (k=%d)\n",
			strerror(errno), k);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------- main */

/* KMS state (filled after video dims are known) */
static int g_dri_fd;
static uint32_t g_conn_id, g_crtc_id;
static drmModeModeInfo g_mode;
static int g_scanout_w, g_scanout_h;

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/root/media/bbb_hw.mp4";
	const char *result_file = argc > 2 ? argv[2] : "/root/minplayer-result.txt";
	double run_seconds = argc > 3 ? atof(argv[3]) : 0;   /* 0 = run to EOF */

	av_register_all();

	/* --- KMS: DRM fd --- */
	int dri_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (dri_fd < 0) return die("/dev/dri/card0");
	g_dri_fd = dri_fd;

	/* --- VA display (radeonsi driver) --- */
	VADisplay va_dpy = vaGetDisplayDRM(dri_fd);
	if (!va_dpy) { fprintf(stderr, "vaGetDisplayDRM failed\n"); return 1; }
	int major = 0, minor = 0;
	if (vaInitialize(va_dpy, &major, &minor) != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize failed\n");
		return 1;
	}
	fprintf(stderr, "VA-API %d.%d\n", major, minor);

	/* --- FFmpeg demux/decode with VAAPI hwaccel --- */
	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
		fprintf(stderr, "avformat_open_input %s failed\n", path);
		return 1;
	}
	if (avformat_find_stream_info(fmt, NULL) < 0) {
		fprintf(stderr, "avformat_find_stream_info failed\n");
		return 1;
	}
	int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vs < 0) { fprintf(stderr, "no video stream\n"); return 1; }
	AVCodecParameters *par = fmt->streams[vs]->codecpar;
	AVCodec *dec = avcodec_find_decoder(par->codec_id);
	if (!dec) { fprintf(stderr, "no decoder\n"); return 1; }
	AVCodecContext *ctx = avcodec_alloc_context3(dec);
	if (!ctx) { fprintf(stderr, "avcodec_alloc_context3\n"); return 1; }
	if (avcodec_parameters_to_context(ctx, par) < 0) {
		fprintf(stderr, "avcodec_parameters_to_context\n");
		return 1;
	}
	video_w = ctx->width; video_h = ctx->height;
	fprintf(stderr, "video %dx%d codec %s\n", video_w, video_h, dec->name);

	/* --- KMS: scan connector/crtc, pick a mode close to the video size --- */
	{
		drmModeRes *res = drmModeGetResources(dri_fd);
		if (!res) return die("drmModeGetResources");
		memset(&g_mode, 0, sizeof g_mode);
		for (int i = 0; i < res->count_connectors && !g_conn_id; i++) {
			drmModeConnector *c = drmModeGetConnector(dri_fd, res->connectors[i]);
			if (!c) continue;
			if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
				drmModeEncoder *e = drmModeGetEncoder(dri_fd, c->encoder_id);
				if (e) {
					/* prefer a mode matching the video size, else the smallest */
					int pick = 0;
					for (int j = 0; j < c->count_modes; j++) {
						if (c->modes[j].hdisplay == video_w &&
						    c->modes[j].vdisplay == video_h) {
							pick = j; break;
						}
						if (c->modes[j].hdisplay * c->modes[j].vdisplay <
						    c->modes[pick].hdisplay * c->modes[pick].vdisplay)
							pick = j;
					}
					memcpy(&g_mode, &c->modes[pick], sizeof g_mode);
					g_scanout_w = g_mode.hdisplay;
					g_scanout_h = g_mode.vdisplay;
					g_conn_id = c->connector_id;
					g_crtc_id = e->crtc_id;
					drmModeFreeEncoder(e);
				}
			}
			drmModeFreeConnector(c);
		}
		drmModeFreeResources(res);
	}
	if (!g_conn_id) { fprintf(stderr, "no connected connector\n"); return 1; }
	fprintf(stderr, "connector %u crtc %u mode %dx%d@%d (scanout %dx%d)\n",
		g_conn_id, g_crtc_id, g_mode.hdisplay, g_mode.vdisplay,
		g_mode.vrefresh, g_scanout_w, g_scanout_h);

	AVBufferRef *hw_dev_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
	if (!hw_dev_ctx) { fprintf(stderr, "av_hwdevice_ctx_alloc\n"); return 1; }
	AVHWDeviceContext *hw = (AVHWDeviceContext *)hw_dev_ctx->data;
	AVVAAPIDeviceContext *vad = (AVVAAPIDeviceContext *)hw->hwctx;
	vad->display = va_dpy;
	if (av_hwdevice_ctx_init(hw_dev_ctx) < 0) {
		fprintf(stderr, "av_hwdevice_ctx_init failed\n");
		return 1;
	}
	ctx->hw_device_ctx = av_buffer_ref(hw_dev_ctx);
	ctx->get_format = get_hw_format;

	if (avcodec_open2(ctx, dec, NULL) < 0) {
		fprintf(stderr, "avcodec_open2 failed (hardware decode not usable?)\n");
		return 1;
	}
	fprintf(stderr, "decoder opened, waiting for first frame\n");

	/* --- GBM + EGL --- */
	if (init_gbm_egl(dri_fd) < 0) return 1;
	if (init_gl() < 0) return 1;

	/* --- scanout BOs: XRGB8888, 1 plane, render targets --- */
	n_fbs = 3;
	for (int k = 0; k < n_fbs; k++)
		if (make_scanout(k, g_scanout_w, g_scanout_h) < 0) return 1;
	fprintf(stderr, "gbm: %d scanout BOs %dx%d\n", n_fbs, g_scanout_w, g_scanout_h);

	/* --- set CRTC (X must be stopped before this) --- */
	if (drmModeSetCrtc(dri_fd, g_crtc_id, fbs[0].fb, 0, 0,
			   &g_conn_id, 1, &g_mode)) {
		fprintf(stderr, "drmModeSetCrtc failed: %s (is X still running?)\n",
			strerror(errno));
		return 1;
	}
	fprintf(stderr, "CRTC set, presenting\n");

	drmEventContext evctx; memset(&evctx, 0, sizeof evctx);
	evctx.version = 2;
	evctx.page_flip_handler = flip_handler;

	/* triangle VBO (full-screen, one triangle) */
	float tri[6] = { -1, -1, 3, -1, -1, 3 };
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

	/* --- decode + present loop --- */
	AVPacket pkt; memset(&pkt, 0, sizeof pkt);
	AVFrame *frame = av_frame_alloc();
	if (!frame) { fprintf(stderr, "av_frame_alloc\n"); return 1; }

	FILE *result = fopen(result_file, "w");
	if (result) fprintf(result, "# minplayer: VAAPI->dmabuf->EGL/GLES->KMS\n");

	long clock_ticks = sysconf(_SC_CLK_TCK);
	unsigned long long last_cpu = 0; int have_last_cpu = 0;
	long long last_wall = 0;
	int frames_presented = 0;
	int next_fb = 0;
	int eof = 0;
	long long t0 = now_ns();
	long long next_stat = t0;

	for (;;) {
		/* --- decode --- */
		if (!eof) {
			int r = av_read_frame(fmt, &pkt);
			if (r < 0) {
				eof = 1;
			} else {
				if (pkt.stream_index == vs) {
					int sr = avcodec_send_packet(ctx, &pkt);
					if (sr < 0 && sr != AVERROR(EAGAIN))
						fprintf(stderr, "send_packet: %d\n", sr);
				}
				av_packet_unref(&pkt);
			}
		}

		int got = avcodec_receive_frame(ctx, frame);
		if (got != 0) {
			if (eof) break;
			if (got == AVERROR(EAGAIN))
				continue;
			break;
		}
		/* the hw_frames_ctx is created by the hwaccel on the first frame */
		if (!ctx->hw_frames_ctx) {
			fprintf(stderr, "no hw_frames_ctx after first frame\n");
			break;
		}

		/* --- cached EGL images per VA surface (export once, not per frame) --- */
		VASurfaceID surf = (VASurfaceID)(uintptr_t)frame->data[3];
		int ci = -1;
		for (int k = 0; k < g_cache_n; k++)
			if (g_cache[k].id == surf) { ci = k; break; }
		if (ci < 0 && g_cache_n < (int)(sizeof(g_cache)/sizeof(g_cache[0]))) {
			VADRMPRIMESurfaceDescriptor desc;
			if (vaExportSurfaceHandle(va_dpy, surf,
						  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
						  VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc)
			    != VA_STATUS_SUCCESS) {
				fprintf(stderr, "vaExportSurfaceHandle failed\n");
				break;
			}
			int fd0 = desc.objects[desc.layers[0].object_index[0]].fd;
			int fd1 = desc.objects[desc.layers[1].object_index[0]].fd;
			uint32_t y_off = desc.layers[0].offset[0];
			uint32_t uv_off = desc.layers[1].offset[0];
			uint32_t y_pitch = desc.layers[0].pitch[0];
			uint32_t uv_pitch = desc.layers[1].pitch[0];
			EGLint y_attrs[] = {
				EGL_WIDTH, video_w, EGL_HEIGHT, video_h,
				EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
				EGL_DMA_BUF_PLANE0_FD_EXT, fd0,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, y_off,
				EGL_DMA_BUF_PLANE0_PITCH_EXT, y_pitch,
				EGL_NONE
			};
			EGLint uv_attrs[] = {
				EGL_WIDTH, video_w / 2, EGL_HEIGHT, video_h / 2,
				EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
				EGL_DMA_BUF_PLANE0_FD_EXT, fd1,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, uv_off,
				EGL_DMA_BUF_PLANE0_PITCH_EXT, uv_pitch,
				EGL_NONE
			};
			EGLImage img_y = eglCreateImageKHR_fp(egl_dpy, EGL_NO_CONTEXT,
					EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, y_attrs);
			EGLImage img_uv = eglCreateImageKHR_fp(egl_dpy, EGL_NO_CONTEXT,
					EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, uv_attrs);
			if (img_y == EGL_NO_IMAGE || img_uv == EGL_NO_IMAGE) {
				fprintf(stderr, "eglCreateImageKHR failed 0x%x\n", eglGetError());
				break;
			}
			GLuint ty = 0, tuv = 0;
			glGenTextures(1, &ty); glGenTextures(1, &tuv);
			glBindTexture(GL_TEXTURE_2D, ty);
			glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES_dat)img_y);
			glBindTexture(GL_TEXTURE_2D, tuv);
			glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES_dat)img_uv);
			ci = g_cache_n++;
			g_cache[ci].id = surf; g_cache[ci].y = img_y; g_cache[ci].uv = img_uv;
			g_cache[ci].tex_y = ty; g_cache[ci].tex_uv = tuv;
			g_cache[ci].fd0 = fd0; g_cache[ci].fd1 = fd1;
		}
		if (ci < 0) { fprintf(stderr, "surface cache full\n"); break; }

		/* --- sample the cached textures into the scanout BO --- */
		{
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, g_cache[ci].tex_y);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, g_cache[ci].tex_uv);

			GLuint fb = 0;
			glGenFramebuffers(1, &fb);
			glBindFramebuffer(GL_FRAMEBUFFER, fb);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					       GL_TEXTURE_2D, fbs[next_fb].tex, 0);
			int vb = (g_scanout_h - video_h) / 2;
			glViewport(0, vb > 0 ? vb : 0, g_scanout_w, video_h);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &fb);
		}

		/* --- present --- */
		flip_pending = 1;
		if (drmModePageFlip(dri_fd, g_crtc_id, fbs[next_fb].fb,
				    DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
			/* wait for vblank with a 500 ms timeout so a stuck flip
			 * can't hang the player */
			long long flip_t0 = now_ns();
			while (flip_pending && now_ns() - flip_t0 < 500000000LL)
				drmHandleEvent(dri_fd, &evctx);
			frames_presented++;
		} else {
			fprintf(stderr, "drmModePageFlip failed: %s\n", strerror(errno));
			break;
		}
		next_fb = (next_fb + 1) % n_fbs;
		av_frame_unref(frame);   /* release the hw surface back to the pool */

		/* --- pacing to the video fps (by frame index, not pts) --- */
		if (run_seconds > 0) {
			AVRational fr = fmt->streams[vs]->avg_frame_rate;
			double fps = (fr.num > 0 && fr.den > 0) ?
				     (double)fr.num / fr.den : 30.0;
			long long target = t0 + (long long)(frames_presented * 1e9 / fps);
			long long d = target - now_ns();
			if (d > 0 && d < 500000000LL)
				usleep((useconds_t)(d / 1000));
		}

		/* --- per-second CPU% --- */
		long long w = now_ns();
		if (w - next_stat >= 1000000000LL) {
			char buf[1024];
			FILE *st = fopen("/proc/self/stat", "r");
			int nread = st ? (int)fread(buf, 1, sizeof buf - 1, st) : 0;
			if (st) fclose(st);
			if (nread > 0) {
				buf[nread] = 0;
				char *rp = strrchr(buf, ')');
				if (rp) {
					unsigned long long v[20];
					char *p = rp + 1; int i = 0;
					while (i < 20) {
						while (*p == ' ') p++;
						char *st = p;
						unsigned long long tok = strtoull(p, &p, 10);
						if (p == st) {   /* non-digit token (state 'R'/'S'): skip it */
							if (*p) p++;
							tok = 0;
						}
						v[i++] = tok;
						if (!*p) break;
					}
					/* v[0]=field3 .. v[11]=field14(utime) v[12]=field15(stime) */
					unsigned long long cpu_now = v[11] + v[12];
					if (have_last_cpu) {
						double dt_sec = (double)(w - last_wall) / 1e9;
						if (dt_sec > 0.01) {
							double pct = (double)(cpu_now - last_cpu) * 100.0 /
								     ((double)clock_ticks * dt_sec);
							fprintf(stderr, "cpu %.1f%% frames %d\n",
								pct, frames_presented);
							if (result)
								fprintf(result, "cpu %.1f%% frames %d\n",
									pct, frames_presented);
						}
					}
					if (first_wall == 0) {
						first_cpu = cpu_now;
						first_wall = w;
					}
					last_cpu = cpu_now;
					last_wall = w;
					have_last_cpu = 1;
				}
			}
			next_stat = w + 1000000000LL;
		}

		if (run_seconds > 0 && (w - t0) / 1e9 >= run_seconds) {
			fprintf(stderr, "break at %.1fs (run_seconds=%.1f)\n",
				(double)(w - t0) / 1e9, run_seconds);
			fflush(stderr);
			break;
		}
	}

	/* --- final average over the whole run --- */
	double elapsed = (now_ns() - t0) / 1e9;
	double avg_cpu = 0.0;
	/* average over the whole run: first sample at last_cpu was taken at
	 * last_wall; use utime+stime delta from the FIRST sample to now */
	if (have_last_cpu && elapsed > 0.5) {
		char buf[1024];
		FILE *st = fopen("/proc/self/stat", "r");
		int nread = st ? (int)fread(buf, 1, sizeof buf - 1, st) : 0;
		if (st) fclose(st);
		if (nread > 0) {
			buf[nread] = 0;
			char *rp = strrchr(buf, ')');
			if (rp) {
				unsigned long long v[20];
				char *p = rp + 1; int i = 0;
				while (i < 20) {
					while (*p == ' ') p++;
					char *st2 = p;
					unsigned long long tok = strtoull(p, &p, 10);
					if (p == st2) {
						if (*p) p++;
						tok = 0;
					}
					v[i++] = tok;
					if (!*p) break;
				}
				unsigned long long cpu_now = v[11] + v[12];
				/* total CPU since the first per-second sample */
				double total = (double)(cpu_now - first_cpu) /
					       (double)clock_ticks;
				avg_cpu = total * 100.0 /
					  ((double)(now_ns() - first_wall) / 1e9);
			}
		}
	}
	fprintf(stderr, "done: %d frames presented in %.1fs, avg cpu %.1f%%\n",
		frames_presented, elapsed, avg_cpu);
	if (result) {
		fprintf(result, "frames_presented=%d\nelapsed_s=%.1f\ncpu_percent=%.1f\n",
			frames_presented, elapsed, avg_cpu);
		fclose(result);
	}
	fprintf(stderr, "result file: %s\n", result_file);
	return 0;
}