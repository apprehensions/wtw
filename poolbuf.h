/*
 * poolbuf - https://codeberg.org/sewn/drwl
 *
 * Copyright (c) 2023-2024 sewn <sewn@disroot.org>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-client.h>

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
	char name[] = "/poolbuf-XXXXXX";
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

static void
poolbuf_destroy(PoolBuf *buf)
{
	wl_buffer_destroy(buf->wl_buf);
	munmap(buf->data, buf->size);
	free(buf);
}

static void
poolbuf_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	PoolBuf *buf = data;
	poolbuf_destroy(buf);
}

static struct wl_buffer_listener poolbuf_buffer_listener = {
	.release = poolbuf_buffer_release,
};

/* Caller must call poolbuf_destroy after finalizing usage if norelease is passed */
static PoolBuf *
poolbuf_create(struct wl_shm *shm,
		int32_t width, int32_t height, int32_t stride, int norelease)
{
	int fd;
	void *data;
	struct wl_shm_pool *shm_pool;
	struct wl_buffer *wl_buf;
	int32_t size = stride * height;
	PoolBuf *buf;

#ifdef __POOLBUF_HAS_MEMFD_CREATE
	fd = memfd_create("poolbuf-shm-buffer-pool",
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
	if (!norelease)
		wl_buffer_add_listener(wl_buf, &poolbuf_buffer_listener, buf);
	return buf;
}
