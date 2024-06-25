/* See LICENSE file for copyright and license details. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

#include "drwl.h"
#include "poolbuf.h"
#include "xdg-shell-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#define MAX(A, B)  ((A) > (B) ? (A) : (B))
#define MIN(A, B)  ((A) < (B) ? (A) : (B))

#define INITIAL_CAPACITY 2

static const char usage[] =
	"usage: wtw [-b rrggbbaa] [-c rrggbbaa] [-f font] [-p period] [-P padding]\n"
	"           [-w pos] [-h pos] [-x pos] [-y pos] command [arg ...]\n";

#include "config.h"

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_shm *shm;
static struct wl_compositor *compositor;
static struct wl_surface *surface;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static Drwl *drw;

static char **cmd;
static pid_t cmdpid;
static FILE *inputf;
static char *text;
static int signal_fd = -1;
static size_t len;
static size_t cap;

static bool restart = false;
static bool running = false;

static int
start_cmd(void)
{
	int fds[2];
	if (pipe(fds) == -1) {
		perror("pipe");
		return -1;
	}

	if (!(inputf = fdopen(fds[0], "r"))) {
		perror("fdopen");
		return -1;
	}

	cmdpid = fork();
	switch (cmdpid) {
	case -1:
		perror("fork");
		return 1;
	case 0:
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		setpgid(0, 0);
		execvp(cmd[0], cmd);
		perror("execvp");
		exit(EXIT_FAILURE);
	default:
		break;
	}

	close(fds[1]);
	return 0;
}

static void
reap(void)
{
	pid_t p;
	int status;

	do {
		if ((p = waitpid(-1, &status, cmdpid == 0 ? WNOHANG : 0)) < 0) {
			perror("waitpid");
			return;
		}
		if (p == cmdpid) {
			cmdpid = 0;
			return;
		}
	} while (!WIFEXITED(status) && !WIFSIGNALED(status));
}

static int
read_text(void)
{
	char *line;
	int llen, dlen = strlen(delimeter);

	len = 0;
	for (;;) {
		if (len + dlen + 2 > cap) {
			/* 
			 * Buffer must have sufficient capacity to
			 * store delimeter string, \n and \0 in one read.
			 */
			cap = cap ? cap * 2 : INITIAL_CAPACITY;
			if (!(text = realloc(text, cap))) {
				perror("realloc");
				return -1;
			}
		}

		line = &text[len];
		if (fgets(line, cap - len, inputf) == NULL) {
			if (!feof(inputf)) {
				perror("fgets");
				return -1;
			}

			if (fclose(inputf) == -1) {
				perror("fclose");
				return -1;
			}
			inputf = NULL;
			break;
		}

		llen = strlen(line);

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
render(void)
{
	int ty = y + pad;
	uint32_t tw = 0, w = 0, h = 0;
	char *line;
	PoolBuf *buf;

	if (width < 0 || height < 0)
		return;

	/* Use maximum text line width and height */
	for (line = text; line < text + len; line += strlen(line) + 1) {
		tw = drwl_font_getwidth(drw, line);
		w = MAX(w, tw);
		h += drw->font->height;
	}

	w = MIN(w + pad * 2 + x, width);
	h = MIN(h + pad * 2 + y, height);

	if (!(buf = poolbuf_create(shm, w, h))) {
		fputs("failed to create draw buffer\n", stderr);
		return;
	}

	drwl_prepare_drawing(drw, w, h, buf->data, buf->stride);

	drwl_rect(drw, x, y, w, h, 1, 1);

	for (line = text; line < text + len; line += strlen(line) + 1) {
		drwl_text(drw, x + pad, ty, w - pad * 2, drw->font->height, 0, line, 0);
		ty += drw->font->height;
	}

	drwl_finish_drawing(drw);

	wl_surface_attach(surface, buf->wl_buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, w, h);
	poolbuf_destroy(buf);
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
		fputs("unsupported compositor\n", stderr);
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

	drwl_init();
	if (!(drw = drwl_create())) {
		fputs("failed to create drwl context\n", stderr);
		return -1;
	}
	if (!(drwl_load_font(drw, 1, &font_name, NULL)))
		return -1;
	drwl_setscheme(drw, scheme);

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
				perror("wl_display_dispatch_pending");
				break;
			}
		}

		wl_display_flush(display);

		if (restart && cmdpid == 0 && !inputf) {
			restart = false;
			start_cmd();
		}

		fds[2].fd = inputf ? fileno(inputf) : -1;

		if (poll(fds, 3, -1) < 0) {
			perror("poll");
			return EXIT_FAILURE;
		}

		if (fds[1].revents & POLLIN) {
			ssize_t n = read(signal_fd, &si, sizeof(si));
			if (n != sizeof(si))
				perror("signalfd");
			if (si.ssi_signo == SIGCHLD) {
				reap();
				if (period < 0)
					restart = true;
				else if (!restart)
					alarm(period);
			} else if (si.ssi_signo == SIGALRM && cmdpid == 0)
				restart = true;
			else
				return EXIT_FAILURE;
		}

		/* Command error */
		if (fds[2].revents & POLLHUP) {
			inputf = NULL;
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
	if (drw)
		drwl_destroy(drw);
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
	drwl_fini();
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
}

int
main(int argc, char *argv[])
{
	int opt;
	int ret = EXIT_FAILURE;

	while ((opt = getopt(argc, argv, "b:c:f:p:P:w:h:x:y:")) != -1) {
		switch (opt) {
		case 'b':
		case 'c':
			scheme[opt == 'b' ? ColBg : ColFg] = strtoul(optarg, NULL, 16);
			break;
		case 'f': font_name = optarg; break;
		case 'p': period = atoi(optarg); break;
		case 'P': pad = atoi(optarg); break;
		case 'w': width = atoi(optarg); break;
		case 'h': height = atoi(optarg); break;
		case 'x': x = atoi(optarg); break;
		case 'y': y = atoi(optarg); break;
		default:
			fprintf(stderr, usage);
			return ret;
		}
	}
	argv += optind;
	argc -= optind;
	if (argc < 1) {
		fprintf(stderr, usage);
		return ret;
	}

	cmd = argv;

	if (setup() < 0)
		goto err;

	ret = run();
err:
	cleanup();
	return ret;
}
