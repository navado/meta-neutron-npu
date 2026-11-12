// SPDX-License-Identifier: MIT
/*
 * Neutron buffer -> Mali GPU (OpenCL, cl_arm_import_memory_dma_buf).
 * The Mali has its own MMU (not the SoC SMMU), so it can map a non-contiguous dma-buf;
 * this checks that it does, that the data is right, and what it costs in bandwidth.
 *
 *   gpu_test [MiB] [iterations]     JSON lines on stdout
 *
 * For each run: import a Neutron buffer, run an in-place kernel (p = p*3+1, uint4),
 * verify every word from the CPU side, then time the kernel on the imported buffer
 * and on a GPU-allocated buffer of the same size (baseline).
 * Build: gcc -O2 -Wall -I<CL headers> -o gpu_test gpu_test.c -lOpenCL
 */
#define CL_TARGET_OPENCL_VERSION 300
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <CL/cl.h>
#include <CL/cl_ext.h>
#include "neutron_uapi.h"

#define MiB (1024UL * 1024UL)

static const char *src =
	"__kernel void k(__global uint4 *p) { size_t i = get_global_id(0); p[i] = p[i] * 3u + 1u; }\n";

static char mode[16] = "stock";

static void read_mode(void)
{
	FILE *f = fopen("/sys/module/neutron/parameters/iommu_noncontig", "r");

	if (f) {
		strcpy(mode, fgetc(f) == 'Y' ? "noncontig" : "contig");
		fclose(f);
	}
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* median kernel time in seconds over iters runs, via profiling events */
static double time_kernel(cl_command_queue q, cl_kernel k, cl_mem m, size_t n4, int iters)
{
	double t[64];
	int i;

	clSetKernelArg(k, 0, sizeof(m), &m);
	for (i = 0; i < iters && i < 64; i++) {
		cl_event ev;
		cl_ulong a, b;

		clEnqueueNDRangeKernel(q, k, 1, NULL, &n4, NULL, 0, NULL, &ev);
		clWaitForEvents(1, &ev);
		clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(a), &a, NULL);
		clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof(b), &b, NULL);
		clReleaseEvent(ev);
		t[i] = (b - a) / 1e9;
	}
	qsort(t, i, sizeof(double), cmp_d);
	return t[i / 2];
}

int main(int argc, char **argv)
{
	size_t mib = argc > 1 ? strtoul(argv[1], NULL, 0) : 256, size = mib * MiB, n = size / 4, k, bad = 0;
	int iters = argc > 2 ? atoi(argv[2]) : 20, dev, fd;
	struct neutron_uapi_buffer_create bc = { .size = (uint32_t)size };
	struct neutron_uapi_cache_sync cs = { 0 };
	cl_import_properties_arm props[] = { CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM, 0 };
	cl_platform_id plat;
	cl_device_id gpu;
	cl_context ctx;
	cl_command_queue q;
	cl_program prog;
	cl_kernel kern;
	cl_mem imp, own;
	cl_int err;
	uint32_t *p;
	char name[128] = "";
	double t_imp, t_own;
	cl_queue_properties qp[] = { CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0 };

	read_mode();
	dev = open("/dev/neutron0", O_RDWR | O_CLOEXEC);
	if (dev < 0 || (fd = ioctl(dev, NEUTRON_IOCTL_BUFFER_CREATE, &bc)) < 0) {
		printf("{\"test\":\"gpu\",\"mode\":\"%s\",\"error\":\"neutron buffer: %s\"}\n", mode, strerror(errno));
		return 1;
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (k = 0; k < n; k++)
		p[k] = (uint32_t)k;
	cs.fd = fd; cs.size = (uint32_t)size; cs.direction = 0;	/* clean to memory */
	ioctl(dev, NEUTRON_IOCTL_CACHE_SYNC, &cs);

	if (clGetPlatformIDs(1, &plat, NULL) || clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 1, &gpu, NULL)) {
		printf("{\"test\":\"gpu\",\"mode\":\"%s\",\"error\":\"no OpenCL GPU\"}\n", mode);
		return 1;
	}
	clGetDeviceInfo(gpu, CL_DEVICE_NAME, sizeof(name), name, NULL);
	ctx = clCreateContext(NULL, 1, &gpu, NULL, NULL, &err);
	q = clCreateCommandQueueWithProperties(ctx, gpu, qp, &err);
	prog = clCreateProgramWithSource(ctx, 1, &src, NULL, &err);
	clBuildProgram(prog, 1, &gpu, "", NULL, NULL);
	kern = clCreateKernel(prog, "k", &err);

	/* extension entry points are not exported by the ICD loader: look it up */
	{
		typedef cl_mem (*import_fn)(cl_context, cl_mem_flags, const cl_import_properties_arm *,
					    void *, size_t, cl_int *);
		import_fn import = (import_fn)clGetExtensionFunctionAddressForPlatform(plat, "clImportMemoryARM");

		if (!import) {
			printf("{\"test\":\"gpu\",\"mode\":\"%s\",\"gpu\":\"%s\",\"mib\":%zu,\"import\":\"no clImportMemoryARM\"}\n",
			       mode, name, mib);
			return 1;
		}
		imp = import(ctx, CL_MEM_READ_WRITE, props, &fd, size, &err);
	}
	if (err != CL_SUCCESS) {
		/* a refused import is a result too: print it and stop */
		printf("{\"test\":\"gpu\",\"mode\":\"%s\",\"gpu\":\"%s\",\"mib\":%zu,\"import\":\"error %d\"}\n",
		       mode, name, mib, err);
		return 0;
	}

	/* correctness: one pass, then read back through the CPU mapping */
	{
		size_t n4 = n / 4;

		clSetKernelArg(kern, 0, sizeof(imp), &imp);
		clEnqueueNDRangeKernel(q, kern, 1, NULL, &n4, NULL, 0, NULL, NULL);
		clFinish(q);
	}
	cs.direction = 1;	/* invalidate before the CPU reads */
	ioctl(dev, NEUTRON_IOCTL_CACHE_SYNC, &cs);
	for (k = 0; k < n; k++)
		if (p[k] != (uint32_t)k * 3u + 1u)
			bad++;

	t_imp = time_kernel(q, kern, imp, n / 4, iters);
	own = clCreateBuffer(ctx, CL_MEM_READ_WRITE, size, NULL, &err);
	t_own = err == CL_SUCCESS ? time_kernel(q, kern, own, n / 4, iters) : 0;

	printf("{\"test\":\"gpu\",\"mode\":\"%s\",\"gpu\":\"%s\",\"mib\":%zu,\"import\":\"ok\","
	       "\"mismatches\":%zu,\"imported_gbs\":%.2f,\"gpu_alloc_gbs\":%.2f,\"iters\":%d}\n",
	       mode, name, mib, bad, 2.0 * size / t_imp / 1e9, t_own ? 2.0 * size / t_own / 1e9 : 0.0, iters);

	clReleaseMemObject(imp);
	if (own)
		clReleaseMemObject(own);
	clReleaseKernel(kern);
	clReleaseProgram(prog);
	clReleaseCommandQueue(q);
	clReleaseContext(ctx);
	munmap(p, size);
	close(fd);
	return bad ? 1 : 0;
}
