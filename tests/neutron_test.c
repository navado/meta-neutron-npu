// SPDX-License-Identifier: MIT
/*
 * Neutron NPU driver tests: corner cases, CPU-side micro-benchmarks, dma-buf export.
 * Runs against /dev/neutron0 with raw ioctls (no NPU job is ever submitted).
 *
 *   neutron_test corner              correctness and corner cases (TAP-style lines)
 *   neutron_test bench [size_MiB...] alloc / first touch / read / sync / free timings (JSON lines)
 *   neutron_test prime [size_MiB]    export a buffer to /dev/dri/card0 (DPU, no IOMMU)
 *   neutron_test alloc <size_MiB> [n] allocate n buffers, report success/errno (JSON line)
 *   neutron_test exhaust <step_MiB> <max_MiB>  allocate until failure; no OOM kill expected
 *
 * Mode (what the running kernel does for new buffers) is read from
 * /sys/module/neutron/parameters/iommu_noncontig; results are tagged with it.
 * Build: gcc -O2 -Wall -o neutron_test neutron_test.c
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <drm/drm.h>
#include "neutron_uapi.h"

#define MiB (1024UL * 1024UL)
#define PG 4096UL

static int dev = -1;
static int failures, tests;
static char mode[16] = "unknown";

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void read_mode(void)
{
	FILE *f = fopen("/sys/module/neutron/parameters/iommu_noncontig", "r");
	int c;

	if (!f) {
		strcpy(mode, "stock");	/* unpatched driver: always contiguous */
		return;
	}
	c = fgetc(f);
	fclose(f);
	strcpy(mode, c == 'Y' ? "noncontig" : "contig");
}

static void ok(int cond, const char *name, const char *fmt, ...)
{
	char detail[256] = "";
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	tests++;
	if (!cond)
		failures++;
	printf("%s %d %s [%s]%s%s\n", cond ? "ok" : "not ok", tests, name, mode,
	       detail[0] ? " # " : "", detail);
	fflush(stdout);
}

/* returns dma-buf fd or -errno */
static int buf_create(size_t size, uint64_t *iova)
{
	struct neutron_uapi_buffer_create a = { .size = (uint32_t)size };
	int r = ioctl(dev, NEUTRON_IOCTL_BUFFER_CREATE, &a);

	if (r < 0)
		return -errno;
	if (iova)
		*iova = a.addr;
	return r;
}

static int cache_sync(int fd, uint32_t off, uint32_t size, int from_dev)
{
	struct neutron_uapi_cache_sync a = { .fd = fd, .offset = off, .size = size,
					     .direction = from_dev };
	return ioctl(dev, NEUTRON_IOCTL_CACHE_SYNC, &a) < 0 ? -errno : 0;
}

static size_t pgalign(size_t n) { return (n + PG - 1) & ~(PG - 1); }

/* one 64-bit word per page plus the last byte; returns mismatches */
static size_t fill_check(volatile uint8_t *p, size_t size, uint64_t seed, int write)
{
	size_t bad = 0, off;

	for (off = 0; off + 8 <= size; off += PG) {
		uint64_t v = seed ^ (off * 0x9e3779b97f4a7c15ULL);

		if (write)
			*(volatile uint64_t *)(p + off) = v;
		else if (*(volatile uint64_t *)(p + off) != v)
			bad++;
	}
	if (write)
		p[size - 1] = (uint8_t)seed;
	else if (p[size - 1] != (uint8_t)seed)
		bad++;
	return bad;
}

static long meminfo_kb(const char *key)
{
	char line[256];
	long v = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == ':') {
			v = atol(line + strlen(key) + 1);
			break;
		}
	fclose(f);
	return v;
}

/* ------------------------------------------------------------ corner cases */

static void t_sizes(void)
{
	static const size_t sizes[] = { 1, PG - 1, PG, PG + 1, 2 * MiB - 1, 2 * MiB + 1,
					3 * MiB + 123, 64 * MiB, 256 * MiB + 4097 };
	unsigned int i;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t sz = sizes[i], len = pgalign(sz), bad;
		uint64_t iova = 0;
		char name[64];
		int fd = buf_create(sz, &iova);
		uint8_t *p;

		snprintf(name, sizeof(name), "size_%zu", sz);
		if (fd < 0) {
			ok(0, name, "create: %s", strerror(-fd));
			continue;
		}
		p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			ok(0, name, "mmap: %s", strerror(errno));
			close(fd);
			continue;
		}
		fill_check(p, sz, sz, 1);
		cache_sync(fd, 0, sz, 0);
		cache_sync(fd, 0, sz, 1);
		bad = fill_check(p, sz, sz, 0);
		ok(bad == 0 && iova != 0 && (iova & (PG - 1)) == 0, name,
		   "iova=0x%llx mismatches=%zu", (unsigned long long)iova, bad);
		munmap(p, len);
		close(fd);
	}
}

static void t_zero_and_huge(void)
{
	int fd = buf_create(0, NULL);

	ok(fd == -EINVAL, "size_0_einval", "got %s", fd < 0 ? strerror(-fd) : "success");
	if (fd >= 0)
		close(fd);
}

static void t_mmap_bounds(void)
{
	size_t sz = 4 * MiB;
	int fd = buf_create(sz, NULL);
	uint8_t *p, *q;

	if (fd < 0) {
		ok(0, "mmap_bounds", "create: %s", strerror(-fd));
		return;
	}
	p = mmap(NULL, sz + PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ok(p == MAP_FAILED, "mmap_beyond_size_refused", "%s",
	   p == MAP_FAILED ? strerror(errno) : "mapped");
	if (p != MAP_FAILED)
		munmap(p, sz + PG);

	/* Offset mapping: the driver resets vm_pgoff, so offset maps show the buffer start.
	 * Pre-existing behaviour; the test records it and checks both modes agree. */
	p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	q = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 1 * MiB);
	if (p != MAP_FAILED && q != MAP_FAILED) {
		*(volatile uint64_t *)p = 0x1111;
		*(volatile uint64_t *)(p + MiB) = 0x2222;
		uint64_t v = *(volatile uint64_t *)q;

		printf("# offset_mmap_semantics [%s] %s\n", mode,
		       v == 0x2222 ? "offset-honoured" : v == 0x1111 ? "maps-buffer-start" : "other");
		ok(1, "offset_mmap_no_crash", "first word at offset 1MiB map = 0x%llx",
		   (unsigned long long)v);
	} else {
		ok(q == MAP_FAILED, "offset_mmap_no_crash", "offset mmap: %s",
		   q == MAP_FAILED ? strerror(errno) : "ok");
	}
	if (p != MAP_FAILED)
		munmap(p, sz);
	if (q != MAP_FAILED)
		munmap(q, PG);
	close(fd);
}

static void t_cache_sync(void)
{
	size_t sz = 8 * MiB + 12345;
	int fd = buf_create(sz, NULL), pipefd[2], r;

	if (fd < 0) {
		ok(0, "cache_sync", "create: %s", strerror(-fd));
		return;
	}
	ok(cache_sync(fd, 0, sz, 0) == 0, "sync_full_to_dev", "");
	ok(cache_sync(fd, 0, sz, 1) == 0, "sync_full_from_dev", "");
	/* ranges straddling 4K / 64K / 2M boundaries (segment edges in noncontig mode) */
	ok(cache_sync(fd, PG - 8, 16, 0) == 0, "sync_straddle_4k", "");
	ok(cache_sync(fd, 2 * MiB - 100, 200, 1) == 0, "sync_straddle_2m", "");
	ok(cache_sync(fd, sz - 1, 1, 0) == 0, "sync_last_byte", "");
	r = cache_sync(fd, sz - 4, 8, 0);
	ok(r == -EINVAL, "sync_past_end_einval", "got %s", r ? strerror(-r) : "success");
	r = cache_sync(fd, 0xfffff000u, 0x2000, 0);
	ok(r == -EINVAL, "sync_wraparound_einval", "got %s", r ? strerror(-r) : "success");
	r = cache_sync(fd, 0, 0, 0);
	ok(r == 0, "sync_zero_size", "got %s", r ? strerror(-r) : "success");
	close(fd);

	r = cache_sync(fd, 0, 16, 0);	/* closed fd */
	ok(r == -EINVAL, "sync_closed_fd_einval", "got %s", r ? strerror(-r) : "success");
	if (!pipe(pipefd)) {
		r = cache_sync(pipefd[0], 0, 16, 0);
		ok(r == -EINVAL, "sync_foreign_fd_einval", "got %s", r ? strerror(-r) : "success");
		close(pipefd[0]);
		close(pipefd[1]);
	}
}

static void t_bad_fd_ioctls(void)
{
	struct neutron_uapi_firmware_load fw = { .buf_fd = 99999 };
	struct neutron_uapi_inference_args inf = { .buf_id = 99999 };
	int r;

	r = ioctl(dev, NEUTRON_IOCTL_FIRMWARE_LOAD, &fw) < 0 ? -errno : 0;
	ok(r != 0, "firmware_load_bad_fd_refused", "got %s", r ? strerror(-r) : "success");
	/* Stock driver: a bad buf_id is queued and oopses later. Patched: -EINVAL. */
	if (strcmp(mode, "stock")) {
		r = ioctl(dev, NEUTRON_IOCTL_INFERENCE_CREATE, &inf);
		if (r >= 0)
			close(r);
		r = r < 0 ? -errno : 0;
		ok(r == -EINVAL, "inference_bad_buf_einval", "got %s", r ? strerror(-r) : "fd");
	} else {
		printf("# skip inference_bad_buf_einval: stock driver would oops\n");
	}
}

static void t_close_with_live_mmap(void)
{
	size_t sz = 32 * MiB, bad;
	int fd = buf_create(sz, NULL);
	uint8_t *p;

	if (fd < 0) {
		ok(0, "close_fd_keep_mmap", "create: %s", strerror(-fd));
		return;
	}
	p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);		/* the mapping holds the dma-buf */
	if (p == MAP_FAILED) {
		ok(0, "close_fd_keep_mmap", "mmap: %s", strerror(errno));
		return;
	}
	fill_check(p, sz, 77, 1);
	bad = fill_check(p, sz, 77, 0);
	ok(bad == 0, "close_fd_keep_mmap", "mismatches=%zu", bad);
	munmap(p, sz);
}

static void t_dmabuf_sync_ioctl(void)
{
	size_t sz = 16 * MiB;
	int fd = buf_create(sz, NULL), r1, r2;
	struct dma_buf_sync s;

	if (fd < 0) {
		ok(0, "dmabuf_sync_ioctl", "create: %s", strerror(-fd));
		return;
	}
	s.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
	r1 = ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
	s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
	r2 = ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
	ok(r1 == 0 && r2 == 0, "dmabuf_begin_end_cpu_access", "begin=%d end=%d", r1, r2);
	close(fd);
}

/* free + page cache; MemFree includes free CMA pages */
static long free_kb(void)
{
	return meminfo_kb("MemFree") + meminfo_kb("Cached");
}

static int leak_round(int base)
{
	int i, errs = 0;

	for (i = base; i < base + 100; i++) {
		size_t sz = (size_t)(1 + (i * 7919) % 48) * MiB + (i % 3) * 777;
		int fd = buf_create(sz, NULL);
		uint8_t *p;

		if (fd < 0) {
			errs++;
			continue;
		}
		p = mmap(NULL, pgalign(sz), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p != MAP_FAILED) {
			fill_check(p, sz, i, 1);
			munmap(p, pgalign(sz));
		}
		close(fd);
	}
	usleep(300000);
	return errs;
}

/*
 * Leak check robust to other processes' memory churn: a warm-up round, then two more.
 * ~2.4 GB goes through each round; a leak shows as a steady decline in both later
 * rounds, background noise does not.
 */
static void t_leak(void)
{
	long r1, r2, r3;
	int errs;

	sync();
	errs = leak_round(0);
	r1 = free_kb();
	errs += leak_round(100);
	r2 = free_kb();
	errs += leak_round(200);
	r3 = free_kb();
	ok(errs == 0 && !(r1 - r2 > 16 * 1024 && r2 - r3 > 16 * 1024 && r1 - r3 > 64 * 1024),
	   "leak_300_buffers", "errors=%d free+cached after rounds 1/2/3: %ld/%ld/%ld MiB",
	   errs, r1 / 1024, r2 / 1024, r3 / 1024);
}

static void t_concurrency(void)
{
	int c, status, bad = 0;

	for (c = 0; c < 4; c++) {
		if (fork() == 0) {
			int i, ecreate = 0, emmap = 0, edata = 0, esync = 0;

			srand(getpid());
			for (i = 0; i < 150; i++) {
				size_t sz = (size_t)(1 + rand() % 24) * MiB + rand() % PG;
				int fd = buf_create(sz, NULL);
				uint8_t *p;

				if (fd < 0) {
					if (!ecreate++)
						fprintf(stderr, "# child %d: create %zu: %s\n", c, sz, strerror(-fd));
					continue;
				}
				p = mmap(NULL, pgalign(sz), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
				if (p == MAP_FAILED) {
					emmap++;
				} else {
					fill_check(p, sz, i, 1);
					/* the order a user must follow: clean to the device, then
					 * invalidate before reading. "from device" alone would
					 * legitimately discard the CPU's dirty lines. */
					if (cache_sync(fd, 0, sz, 0) || cache_sync(fd, 0, sz, 1))
						esync++;
					if (fill_check(p, sz, i, 0))
						edata++;
					munmap(p, pgalign(sz));
				}
				close(fd);
			}
			if (ecreate + emmap + edata + esync)
				fprintf(stderr, "# child %d: create=%d mmap=%d sync=%d data=%d\n",
					c, ecreate, emmap, esync, edata);
			_exit(ecreate + emmap + edata + esync ? 1 : 0);
		}
	}
	for (c = 0; c < 4; c++) {
		wait(&status);
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			bad++;
	}
	ok(bad == 0, "concurrency_4x150", "failed children=%d", bad);
}

/* A new buffer must never show a previous owner's data (security: user space maps it). */
static void t_zeroed(void)
{
	size_t sz = 64 * MiB, k, dirty = 0;
	int round;

	for (round = 0; round < 4; round++) {
		int fd = buf_create(sz, NULL);
		uint64_t *p;

		if (fd < 0)
			break;
		p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p != MAP_FAILED) {
			for (k = 0; k < sz / 8; k++)
				if (p[k])
					dirty++;
			memset(p, 0xa5, sz);	/* poison for the next round */
			munmap(p, sz);
		}
		close(fd);
	}
	ok(dirty == 0, "new_buffer_zeroed", "non-zero words seen in fresh buffers: %zu", dirty);
}

static void t_fork_inherit(void)
{
	size_t sz = 8 * MiB;
	int fd = buf_create(sz, NULL), status;
	uint8_t *p;

	if (fd < 0) {
		ok(0, "fork_shared_mapping", "create: %s", strerror(-fd));
		return;
	}
	p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (fork() == 0) {
		fill_check(p, sz, 4242, 1);
		_exit(0);
	}
	wait(&status);
	ok(fill_check(p, sz, 4242, 0) == 0, "fork_shared_mapping", "child writes seen by parent");
	munmap(p, sz);
}

static int cmd_corner(void)
{
	t_zero_and_huge();
	t_zeroed();
	t_sizes();
	t_mmap_bounds();
	t_cache_sync();
	t_bad_fd_ioctls();
	t_close_with_live_mmap();
	t_dmabuf_sync_ioctl();
	t_fork_inherit();
	t_concurrency();
	t_leak();
	printf("# %s: %d tests, %d failed\n", mode, tests, failures);
	return failures ? 1 : 0;
}

/* ------------------------------------------------------------ micro-benchmarks */

static int cmd_bench(int argc, char **argv)
{
	static const size_t def[] = { 64, 512, 2048 };
	int i, n = argc ? argc : 3;

	for (i = 0; i < n; i++) {
		size_t sz = (argc ? strtoul(argv[i], NULL, 0) : def[i]) * MiB;
		double t0, t_alloc, t_touch, t_read, t_sync_dev, t_sync_cpu, t_free;
		volatile uint64_t sum = 0;
		uint64_t *p;
		size_t k;
		int fd;

		t0 = now();
		fd = buf_create(sz, NULL);
		t_alloc = now() - t0;
		if (fd < 0) {
			printf("{\"bench\":\"buffer\",\"mode\":\"%s\",\"mib\":%zu,\"error\":\"%s\"}\n",
			       mode, sz / MiB, strerror(-fd));
			continue;
		}
		p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			close(fd);
			continue;
		}
		t0 = now();
		memset(p, 0x5a, sz);
		t_touch = now() - t0;
		t0 = now();
		for (k = 0; k < sz / 8; k += 8)
			sum += p[k];
		t_read = now() - t0;
		t0 = now();
		cache_sync(fd, 0, sz > 0xffffffffUL ? 0xfffff000u : sz, 0);
		t_sync_dev = now() - t0;
		t0 = now();
		cache_sync(fd, 0, sz > 0xffffffffUL ? 0xfffff000u : sz, 1);
		t_sync_cpu = now() - t0;
		munmap(p, sz);
		t0 = now();
		close(fd);
		t_free = now() - t0;
		printf("{\"bench\":\"buffer\",\"mode\":\"%s\",\"mib\":%zu,\"alloc_ms\":%.2f,"
		       "\"touch_gbs\":%.2f,\"read_gbs\":%.2f,\"sync_to_dev_ms\":%.2f,"
		       "\"sync_from_dev_ms\":%.2f,\"free_ms\":%.2f}\n",
		       mode, sz / MiB, t_alloc * 1e3, sz / t_touch / 1e9, sz / t_read / 1e9,
		       t_sync_dev * 1e3, t_sync_cpu * 1e3, t_free * 1e3);
		fflush(stdout);
	}
	return 0;
}

/* ------------------------------------------------------------ dma-buf export to the DPU */

static int cmd_prime(size_t mib)
{
	struct drm_prime_handle h = { 0 };
	struct drm_gem_close gc = { 0 };
	int fd = buf_create(mib * MiB, NULL), card, r, expect_refused;

	if (fd < 0) {
		printf("{\"test\":\"prime_dpu\",\"mode\":\"%s\",\"error\":\"create: %s\"}\n", mode,
		       strerror(-fd));
		return 1;
	}
	card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (card < 0) {
		printf("{\"test\":\"prime_dpu\",\"mode\":\"%s\",\"skip\":\"card0: %s\"}\n", mode,
		       strerror(errno));
		close(fd);
		return 0;
	}
	h.fd = fd;
	r = ioctl(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &h) < 0 ? -errno : 0;
	/* Expected: contiguous buffers import; non-contiguous ones are refused (-EINVAL)
	 * because the DPU has no IOMMU. Anything else (success with noncontig) is a bug. */
	{
		/* v4 had cma_first, v5 cma_whole: then a small buffer comes from CMA and imports */
		FILE *f = fopen("/sys/module/neutron/parameters/cma_whole", "r");
		int whole = 0;

		if (!f)
			f = fopen("/sys/module/neutron/parameters/cma_first", "r");
		if (f) {
			whole = fgetc(f) == 'Y';
			fclose(f);
		}
		expect_refused = !strcmp(mode, "noncontig") && !whole;
	}
	printf("{\"test\":\"prime_dpu\",\"mode\":\"%s\",\"mib\":%zu,\"result\":\"%s\",\"expected\":\"%s\"}\n",
	       mode, mib, r ? strerror(-r) : "imported",
	       expect_refused ? "Invalid argument" : "imported");
	if (!r) {
		gc.handle = h.handle;
		ioctl(card, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	close(card);
	close(fd);
	return (expect_refused ? r == -EINVAL : r == 0) ? 0 : 1;
}

/* ------------------------------------------------------------ allocation under pressure */

static int cmd_alloc(size_t mib, int n)
{
	int i, okc = 0, fds[64];
	double t0 = now();
	char err[64] = "";

	for (i = 0; i < n && i < 64; i++) {
		fds[i] = buf_create(mib * MiB, NULL);
		if (fds[i] >= 0)
			okc++;
		else if (!err[0])
			snprintf(err, sizeof(err), "%s", strerror(-fds[i]));
	}
	printf("{\"test\":\"alloc\",\"mode\":\"%s\",\"mib\":%zu,\"n\":%d,\"ok\":%d,\"error\":\"%s\","
	       "\"ms\":%.1f,\"cma_free_mib\":%ld}\n", mode, mib, n, okc, err, (now() - t0) * 1e3,
	       meminfo_kb("CmaFree") / 1024);
	for (i = 0; i < n && i < 64; i++)
		if (fds[i] >= 0)
			close(fds[i]);
	return 0;
}

static int cmd_exhaust(size_t step, size_t max)
{
	int fds[512], n = 0, i, e = 0;
	size_t got = 0;
	double t0 = now();

	while (got + step <= max && n < 512) {
		int fd = buf_create(step * MiB, NULL);

		if (fd < 0) {
			e = -fd;
			break;
		}
		fds[n++] = fd;
		got += step;
	}
	printf("{\"test\":\"exhaust\",\"mode\":\"%s\",\"step_mib\":%zu,\"got_mib\":%zu,"
	       "\"stopped\":\"%s\",\"ms\":%.0f,\"memavail_mib\":%ld,\"swapfree_mib\":%ld}\n",
	       mode, step, got, e ? strerror(e) : "max reached", (now() - t0) * 1e3,
	       meminfo_kb("MemAvailable") / 1024, meminfo_kb("SwapFree") / 1024);
	fflush(stdout);
	for (i = 0; i < n; i++)
		close(fds[i]);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s corner|bench [MiB..]|prime [MiB]|alloc MiB [n]|exhaust step max\n",
			argv[0]);
		return 2;
	}
	dev = open("/dev/neutron0", O_RDWR | O_CLOEXEC);
	if (dev < 0) {
		perror("/dev/neutron0");
		return 2;
	}
	read_mode();
	if (!strcmp(argv[1], "corner"))
		return cmd_corner();
	if (!strcmp(argv[1], "bench"))
		return cmd_bench(argc - 2, argv + 2);
	if (!strcmp(argv[1], "prime"))
		return cmd_prime(argc > 2 ? strtoul(argv[2], NULL, 0) : 16);
	if (!strcmp(argv[1], "alloc") && argc > 2)
		return cmd_alloc(strtoul(argv[2], NULL, 0), argc > 3 ? atoi(argv[3]) : 1);
	if (!strcmp(argv[1], "exhaust") && argc > 3)
		return cmd_exhaust(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
	fprintf(stderr, "unknown command\n");
	return 2;
}
