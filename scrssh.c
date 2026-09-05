/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Enno Boland <g@s01.de>
 */

#define _POSIX_C_SOURCE 200809L

#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MARKER(s) "\xff\xfe" s "\xfe\xff\n"

static const char AGENT[] = {
#include "agent.h"
};

struct {
	char title[256];
	char escalation_method;
	struct termios tio;
	pid_t child;
	int input_fd;
	int video_fd;
	SDL_Thread *decoder;
	SDL_Mutex *locks[2];
	AVFrame *frames[2];
	SDL_AtomicU32 ready;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	bool fullscreen;
} app = {0};

static void
die(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
	exit(1);
}

static void
ssh_spawn(char **argv, size_t argc) {
	const char *post_argv[] = {
			"printf '" MARKER("login") "';", "exec", "python3 -u -c", AGENT,
			NULL};
	switch (app.escalation_method) {
	case 's':
		post_argv[1] = "exec sudo -S";
		break;
	case 'a':
		post_argv[1] = "exec doas -n";
		break;
	}

	char **exec_argv =
			calloc(argc + 1 + SDL_arraysize(post_argv), sizeof(char *));
	if (!exec_argv) {
		die("out of memory");
	}
	exec_argv[0] = "ssh";
	memcpy(&exec_argv[1], argv, argc * sizeof(char *));
	memcpy(&exec_argv[argc + 1], post_argv, sizeof(post_argv));

	int pin[2], pout[2];
	if (pipe(pin) < 0 || pipe(pout) < 0) {
		die("could not create a pipe: %s", strerror(errno));
	}

	pid_t pid = fork();
	if (pid < 0) {
		die("could not fork: %s", strerror(errno));
	} else if (pid == 0) {
		dup2(pin[0], STDIN_FILENO);
		dup2(pout[1], STDOUT_FILENO);
		for (size_t i = 0; i < 2; i++) {
			close(pin[i]);
			close(pout[i]);
		}
		execvp("ssh", exec_argv);
		fprintf(stderr,
				"no `ssh` on PATH; scrssh uses the system OpenSSH client\n");
		_exit(127);
	}

	free(exec_argv);
	close(pin[0]);
	close(pout[1]);
	app.input_fd = pin[1];
	app.video_fd = pout[0];
	app.child = pid;
}

enum CONFIG {
	CONFIG_DEVICE,
	CONFIG_CRTC,
	CONFIG_PLANE,
	CONFIG_FPS,
	CONFIG_BITRATE,
	CONFIG_ENCODER,
	CONFIG_COUNT,
};

static void
put(int fd, const void *data, size_t size) {
	if (write(fd, data, size) != (ssize_t)size) {
		die("could not send to the remote host: %s", strerror(errno));
	}
}

static void
echo_restore(void) {
	tcsetattr(STDIN_FILENO, TCSANOW, &app.tio);
}

static void
interrupted(int signal_number) {
	echo_restore();
	_exit(128 + signal_number);
}

static void
echo_off(void) {
	struct termios no_echo;
	if (tcgetattr(STDIN_FILENO, &app.tio) < 0) {
		return;
	}
	no_echo = app.tio;
	no_echo.c_lflag &= ~ECHO;
	no_echo.c_lflag |= ECHONL;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &no_echo) < 0) {
		return;
	}
	atexit(echo_restore);
	signal(SIGINT, interrupted);
	signal(SIGTERM, interrupted);
	signal(SIGHUP, interrupted);
}

static void
greeting(const char *marker, int in) {
	char buffer[256];
	size_t matched = 0;

	while (marker[matched]) {
		struct pollfd fds[] = {
				{app.video_fd, POLLIN, 0},
				{in, POLLIN, 0},
		};
		if (poll(fds, SDL_arraysize(fds), -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			die("could not wait for the remote agent: %s", strerror(errno));
		}

		if (fds[1].revents) {
			ssize_t n = read(in, buffer, sizeof(buffer));
			if (n > 0) {
				put(app.input_fd, buffer, n);
			} else if (n == 0 || errno != EINTR) {
				in = -1;
			}
		}

		if (fds[0].revents) {
			char byte = '\0';
			ssize_t n = read(app.video_fd, &byte, 1);
			if (n == 0) {
				die("the remote agent did not start");
			} else if (n < 0 && errno != EINTR) {
				die("could not read from the remote host: %s", strerror(errno));
			} else if (byte == marker[matched]) {
				matched++;
			} else {
				matched = byte == marker[0];
			}
		}
	}
}

static void
send_config(int fd, const char **config) {
	size_t length = 0;
	for (size_t i = 0; i < CONFIG_COUNT; i++) {
		length += strlen(config[i]) + 1;
	}
	if (length > UINT16_MAX) {
		die("the capture configuration is too long");
	}

	uint16_t header = htons(length);
	put(fd, &header, sizeof(header));
	for (size_t i = 0; i < CONFIG_COUNT; i++) {
		put(fd, config[i], strlen(config[i]) + 1);
	}
}

static unsigned int
publish(AVFrame *decoded, unsigned int back_idx) {
	unsigned int front_idx = !back_idx;

	av_frame_unref(app.frames[back_idx]);
	av_frame_move_ref(app.frames[back_idx], decoded);

	SDL_LockMutex(app.locks[front_idx]);
	SDL_AddAtomicU32(&app.ready, 1);
	SDL_UnlockMutex(app.locks[back_idx]);

	SDL_PushEvent(&(SDL_Event){.type = SDL_EVENT_USER});

	return front_idx;
}

static const char *
decode_packet(
		AVCodecContext *decoder, int stream, AVPacket *packet, AVFrame *decoded,
		unsigned int *back_idx) {
	if (packet->stream_index != stream ||
		avcodec_send_packet(decoder, packet) < 0) {
		return NULL;
	}

	while (avcodec_receive_frame(decoder, decoded) >= 0) {
		if (decoded->format != AV_PIX_FMT_YUV420P) {
			return "the remote sent a pixel format scrssh cannot show; "
				   "kmsgrab and VAAPI produce 8-bit 4:2:0";
		}
		*back_idx = publish(decoded, *back_idx);
	}

	return NULL;
}

static int
run_decode(void *arg) {
	(void)arg;

	char url[32];
	snprintf(url, sizeof(url), "pipe:%d", app.video_fd);

	const char *error = NULL;
	AVFormatContext *format = NULL;
	AVCodecContext *decoder = NULL;
	AVPacket *packet = NULL;
	AVFrame *decoded = NULL;

	unsigned int back_idx = !(SDL_GetAtomicU32(&app.ready) % 2);
	SDL_LockMutex(app.locks[back_idx]);

	format = avformat_alloc_context();
	if (!format) {
		error = "could not set up the demuxer";
		goto done;
	}
	format->probesize = 32 * 1024;
	format->max_analyze_duration = 100000;
	format->fps_probe_size = 0;

	if (avformat_open_input(
				&format, url, av_find_input_format("mpegts"), NULL) < 0) {
		error = "could not open the remote video stream";
		goto done;
	}
	const AVCodec *codec = NULL;
	int stream = -1;
	if (avformat_find_stream_info(format, NULL) < 0 ||
		(stream = av_find_best_stream(
				 format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0) {
		error = "the remote stream contains no video";
		goto done;
	}

	packet = av_packet_alloc();
	decoded = av_frame_alloc();
	if (!packet || !decoded || !(decoder = avcodec_alloc_context3(codec)) ||
		avcodec_parameters_to_context(
				decoder, format->streams[stream]->codecpar) < 0) {
		error = "could not open the H.264 decoder";
		goto done;
	}
	decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (avcodec_open2(decoder, codec, NULL) < 0) {
		error = "could not open the H.264 decoder";
		goto done;
	}

	while (av_read_frame(format, packet) >= 0) {
		error = decode_packet(decoder, stream, packet, decoded, &back_idx);
		av_packet_unref(packet);
		if (error) {
			goto done;
		}
	}

	error = "the remote video stream ended";

done:
	SDL_UnlockMutex(app.locks[back_idx]);
	av_frame_free(&decoded);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&format);
	if (error) {
		SDL_PushEvent(&(SDL_Event){
				.user.type = SDL_EVENT_USER, .user.data1 = (void *)error});
	}
	return 0;
}

#define ABS_RANGE_MAX 65535

struct ev {
	uint16_t type;
	uint16_t code;
	int32_t value;
} __attribute__((packed));

_Static_assert(sizeof(struct ev) == 8, "the wire record must not be padded");

static void
send_input(struct ev *ev, size_t ev_count) {
	ev[ev_count - 1].type = EV_SYN;
	for (size_t i = 0; i < ev_count; i++) {
		ev[i].type = htons(ev[i].type);
		ev[i].code = htons(ev[i].code);
		ev[i].value = htonl(ev[i].value);
	}

	write(app.input_fd, ev, ev_count * sizeof(*ev));
}

static uint16_t
to_scancode(SDL_Scancode scancode) {
	switch (scancode) {
#define DEF(sdl, ev) \
	case SDL_SCANCODE_##sdl: \
		return KEY_##ev;
#include "scancodes.h"
#undef DEF
	default:
		return 0;
	}
}

static uint16_t
to_button(Uint8 button) {
	switch (button) {
#define DEF(b) \
	case SDL_BUTTON_##b: \
		return BTN_##b;
		DEF(LEFT)
		DEF(MIDDLE)
		DEF(RIGHT)
#undef DEF
	default:
		return 0;
	}
}

static int32_t
to_abs(float value, int extent) {
	long offset = value;
	if (extent <= 0 || offset <= 0) {
		return 0;
	}
	if (offset >= extent) {
		return ABS_RANGE_MAX;
	}
	return (int32_t)((offset * ABS_RANGE_MAX + extent / 2) / extent);
}

static void
create_window(int width, int height) {
	SDL_Rect bounds = {0, 0, 0, 0};
	SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds);

	long max_w = bounds.w * 9L / 10, max_h = bounds.h * 9L / 10;
	long window_w = width, window_h = height;
	if (width > 0 && height > 0 && max_w > 0 && max_h > 0 &&
		(width > max_w || height > max_h)) {
		long w = width, h = height;
		if (w * max_h > h * max_w) {
			h = h * max_w / w;
			w = max_w;
		} else {
			w = w * max_h / h;
			h = max_h;
		}
		if (w > 0 && h > 0) {
			window_w = w;
			window_h = h;
		}
	}

	SDL_Window *window;
	SDL_WindowFlags flags =
			SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (app.fullscreen) {
		flags |= SDL_WINDOW_FULLSCREEN;
	}
	if (!SDL_CreateWindowAndRenderer(
				app.title, (int)window_w, (int)window_h, flags, &window,
				&app.renderer)) {
		die("could not create the window: %s", SDL_GetError());
	}

	app.texture = SDL_CreateTexture(
			app.renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
			width, height);
	if (!app.texture) {
		die("could not create the video texture: %s", SDL_GetError());
	}
	SDL_SetTextureScaleMode(app.texture, SDL_SCALEMODE_LINEAR);
	SDL_SetRenderLogicalPresentation(
			app.renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_SetWindowAspectRatio(
			window, (float)width / height, (float)width / height);
}

static void
redraw(void) {
	if (SDL_HasEvent(SDL_EVENT_USER) ||
		SDL_HasEvent(SDL_EVENT_WINDOW_RESIZED) ||
		SDL_HasEvent(SDL_EVENT_QUIT)) {
		return;
	}
	SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
	SDL_RenderClear(app.renderer);
	SDL_RenderTexture(app.renderer, app.texture, NULL, NULL);
	SDL_RenderPresent(app.renderer);
}

static const char *
run_ui(void) {
	const char *error = NULL;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		die("could not initialise SDL: %s", SDL_GetError());
	}

	app.frames[0] = av_frame_alloc();
	app.frames[1] = av_frame_alloc();
	app.locks[0] = SDL_CreateMutex();
	app.locks[1] = SDL_CreateMutex();
	if (!app.frames[0] || !app.frames[1] || !app.locks[0] || !app.locks[1]) {
		die("out of memory");
	}

	app.decoder = SDL_CreateThread(run_decode, "decoder", NULL);
	if (!app.decoder) {
		die("could not start the decoder thread: %s", SDL_GetError());
	}

	SDL_Event event;
	while (SDL_WaitEvent(&event)) {
		if (app.renderer) {
			SDL_ConvertEventToRenderCoordinates(app.renderer, &event);
		}
		switch (event.type) {
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			goto out;
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
			redraw();
		} break;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
			if (event.key.repeat) {
				break;
			}
			struct ev key[2] = {
					{EV_KEY, to_scancode(event.key.scancode),
					 event.type == SDL_EVENT_KEY_DOWN}};
			send_input(key, SDL_arraysize(key));
		} break;
		case SDL_EVENT_MOUSE_MOTION: {
			int w = 0, h = 0;
			SDL_RendererLogicalPresentation mode;
			SDL_GetRenderLogicalPresentation(app.renderer, &w, &h, &mode);
			struct ev move[3] = {
					{EV_ABS, ABS_X, to_abs(event.motion.x, w)},
					{EV_ABS, ABS_Y, to_abs(event.motion.y, h)},
			};
			send_input(move, SDL_arraysize(move));
		} break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			uint16_t code = to_button(event.button.button);
			if (!code) {
				break;
			}
			struct ev click[2] = {
					{EV_KEY, code, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN}};
			send_input(click, SDL_arraysize(click));
		} break;
		case SDL_EVENT_MOUSE_WHEEL: {
			struct ev wheel[3] = {
					{EV_REL, REL_WHEEL, event.wheel.integer_y},
					{EV_REL, REL_HWHEEL, event.wheel.integer_x},
			};
			send_input(wheel, SDL_arraysize(wheel));
		} break;
		case SDL_EVENT_USER: {
			if (event.user.data1) {
				error = event.user.data1;
				goto out;
			} else if (SDL_HasEvent(SDL_EVENT_USER)) {
				break;
			}

			unsigned int front_idx = SDL_GetAtomicU32(&app.ready) % 2;
			SDL_LockMutex(app.locks[front_idx]);
			AVFrame *frame = app.frames[front_idx];
			if (!app.renderer) {
				create_window(frame->width, frame->height);
			}
			SDL_UpdateYUVTexture(
					app.texture, NULL, frame->data[0], frame->linesize[0],
					frame->data[1], frame->linesize[1], frame->data[2],
					frame->linesize[2]);
			SDL_UnlockMutex(app.locks[front_idx]);
			redraw();
		} break;
		}
	}

out:
	kill(app.child, SIGTERM);
	SDL_WaitThread(app.decoder, NULL);

	av_frame_free(&app.frames[0]);
	av_frame_free(&app.frames[1]);
	SDL_DestroyMutex(app.locks[0]);
	SDL_DestroyMutex(app.locks[1]);
	SDL_DestroyTexture(app.texture);
	if (app.renderer) {
		SDL_Window *window = SDL_GetRenderWindow(app.renderer);
		SDL_DestroyRenderer(app.renderer);
		SDL_DestroyWindow(window);
	}
	SDL_Quit();

	return error;
}

static void
set_title(char **argv, size_t argc) {
	app.title[0] = 0;
	for (size_t i = 0; i < argc &&
		 sizeof(app.title) > strlen(argv[i]) + strlen(app.title) + 1;
		 i++) {
		if (i) {
			strcat(app.title, " ");
		}
		strcat(app.title, argv[i]);
	}
}

static void
usage(void) {
	die("usage: scrssh [options] [--] <ssh arguments...>\n"
		"https://codeberg.org/Gottox/scrssh\n"
		"\n"
		"options:\n"
		"  -s         run the agent under `sudo -S`\n"
		"  -a         run the agent under `doas -n`\n"
		"  -d <PATH>  DRM device to capture      [default: "
		"/dev/dri/card0]\n"
		"  -C <N>     capture a specific CRTC\n"
		"  -P <N>     capture a specific plane\n"
		"  -f <N>     capture frame rate         [default: 30]\n"
		"  -B <RATE>  capped bitrate             [default: 500K]\n"
		"  -e <NAME>  force an encoder           [default: ask the host]\n"
		"             h264_vaapi, h264_nvenc, h264_v4l2m2m, libx264\n"
		"  -h         show this help");
}

int
main(int argc, char **argv) {
	const char *config[] = {"/dev/dri/card0", "", "", "30", "500K", ""};

	for (int o; (o = getopt(argc, argv, "+ad:C:P:f:FB:e:sh")) != -1;) {
		switch (o) {
#define CFG(c, v) \
	case c: \
		config[v] = optarg; \
		break;
			CFG('d', CONFIG_DEVICE)
			CFG('C', CONFIG_CRTC)
			CFG('P', CONFIG_PLANE)
			CFG('f', CONFIG_FPS)
			CFG('B', CONFIG_BITRATE)
			CFG('e', CONFIG_ENCODER)
#undef CFG
		case 'a':
		case 's':
			app.escalation_method = o;
			break;
		case 'F':
			app.fullscreen = true;
			break;
		default:
			usage();
		}
	}
	if (optind == argc) {
		usage();
	}

	signal(SIGPIPE, SIG_IGN);
	av_log_set_level(AV_LOG_QUIET);

	ssh_spawn(argv + optind, argc - optind);
	set_title(argv + optind, argc - optind);

	greeting(MARKER("login"), -1);
	echo_off();
	greeting(MARKER("agent"), STDIN_FILENO);

	send_config(app.input_fd, config);
	fcntl(app.input_fd, F_SETFL, O_NONBLOCK);

	const char *error = run_ui();
	waitpid(app.child, NULL, 0);

	if (error) {
		die("%s", error);
	}
	return 0;
}
