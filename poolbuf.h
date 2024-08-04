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
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-client.h>
#ifdef __linux__
#include <linux/memfd.h>
#endif

typedef struct {
    struct wl_buffer *wl_buf;
    int32_t stride;
    int32_t size;
    void *data;
} PoolBuf;

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

#if defined(__linux__) || \
	((defined(__FreeBSD__) && (__FreeBSD_version >= 1300048)))
	fd = memfd_create("poolbuf-shm-buffer-pool",
		MFD_CLOEXEC | MFD_ALLOW_SEALING |
#if defined(MFD_NOEXEC_SEAL)
		MFD_NOEXEC_SEAL
#else
		0
#endif
	);
#else
	char template[] = "/tmp/poolbuf-XXXXXX";
#if defined(__OpenBSD__)
	fd = shm_mkstemp(template);
#else
	fd = mkostemp(template, O_CLOEXEC);
#endif
	unlink(template);
#endif
    if (fd < 0)
		return NULL;

	if ((ftruncate(fd, size)) < 0) {
		close(fd);
		return NULL;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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
