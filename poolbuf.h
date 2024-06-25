/*
 * poolbuf - https://codeberg.org/sewn/drwl
 * See LICENSE file for copyright and license details.
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-client.h>

#include "drwl.h"

typedef struct {
    struct wl_buffer *wl_buf;
    int32_t stride;
    int32_t size;
    void *data;
} PoolBuf;

#if defined(__linux__) || defined(___FreeBSD__)
#ifdef __linux__
#include <linux/memfd.h>
#endif
#define __POOLBUF_HAS_MEMFD_CREATE
int memfd_create(const char *, unsigned);
#else
static void
randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

static int
create_shm(void)
{
	char name[] = "/drwl-XXXXXX";
	int retries = 100;

	do {
		randname(name + sizeof(name) - 7);
		--retries;
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);
	return -1;
}
#endif

/* Caller must call poolbuf_destroy after finalizing usage */
static PoolBuf *
poolbuf_create(struct wl_shm *shm, int32_t width, int32_t height)
{
	int fd;
	void *data;
	struct wl_shm_pool *shm_pool;
	struct wl_buffer *wl_buf;
	int32_t stride = drwl_stride(width);
	int32_t size = stride * height;
	PoolBuf *buf;

#ifdef __POOLBUF_HAS_MEMFD_CREATE
	fd = memfd_create("drwl-shm-buffer-pool",
		MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
#else
	fd = create_shm();
#endif
    if (fd < 0)
		return NULL;

	if ((ftruncate(fd, size)) < 0) {
		close(fd);
		return NULL;
	}

	data = mmap(NULL, size,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	shm_pool = wl_shm_create_pool(shm, fd, size);
	wl_buf = wl_shm_pool_create_buffer(shm_pool, 0,
			width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(shm_pool);
	close(fd);

	buf = calloc(1, sizeof(PoolBuf));
	buf->wl_buf = wl_buf;
	buf->stride = stride;
	buf->size = size;
	buf->data = data;
	return buf;
}

static void
poolbuf_destroy(PoolBuf *buf)
{
	wl_buffer_destroy(buf->wl_buf);
	munmap(buf->data, buf->size);
	free(buf);
}
