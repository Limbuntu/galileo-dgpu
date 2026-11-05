/* Fill N per-frame VRAM BOs via SDMA copies from a GTT staging ring.
 * Proves that a full 2.2 GB clip can live in VRAM without system RAM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>
#include <drm_fourcc.h>
#include <xf86drmMode.h>

static long sysf(const char *p){FILE*f=fopen(p,"r");long v=-1;if(f){fscanf(f,"%ld",&v);fclose(f);}return v;}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

static amdgpu_device_handle dev; static amdgpu_context_handle ctx;

static amdgpu_bo_handle alloc(uint32_t dom, size_t sz, void **cpu) {
	struct amdgpu_bo_alloc_request r; memset(&r,0,sizeof r);
	r.alloc_size=sz; r.phys_alignment=4096; r.preferred_heap=dom;
	amdgpu_bo_handle b;
	if (amdgpu_bo_alloc(dev,&r,&b)) return NULL;
	if (cpu && amdgpu_bo_cpu_map(b,cpu)) return NULL;
	return b;
}

int main(int argc, char **argv) {
	int n = argc > 1 ? atoi(argv[1]) : 1800;
	int do_fb = argc > 2 ? atoi(argv[2]) : 0;
	int drmfd = open("/dev/dri/card0", O_RDWR);
	size_t fsz = 640u*480*4;
	int fd = open("/dev/dri/card0", O_RDWR);
	uint32_t ma, mi;
	if (amdgpu_device_initialize(fd,&ma,&mi,&dev)) { perror("init"); return 1; }
	if (amdgpu_cs_ctx_create(dev,&ctx)) { perror("ctx"); return 1; }

	amdgpu_bo_handle stg[4], ibbo; void *pstg[4], *pib;
	for (int k=0;k<4;k++) { stg[k]=alloc(AMDGPU_GEM_DOMAIN_GTT,fsz,&pstg[k]); if(!stg[k]){printf("stg fail\n");return 1;} }
	ibbo = alloc(AMDGPU_GEM_DOMAIN_GTT,4096,&pib); if(!ibbo){printf("ib fail\n");return 1;}

	uint64_t va_stg[4], va_ib = 0x900000000ULL;
	for (int k=0;k<4;k++) { va_stg[k]=0x800000000ULL+k*0x200000ULL;
		if (amdgpu_bo_va_op(stg[k],0,fsz,va_stg[k],0,AMDGPU_VA_OP_MAP)) { printf("va stg %d fail\n",k); return 1; } }
	if (amdgpu_bo_va_op(ibbo,0,4096,va_ib,0,AMDGPU_VA_OP_MAP)) { printf("va ib fail\n"); return 1; }

	amdgpu_bo_handle *dst = calloc(n,sizeof(*dst));
	double t0 = now();
	int ok = 0;
	for (int i=0;i<n;i++) {
		dst[i] = alloc(AMDGPU_GEM_DOMAIN_VRAM, fsz, NULL);
		if (!dst[i]) { printf("vram alloc failed at %d (%.0f MB)\n", i, i*fsz/1048576.0); break; }
		uint64_t va = 0x100000000ULL + (uint64_t)i * 0x200000ULL;
		if (amdgpu_bo_va_op(dst[i],0,fsz,va,0,AMDGPU_VA_OP_MAP)) { printf("va dst %d fail\n",i); break; }
		int k = i & 3;
		memset(pstg[k], i & 0xff, fsz);
		uint32_t *ib = (uint32_t*)pib; int w=0;
		ib[w++] = 1;                 /* COPY_LINEAR */
		ib[w++] = (uint32_t)fsz;
		ib[w++] = 0;
		ib[w++] = (uint32_t)va_stg[k];
		ib[w++] = (uint32_t)(va_stg[k]>>32);
		ib[w++] = (uint32_t)va;
		ib[w++] = (uint32_t)(va>>32);
		amdgpu_bo_handle res[3] = { ibbo, stg[k], dst[i] };
		amdgpu_bo_list_handle list;
		if (amdgpu_bo_list_create(dev,3,res,NULL,&list)) break;
		struct amdgpu_cs_ib_info ii; memset(&ii,0,sizeof ii);
		ii.ib_mc_address=va_ib; ii.size=7*4;
		struct amdgpu_cs_request rq; memset(&rq,0,sizeof rq);
		rq.ip_type=AMDGPU_HW_IP_DMA; rq.resources=list; rq.number_of_ibs=1; rq.ibs=&ii;
		if (amdgpu_cs_submit(ctx,0,&rq,1)) { printf("submit failed at %d\n",i); break; }
		struct amdgpu_cs_fence f; memset(&f,0,sizeof f);
		f.context=ctx; f.ip_type=AMDGPU_HW_IP_DMA; f.fence=rq.seq_no;
		uint32_t exp=0;
		for (int t=0;t<500;t++){ if(!amdgpu_cs_query_fence_status(&f,5000000ULL,0,&exp)&&exp) break; }
		if (do_fb) {
			uint32_t h = 0, pitch = 640*4, off = 0, fb = 0;
			if (amdgpu_bo_export(dst[i], amdgpu_bo_handle_type_kms, &h)) { printf("export fail %d\n", i); break; }
			if (drmModeAddFB2(fd, 640, 480, DRM_FORMAT_ARGB8888, &h, &pitch, &off, &fb, 0)) { perror("addfb"); printf("i=%d handle=%u pitch=%u\n", i, h, pitch); break; }
		}
		amdgpu_bo_list_destroy(list);
		ok++;
		if (do_fb && ok % 300 == 0) {
			FILE*f=fopen("/proc/meminfo","r");char k[64];long v,fr=-1,sh=-1;
			while(fscanf(f,"%63s %ld",k,&v)==2){ if(!strcmp(k,"MemFree:"))fr=v; if(!strcmp(k,"Shmem:"))sh=v; }
			fclose(f);
			printf("  %d frames: memfree=%ldkB shmem=%ldkB vram=%.0fMB\n", ok, fr, sh,
			       sysf("/sys/class/drm/card0/device/mem_info_vram_used")/1048576.0);
			fflush(stdout);
		}
	}
	double dt = now()-t0;
	printf("filled %d frames (%.0f MB) in %.1fs = %.1f MB/s\n", ok, ok*fsz/1048576.0, dt, ok*fsz/1048576.0/dt);
	printf("vram_used=%.0f MB gtt=%ld MB memfree=%ld kB\n",
	       sysf("/sys/class/drm/card0/device/mem_info_vram_used")/1048576.0,
	       sysf("/sys/class/drm/card0/device/mem_info_gtt_used")/1048576,
	       ({FILE*f=fopen("/proc/meminfo","r");char k[64];long v;long r=-1;while(fscanf(f,"%63s %ld",k,&v)==2)if(!strcmp(k,"MemFree:")){r=v;break;}fclose(f);r;}));
	fflush(stdout);
	sleep(5);
	return 0;
}
