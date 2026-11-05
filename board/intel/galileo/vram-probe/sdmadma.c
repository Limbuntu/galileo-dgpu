/* Minimal SDMA linear copy from a GTT staging buffer into a VRAM buffer,
 * then verify the destination by CPU-mapping it (dst is small, inside BAR1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>

static int alloc(amdgpu_device_handle dev, uint32_t dom, size_t sz, amdgpu_bo_handle *bo, void **cpu) {
	struct amdgpu_bo_alloc_request r; memset(&r, 0, sizeof r);
	r.alloc_size = sz; r.phys_alignment = 4096; r.preferred_heap = dom;
	if (amdgpu_bo_alloc(dev, &r, bo)) return -1;
	if (cpu && amdgpu_bo_cpu_map(*bo, cpu)) return -1;
	return 0;
}

int main(void) {
	int fd = open("/dev/dri/card0", O_RDWR);
	amdgpu_device_handle dev; uint32_t ma, mi;
	if (amdgpu_device_initialize(fd, &ma, &mi, &dev)) { perror("init"); return 1; }
	amdgpu_context_handle ctx;
	if (amdgpu_cs_ctx_create(dev, &ctx)) { perror("ctx"); return 1; }

	const size_t SZ = 64 * 1024;
	amdgpu_bo_handle src, dst, ibbo;
	void *psrc = NULL, *pdst = NULL, *pib = NULL;
	if (alloc(dev, AMDGPU_GEM_DOMAIN_GTT, SZ, &src, &psrc)) { perror("src"); return 1; }
	if (alloc(dev, AMDGPU_GEM_DOMAIN_VRAM, SZ, &dst, &pdst)) { perror("dst"); return 1; }
	if (alloc(dev, AMDGPU_GEM_DOMAIN_GTT, 4096, &ibbo, &pib)) { perror("ib"); return 1; }

	for (size_t i = 0; i < SZ; i++) ((unsigned char *)psrc)[i] = (unsigned char)(i * 31 + 7);
	memset(pdst, 0, SZ);

	uint64_t va_src = 0x100000000ULL, va_dst = 0x100200000ULL, va_ib = 0x100400000ULL;
	if (amdgpu_bo_va_op(src, 0, SZ, va_src, 0, AMDGPU_VA_OP_MAP)) { perror("va src"); return 1; }
	if (amdgpu_bo_va_op(dst, 0, SZ, va_dst, 0, AMDGPU_VA_OP_MAP)) { perror("va dst"); return 1; }
	if (amdgpu_bo_va_op(ibbo, 0, 4096, va_ib, 0, AMDGPU_VA_OP_MAP)) { perror("va ib"); return 1; }

	uint32_t *ib = (uint32_t *)pib;
	int n = 0;
	ib[n++] = 1 | (0 << 8);            /* SDMA_OP_COPY | SUBOP_COPY_LINEAR */
	ib[n++] = (uint32_t)SZ;            /* byte count */
	ib[n++] = 0;                       /* endian swap */
	ib[n++] = (uint32_t)va_src;
	ib[n++] = (uint32_t)(va_src >> 32);
	ib[n++] = (uint32_t)va_dst;
	ib[n++] = (uint32_t)(va_dst >> 32);
	memset(ib + n, 0, 4096 - n * 4);
	amdgpu_bo_cpu_unmap(ibbo);

	amdgpu_bo_handle res[3] = { ibbo, src, dst };
	amdgpu_bo_list_handle list;
	if (amdgpu_bo_list_create(dev, 3, res, NULL, &list)) { perror("bolist"); return 1; }

	struct amdgpu_cs_ib_info ib_info; memset(&ib_info, 0, sizeof ib_info);
	ib_info.ib_mc_address = va_ib;
	ib_info.size = 7 * 4;
	struct amdgpu_cs_request req; memset(&req, 0, sizeof req);
	req.ip_type = AMDGPU_HW_IP_DMA;
	req.ring = 0;
	req.resources = list;
	req.number_of_ibs = 1;
	req.ibs = &ib_info;

	int r = amdgpu_cs_submit(ctx, 0, &req, 1);
	printf("submit=%d seq=%llu\n", r, (unsigned long long)req.seq_no);
	if (r) return 1;

	struct amdgpu_cs_fence fence; memset(&fence, 0, sizeof fence);
	fence.context = ctx; fence.ip_type = AMDGPU_HW_IP_DMA; fence.ring = 0; fence.fence = req.seq_no;
	uint32_t expired = 0;
	for (int i = 0; i < 200; i++) {
		if (amdgpu_cs_query_fence_status(&fence, 10000000ULL, 0, &expired) == 0 && expired) break;
		usleep(10000);
	}
	printf("fence expired=%u\n", expired);

	int bad = 0, first = -1;
	for (size_t i = 0; i < SZ; i++) {
		if (((unsigned char *)pdst)[i] != (unsigned char)(i * 31 + 7)) { if (first < 0) first = i; bad++; }
	}
	printf("verify: %s (bad=%d first=%d dst0=%u src0=%u)\n",
	       bad ? "MISMATCH" : "OK", bad, first, ((unsigned char *)pdst)[0], ((unsigned char *)psrc)[0]);
	return 0;
}
