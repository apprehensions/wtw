#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/signalfd.h>
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
#include "xdg-shell-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#define INITIAL_CAPACITY 2

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
static int32_t width = 0, height = 0, x = 0, y = 0;

static char **cmd;
static pid_t cmdpid;
static FILE *inputf;
static char *text;
static int signal_fd = -1;
static size_t len;
static size_t cap;


static bool restart = false;
static bool running = false;

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

static int
start_cmd(void)
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
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		setpgid(0, 0);
		execvp(cmd[0], cmd);
		exit(EXIT_FAILURE);
	default:
		break;
	}

	close(fds[1]);
	return 0;
}

static int
reap(void)
{
	for (;;) {
		int wstatus;
		pid_t p = waitpid(-1, &wstatus, cmdpid == 0 ? WNOHANG : 0);
		if (p == -1) {
			if (cmdpid == 0 && errno == ECHILD) {
				errno = 0;
				break;
			}
			perror("waitpid:");
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
read_text(void)
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
render(void)
{
	uint32_t stride = width * 4;
	uint32_t size = stride * height;
	int fd;
	uint32_t *data;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	pixman_image_t *pix;
	pixman_region32_t clip;
	int ty = y;

	if (width < 0 || height < 0)
		return;

	fd = memfd_create("wtw-wayland-shm-buffer-pool",
		MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd == -1)
		return;

	if ((ftruncate(fd, size)) == -1) {
		close(fd);
		return;
	}

	data = mmap(NULL, size,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return;
	}

	pool = wl_shm_create_pool(shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);

	pix = pixman_image_create_bits_no_clear(
		PIXMAN_a8r8g8b8, width, height, data, stride);
	pixman_region32_init_rect(&clip, 0, 0, width, height);
	pixman_image_set_clip_region32(pix, &clip);
	pixman_region32_fini(&clip);

	for (char *line = text; line < text + len; line += strlen(line) + 1) {
		drwl_text(pix, font, x, ty, width, font->height, 0, line, &colors[0], &colors[1]);
		ty += font->height;
	}

	pixman_image_unref(pix);
	munmap(data, size);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_buffer_destroy(buffer);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t w, uint32_t h)
{
	width = w;
	height = h;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
	running = false;
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
		"usage: %s [-b rrggbbaa] [-c rrggbbaa] [-f font] [-p period]\n"
		"           [-w pos] [-h pos] [-x pos] [-y pos] command [arg ...]\n", argv0);
	exit(EXIT_SUCCESS);
}

static int
setup(void)
{
	sigset_t mask;

	if (!(display = wl_display_connect(NULL))) {
		fprintf(stderr, "could not connect to display\n");
		return -1;
	}

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !layer_shell) {
		fputs("unsupported compositor", stderr);
		return -1;
	}

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGALRM);
	sigaddset(&mask, SIGCHLD);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("sigprocmask");
		return -1;
	}

	if ((signal_fd = signalfd(-1, &mask, SFD_NONBLOCK)) < 0) {
		perror("signalfd");
		return -1;
	}

	fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_WARNING);
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);
	if (!(font = fcft_from_name(1, &font_name, NULL))) {
		fprintf(stderr, "bad font\n");
		return -1;
	}

	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface,
		NULL, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "wtw");

	zwlr_layer_surface_v1_add_listener(
		layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	wl_surface_commit(surface);

	return 0;
}

static int
run(void)
{
	struct signalfd_siginfo si;
	struct pollfd fds[3] = {
		{ .fd = wl_display_get_fd(display), .events = POLLIN },
		{ .fd = signal_fd,                  .events = POLLIN },
		{ .fd = -1,                         .events = POLLIN },
	};
	
	restart = running = true;
	while (running) {
		if (wl_display_prepare_read(display) < 0) {
			if (wl_display_dispatch_pending(display) < 0) {
				perror("wl_display_dispatch_pending:");
				break;
			}
		}

		wl_display_flush(display);

		if (restart && cmdpid == 0 && inputf == NULL) {
			restart = false;
			start_cmd();
		}

		fds[2].fd = inputf ? fileno(inputf) : -1;

		if (poll(fds, 3, -1) < 0) {
			perror("poll:");
			return EXIT_FAILURE;
		}
		
		if (fds[1].revents & POLLIN) {
			ssize_t n = read(signal_fd, &si, sizeof(si));
			if (n != sizeof(si))
				perror("signalfd");
			if (si.ssi_signo == SIGCHLD) {
				if (reap() < 0)
					return EXIT_FAILURE;
				if (period < 0)
					restart = true;
				else if (!restart)
					alarm(period);
			} else if (si.ssi_signo == SIGALRM && cmdpid == 0)
				restart = true;
			else if (si.ssi_signo == SIGINT)
				return EXIT_FAILURE;
		}

		if (inputf && fds[2].revents & POLLIN) {
			if (read_text() < 0)
				return EXIT_FAILURE;
			render();
		}

		if (!(fds[0].revents & POLLIN)) {
			wl_display_cancel_read(display);
			continue;
		}

		if (wl_display_read_events(display) < 0) {
			perror("wl_display_read_events");
			return EXIT_FAILURE;
		}

		if (wl_display_dispatch_pending(display) < 0) {
			perror("wl_display_dispatch_pending");
			return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}

static void
cleanup(void)
{
	if (!display)
		return;
	if (signal_fd > 0)
		close(signal_fd);
	if (font)
		fcft_destroy(font);
	if (layer_surface)
		zwlr_layer_surface_v1_destroy(layer_surface);
	if (layer_shell)
		zwlr_layer_shell_v1_destroy(layer_shell);
	if (surface)
		wl_surface_destroy(surface);
	if (compositor)
		wl_compositor_destroy(compositor);
	if (shm)
		wl_shm_destroy(shm);
	fcft_fini();
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
}

int
main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;

	ARGBEGIN {
	case '?': usage();
	case 'b': colors[1] = parse_color(EARGF(usage())); break;
	case 'c': colors[0] = parse_color(EARGF(usage())); break;
	case 'f': font_name = EARGF(usage()); break;
	case 'p': period = atoi(EARGF(usage())); break;
	case 'w': width = atoi(EARGF(usage())); break;
	case 'h': height = atoi(EARGF(usage())); break;
	case 'x': x = atoi(EARGF(usage())); break;
	case 'y': y = atoi(EARGF(usage())); break;
	default:
		warn("bad option: -%c", ARGC());
		usage();
	} ARGEND

	if (argc == 0)
		usage();

	cmd = argv;

	if (setup() < 0)
		goto err;

	ret = run();
err:
	cleanup();
	return ret;
}
