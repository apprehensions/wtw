#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <err.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <poll.h>
#include <wlr/util/box.h>
#include <wayland-client.h>
#include <pixman-1/pixman.h>

#include "arg.h"
#include "drwl.h"
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#define INITIAL_CAPACITY 2

#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))

char *argv0;

static pixman_color_t colors[2] = {
	{ 0xbbbb, 0xbbbb, 0xbbbb, 0xffff },
	{ 0x0000, 0x0000, 0x0000, 0x0000 }
};

static char delimeter[] = "\4";
static const char *font_name = "monospace:size=16";
static int period = 5;

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_shm *shm;
static struct wl_compositor *compositor;
static struct wl_surface *surface;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct fcft_font *font;
static int wfd;

static struct wlr_box geom = {0, 0, 0, 0};

static char **cmd;
static pid_t cmdpid;
static FILE *inputf;
static char *text;
static size_t len;
static size_t cap;
static int spipe[2];

static bool closed = false;

pixman_color_t
parse_color(const char *hex_color)
{
	uint32_t h = strtoul(hex_color, NULL, 16);
	return (pixman_color_t){
		.red   = ((h >> 24) & 0xFF) * 0x101,
		.green = ((h >> 16) & 0xFF) * 0x101,
		.blue  = ((h >> 8) & 0xFF) * 0x101,
		.alpha = (h & 0xFF) * 0x101,
	};
}

static void
signal_handler(int s)
{
	if (-1 == write(spipe[1], s == SIGCHLD ? "c" : "a", 1))
		abort();
}

static int
start_cmd()
{
	int fds[2];
	if (pipe(fds) == -1) {
		perror("pipe:");
		return 1;
	}

	inputf = fdopen(fds[0], "r");
	if (inputf == NULL) {
		perror("pipe:");
		return 1;
	}

	cmdpid = fork();
	switch (cmdpid) {
	case -1:
		perror("fork:");
		return 1;
	case 0:
		close(spipe[0]);
		close(spipe[1]);
		close(wfd);
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		setpgid(0, 0);
		execvp(cmd[0], cmd);
		exit(1);
	default:
		break;
	}

	close(fds[1]);
	return 0;
}

static int
reap()
{
	for (;;) {
		int wstatus;
		pid_t p = waitpid(-1, &wstatus, cmdpid == 0 ? WNOHANG : 0);
		if (p == -1) {
			if (cmdpid == 0 && errno == ECHILD) {
				errno = 0;
				break;
			}
			return -1;
		}
		if (p == 0)
			break;
		if (p == cmdpid && (WIFEXITED(wstatus) || WIFSIGNALED(wstatus))) {
			cmdpid = 0;
		}
	}
	return 0;
}

static int
read_text()
{
	int dlen = strlen(delimeter);

	len = 0;
	for (;;) {
		if (len + dlen + 2 > cap) {
			// buffer must have sufficient capacity to
			// store delimeter string, \n and \0 in one read
			cap = cap ? cap * 2 : INITIAL_CAPACITY;
			if (!(text = realloc(text, cap))) {
				perror("realloc:");
				return -1;
			}
		}

		char *line = &text[len];
		if (fgets(line, cap - len, inputf) == NULL) {
			if (!feof(inputf)) {
				perror("fgets:");
				return -1;
			}

			if (fclose(inputf) == -1) {
				perror("fclose:");
				return -1;
			}
			inputf = NULL;
			break;
		}

		int llen = strlen(line);

		if (line[llen - 1] == '\n') {
			line[--llen] = '\0';
			len += llen + 1;
		} else {
			len += llen;
		}

		if (llen == dlen && strcmp(line, delimeter) == 0) {
			len -= dlen + 2;
			break;
		}
	}

	return 0;
}

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static void
render()
{
	uint32_t stride = geom.width * 4;
	uint32_t size = stride * geom.height;

	int fd = memfd_create("wtw-wayland-shm-buffer-pool",
		MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd == -1)
		return;

	if ((ftruncate(fd, size)) == -1) {
		close(fd);
		return;
	}

	uint32_t *data = mmap(NULL, size,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			geom.width, geom.height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);

	pixman_image_t *pix = pixman_image_create_bits_no_clear(
		PIXMAN_a8r8g8b8, geom.width, geom.height, data, stride);
	pixman_region32_t clip;
	pixman_region32_init_rect(&clip, 0, 0, geom.width, geom.height);
	pixman_image_set_clip_region32(pix, &clip);
	pixman_region32_fini(&clip);

	int y = geom.y;
	for (char *line = text; line < text + len; line += strlen(line) + 1) {
		drwl_text(pix, font, geom.x, y, geom.width, font->height, 0, line, &colors[0], &colors[1]);
		y += font->height;
	}

	pixman_image_unref(pix);
	munmap(data, size);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, geom.width, geom.height);
	wl_surface_commit(surface);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(surface, serial);

	if ((geom.width != 0 && geom.height != 0) && w == geom.width && h == geom.height)
		return;

	geom.width = w;
	geom.height = h;
	render();
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
	closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = &layer_surface_configure,
    .closed = &layer_surface_closed,
};

static void
registry_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(wl_registry, name, &zwlr_layer_shell_v1_interface, 2);
}

static void
registry_global_remove(void *data,
		struct wl_registry *wl_registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void
usage(void)
{
	fprintf(stderr,
		"usage: wtw [-b rrggbbaa] [-c rrggbbaa] [-f font] [-p period]\n"
		"           [-w pos] [-h pos] [-x pos] [-y pos] command [arg ...]\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	int exit_code = EXIT_FAILURE;

	ARGBEGIN {
	case '?': usage();
	case 'b': colors[1] = parse_color(EARGF(usage())); break;
	case 'c': colors[0] = parse_color(EARGF(usage())); break;
	case 'f': font_name = EARGF(usage()); break;
	case 'p': period = atoi(EARGF(usage())); break;
	case 'w': geom.width = atoi(EARGF(usage())); break;
	case 'h': geom.height = atoi(EARGF(usage())); break;
	case 'x': geom.x = atoi(EARGF(usage())); break;
	case 'y': geom.y = atoi(EARGF(usage())); break;
	default:
		warn("bad option: -%c", ARGC());
		usage();
	} ARGEND

	if (argc == 0)
		usage();

	cmd = argv;

	if (!(display = wl_display_connect(NULL))) {
		fprintf(stderr, "could not connect to display\n");
		return exit_code;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "wayland roundtrip failed\n");
		goto err;
	}

	if (!compositor || !shm || !layer_shell) {
		fprintf(stderr, "wl_compositor, wl_shm or zwlr_layer_shell_v1 is missing\n");
		goto err;
	}

	if (pipe(spipe) == -1) {
		perror("pipe:");
		goto err;
	}

	struct sigaction sa = {0};
	sa.sa_handler = signal_handler;
	sa.sa_flags = SA_RESTART;

	if (sigaction(SIGCHLD, &sa, NULL) == -1 || sigaction(SIGALRM, &sa, NULL) == -1) {
		perror("pipe:");
		goto err;
	}

	fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_WARNING);
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);
	if (!(font = fcft_from_name(1, &font_name, NULL))) {
		fprintf(stderr, "bad font\n");
		goto err;
	}

	exit_code = EXIT_SUCCESS;

	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "text");

	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);

	wl_surface_commit(surface);
	wfd = wl_display_get_fd(display);
	
	bool restart_now = true;

	while (!closed) {
		wl_display_flush(display);

		if (restart_now && cmdpid == 0 && inputf == NULL) {
			restart_now = false;
			start_cmd();
		}

		int inputfd = 0;
		if (inputf != NULL) {
			inputfd = fileno(inputf);
		}

		struct pollfd fds[] = {
			{ .fd = spipe[0], .events = POLLIN },
			{ .fd = wfd,      .events = POLLIN },
			{ .fd = inputfd,  .events = POLLIN }
		};

		int fds_len = LENGTH(fds);
		if (inputfd == 0) {
			fds_len--;
		}

		if (poll(fds, LENGTH(fds), -1) == -1) {
			if (errno == EINTR) {
				errno = 0;
				continue;
			}
			perror("poll:");
			exit_code = EXIT_FAILURE;
			goto err;
		}
		if (inputf && (fds[2].revents & POLLIN || fds[2].revents & POLLHUP)) {
			if (read_text() < 0) {
				exit_code = EXIT_FAILURE;
				goto err;
			}
			render();
		}

		if (fds[0].revents & POLLIN) {
			char s;
			if (read(spipe[0], &s, 1) == -1) {
				perror("read:");
				exit_code = EXIT_FAILURE;
				goto err;
			}

			if (s == 'c') {
				if (reap() < 0) {
					perror("waitpid:");
					exit_code = EXIT_FAILURE;
					goto err;
				}
				if (period < 0) {
					restart_now = true;
				} else if (!restart_now) {
					alarm(period);
				}
			} else if (s == 'a' && cmdpid == 0) {
				restart_now = true;
			}
		}

		if (fds[1].revents & POLLIN)
			wl_display_dispatch(display);
		if (fds[1].revents & POLLERR) {
			fprintf(stderr, "wayland socket disconnect\n");
			exit_code = EXIT_FAILURE;
			goto err;
		}
		if (fds[1].revents & POLLHUP) {
			fprintf(stderr, "wayland socket disconnect\n");
			exit_code = EXIT_FAILURE;
			goto err;
		}
	}

err:
	if (layer_shell != NULL)
	    zwlr_layer_shell_v1_destroy(layer_shell);
	if (shm != NULL)
	    wl_shm_destroy(shm);
	if (compositor != NULL)
	    wl_compositor_destroy(compositor);
	if (registry != NULL)
	    wl_registry_destroy(registry);
	if (display != NULL)
	    wl_display_disconnect(display);
	if (font != NULL)
		fcft_destroy(font);
	wl_display_disconnect(display);

	return exit_code;
}

