/* Verify that gbm_bo_map writes really persist in VRAM beyond the BAR window. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <gbm.h>

static long vram(void){FILE*f=fopen("/sys/class/drm/card0/device/mem_info_vram_used","r");long v=-1;if(f){fscanf(f,"%ld",&v);fclose(f);}return v;}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

int main(int argc,char**argv){
	int n=argc>1?atoi(argv[1]):400;
	int fd=open("/dev/dri/card0",O_RDWR);
	struct gbm_device*dev=gbm_create_device(fd);
	struct gbm_bo**bos=calloc(n,sizeof(*bos));
	unsigned char **data=calloc(n,sizeof(*data));
	long v0=vram();
	double t0=now();
	int ok=0;
	for(int i=0;i<n;i++){
		bos[i]=gbm_bo_create(dev,640,480,GBM_FORMAT_ARGB8888,GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
		if(!bos[i]){printf("create fail at %d\n",i);break;}
		uint32_t stride; void*md=NULL;
		void*p=gbm_bo_map(bos[i],0,0,640,480,GBM_BO_TRANSFER_WRITE,&stride,&md);
		if(!p){printf("map fail at %d\n",i);break;}
		data[i]=malloc(0);
		memset(p,(unsigned char)(i*7+3),stride*480);
		gbm_bo_unmap(bos[i],md);
		ok++;
	}
	double dt=now()-t0;
	printf("wrote %d bo (%.0f MB) in %.1fs = %.1f MB/s, vram_delta=%.0f MB\n",
	       ok, ok*1.5, dt, ok*1.5/dt, (vram()-v0)/1048576.0);
	/* verify the LAST bo: remap and read the first byte */
	if(ok>0){
		uint32_t stride; void*md=NULL;
		void*p=gbm_bo_map(bos[ok-1],0,0,640,480,GBM_BO_TRANSFER_READ,&stride,&md);
		if(p){printf("last bo byte=%u (expected %u)\n",((unsigned char*)p)[0],(unsigned char)((ok-1)*7+3));gbm_bo_unmap(bos[ok-1],md);}
		else printf("remap failed\n");
	}
	fflush(stdout);
	return 0;
}
