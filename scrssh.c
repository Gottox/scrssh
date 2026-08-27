#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LENGTH(a) (sizeof(a) / sizeof *(a))

static const char AGENT[] = {
#include "agent.h"
};

static void
die(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
	exit(1);
}

struct app {
	pid_t child;
	int input_fd;
	int video_fd;
	Uint32 wake;
	pthread_t decoder;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	AVFrame *frame;
	const char *error;
};

static void
ssh_spawn(struct app *app, char **args, int count) {
	char **argv = calloc((size_t)count + 6, sizeof *argv);
	if (!argv) {
		die("out of memory");
	}
	argv[0] = (char *)"ssh";
	for (int i = 0; i < count; i++) {
		argv[i + 1] = args[i];
	}
	argv[count + 1] = (char *)"python3";
	argv[count + 2] = (char *)"-u";
	argv[count + 3] = (char *)"-c";
	argv[count + 4] = (char *)AGENT;
	argv[count + 5] = NULL;

	int pin[2], pout[2];
	if (pipe(pin) < 0 || pipe(pout) < 0) {
		die("could not create a pipe: %s", strerror(errno));
	}

	pid_t pid = fork();
	if (pid < 0) {
		die("could not fork: %s", strerror(errno));
	}
	if (pid == 0) {
		dup2(pin[0], STDIN_FILENO);
		dup2(pout[1], STDOUT_FILENO);
		for (int i = 0; i < 2; i++) {
			close(pin[i]);
			close(pout[i]);
		}
		execvp("ssh", argv);
		fprintf(stderr,
				"no `ssh` on PATH; scrssh uses the system OpenSSH client\n");
		_exit(127);
	}

	free(argv);
	close(pin[0]);
	close(pout[1]);
	app->input_fd = pin[1];
	app->video_fd = pout[0];
	app->child = pid;
}

struct config {
	const char *device, *crtc, *plane, *fps, *bitrate, *encoder;
};

static void
put(int fd, const void *data, size_t size) {
	if (write(fd, data, size) != (ssize_t)size) {
		die("could not send the capture configuration: %s", strerror(errno));
	}
}

/* The agent turns this into an ffmpeg command line, so that it can pick an
   encoder that the host actually has. An empty field means "unset". */
static void
send_command(int fd, const struct config *c) {
	const char *fields[] = {c->device, c->crtc,    c->plane,
							c->fps,    c->bitrate, c->encoder};

	size_t length = LENGTH(fields) - 1;
	for (unsigned i = 0; i < LENGTH(fields); i++) {
		length += strlen(fields[i]);
	}
	if (length > UINT16_MAX) {
		die("the capture configuration is too long");
	}

	uint16_t header = htons((uint16_t)length);
	put(fd, &header, sizeof header);
	for (unsigned i = 0; i < LENGTH(fields); i++) {
		if (i) {
			put(fd, "\0", 1);
		}
		put(fd, fields[i], strlen(fields[i]));
	}
}

#define AVIO_BUFFER 65536

static int
read_stream(void *opaque, uint8_t *buffer, int size) {
	for (;;) {
		ssize_t got = read((int)(intptr_t)opaque, buffer, (size_t)size);
		if (got > 0) {
			return (int)got;
		}
		if (got < 0 && errno == EINTR) {
			continue;
		}
		return AVERROR_EOF;
	}
}

static void
decoder_failed(const struct app *app, const char *message) {
	SDL_Event event = {0};
	event.type = app->wake;
	event.user.data2 = (void *)(uintptr_t)message;
	SDL_PushEvent(&event);
}

static void
publish(const struct app *app, AVFrame *decoded) {
	AVFrame *sent = av_frame_alloc();
	if (!sent) {
		return;
	}
	av_frame_move_ref(sent, decoded);

	SDL_Event event = {0};
	event.type = app->wake;
	event.user.data1 = sent;
	if (!SDL_PushEvent(&event)) {
		av_frame_free(&sent);
	}
}

static void *
decode(void *arg) {
	const struct app *app = arg;

	uint8_t *buffer = av_malloc(AVIO_BUFFER);
	AVFormatContext *format = avformat_alloc_context();
	AVIOContext *avio = buffer && format
			? avio_alloc_context(
					  buffer, AVIO_BUFFER, 0, (void *)(intptr_t)app->video_fd,
					  read_stream, NULL, NULL)
			: NULL;
	if (!avio) {
		decoder_failed(app, "could not set up the demuxer");
		return NULL;
	}

	format->pb = avio;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;

	format->probesize = 32 * 1024;
	format->max_analyze_duration = 100000;

	AVCodecContext *decoder = NULL;
	AVPacket *packet = NULL;
	AVFrame *decoded = NULL;

	if (avformat_open_input(
				&format, NULL, av_find_input_format("mpegts"), NULL) < 0) {
		format = NULL;
		decoder_failed(app, "could not open the remote video stream");
		goto done;
	}
	const AVCodec *codec = NULL;
	int stream = -1;
	if (avformat_find_stream_info(format, NULL) < 0 ||
		(stream = av_find_best_stream(
				 format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0) {
		decoder_failed(app, "the remote stream contains no video");
		goto done;
	}

	packet = av_packet_alloc();
	decoded = av_frame_alloc();
	if (!packet || !decoded || !(decoder = avcodec_alloc_context3(codec)) ||
		avcodec_parameters_to_context(
				decoder, format->streams[stream]->codecpar) < 0) {
		decoder_failed(app, "could not open the H.264 decoder");
		goto done;
	}
	decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (avcodec_open2(decoder, codec, NULL) < 0) {
		decoder_failed(app, "could not open the H.264 decoder");
		goto done;
	}

	while (av_read_frame(format, packet) >= 0) {
		if (packet->stream_index == stream &&
			avcodec_send_packet(decoder, packet) >= 0) {
			while (avcodec_receive_frame(decoder, decoded) >= 0) {
				if (decoded->format != AV_PIX_FMT_YUV420P) {
					decoder_failed(
							app,
							"the remote sent a pixel format scrssh cannot "
							"show; kmsgrab and VAAPI produce 8-bit 4:2:0");
					av_packet_unref(packet);
					goto done;
				}
				publish(app, decoded);
			}
		}
		av_packet_unref(packet);
	}

	decoder_failed(app, "the remote video stream ended");

done:
	av_frame_free(&decoded);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&format);

	av_free(avio->buffer);
	avio_context_free(&avio);
	return NULL;
}

enum { DEV_KEYBOARD, DEV_POINTER };

#define ABS_RANGE_MAX 65535

struct wire {
	uint8_t device;
	uint8_t pad;
	uint16_t type;
	uint16_t code;
	int32_t value;
} __attribute__((packed));

_Static_assert(sizeof(struct wire) == 10, "the wire record must not be padded");

static void
send_input(struct app *app, struct wire *events, int n) {
	if (app->input_fd < 0) {
		return;
	}
	struct pollfd pfd = {.fd = app->input_fd, .events = POLLOUT};
	if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLOUT)) {
		return;
	}

	events[n] = (struct wire){.device = events[n - 1].device, .type = EV_SYN};
	for (int i = 0; i <= n; i++) {
		events[i].type = htons(events[i].type);
		events[i].code = htons(events[i].code);
		events[i].value = (int32_t)htonl((uint32_t)events[i].value);
	}

	size_t length = (size_t)(n + 1) * sizeof *events;
	if (write(app->input_fd, events, length) < 0) {
		app->input_fd = -1;
	}
}

static uint16_t
to_evdev(SDL_Scancode scancode) {
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
#define DEF(sdl, ev) \
	case SDL_BUTTON_##sdl: \
		return BTN_##ev;
#include "buttons.h"
#undef DEF
	default:
		return 0;
	}
}

static SDL_Rect
fit(int window_w, int window_h, int video_w, int video_h) {
	if (video_w <= 0 || video_h <= 0 || window_w <= 0 || window_h <= 0) {
		return (SDL_Rect){0, 0, 0, 0};
	}

	int w = window_w, h = window_h;
	if ((long)video_w * window_h > (long)video_h * window_w) {
		h = (int)((long)video_h * window_w / video_w);
	} else {
		w = (int)((long)video_w * window_h / video_h);
	}
	return (SDL_Rect){(window_w - w) / 2, (window_h - h) / 2, w, h};
}

static int32_t
to_abs(int value, int origin, int extent) {
	long offset = value - origin;
	if (extent <= 0 || offset <= 0) {
		return 0;
	}
	if (offset >= extent) {
		return ABS_RANGE_MAX;
	}
	return (int32_t)((offset * ABS_RANGE_MAX + extent / 2) / extent);
}

/* Where the video sits in the window right now. Both the renderer output and
   the texture know their own size, so nothing has to be remembered. */
static SDL_Rect
layout(const struct app *app) {
	int output_w = 0, output_h = 0;
	float video_w = 0, video_h = 0;
	SDL_GetCurrentRenderOutputSize(app->renderer, &output_w, &output_h);
	if (app->texture) {
		SDL_GetTextureSize(app->texture, &video_w, &video_h);
	}
	return fit(output_w, output_h, (int)video_w, (int)video_h);
}

static void
send_pointer(struct app *app, int x, int y) {
	SDL_Rect box = layout(app);
	struct wire move[3] = {
			{DEV_POINTER, 0, EV_ABS, ABS_X, to_abs(x, box.x, box.w)},
			{DEV_POINTER, 0, EV_ABS, ABS_Y, to_abs(y, box.y, box.h)},
	};
	send_input(app, move, 2);
}

static void
initial_size(int *width, int *height) {
	SDL_Rect bounds;
	if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds)) {
		return;
	}

	SDL_Rect box = fit(bounds.w * 9 / 10, bounds.h * 9 / 10, *width, *height);
	if (box.w <= 0 || (*width <= box.w && *height <= box.h)) {
		return;
	}
	*width = box.w < 640 ? 640 : box.w;
	*height = box.h < 480 ? 480 : box.h;
}

static void
redraw(const struct app *app) {
	SDL_Rect box = layout(app);
	SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
	SDL_RenderClear(app->renderer);
	if (app->texture) {
		SDL_FRect destination = {
				(float)box.x, (float)box.y, (float)box.w, (float)box.h};
		SDL_RenderTexture(app->renderer, app->texture, NULL, &destination);
	}
	SDL_RenderPresent(app->renderer);
}

static void
retexture(struct app *app, int w, int h) {
	float have_w = 0, have_h = 0;
	if (app->texture) {
		SDL_GetTextureSize(app->texture, &have_w, &have_h);
	}
	if ((int)have_w == w && (int)have_h == h) {
		return;
	}

	bool first = !app->texture;
	SDL_DestroyTexture(app->texture);
	app->texture = SDL_CreateTexture(
			app->renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w,
			h);
	if (!app->texture) {
		die("could not create the video texture: %s", SDL_GetError());
	}
	SDL_SetTextureScaleMode(app->texture, SDL_SCALEMODE_LINEAR);

	if (first) {
		initial_size(&w, &h);
		SDL_SetWindowSize(SDL_GetRenderWindow(app->renderer), w, h);
	}
}

static void
show(struct app *app) {
	retexture(app, app->frame->width, app->frame->height);
	SDL_UpdateYUVTexture(
			app->texture, NULL, app->frame->data[0], app->frame->linesize[0],
			app->frame->data[1], app->frame->linesize[1], app->frame->data[2],
			app->frame->linesize[2]);
	redraw(app);
}

static void
ui_run(struct app *app) {
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		die("could not initialise SDL: %s", SDL_GetError());
	}
	app->wake = SDL_RegisterEvents(1);

	SDL_Window *window;
	if (!SDL_CreateWindowAndRenderer(
				"scrssh", 320, 200,
				SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window,
				&app->renderer)) {
		die("could not create the window: %s", SDL_GetError());
	}

	if (pthread_create(&app->decoder, NULL, decode, app) != 0) {
		die("could not start the decoder thread");
	}

	SDL_Event event;
	while (SDL_WaitEvent(&event)) {
		SDL_ConvertEventToRenderCoordinates(app->renderer, &event);
		switch (event.type) {
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			goto out;
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			redraw(app);
			break;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
			if (event.key.repeat) {
				break;
			}
			struct wire key[2] = {
					{DEV_KEYBOARD, 0, EV_KEY, to_evdev(event.key.scancode),
					 event.type == SDL_EVENT_KEY_DOWN}};
			send_input(app, key, 1);
			break;
		}
		case SDL_EVENT_MOUSE_MOTION:
			send_pointer(app, (int)event.motion.x, (int)event.motion.y);
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			uint16_t code = to_button(event.button.button);
			if (!code) {
				break;
			}
			send_pointer(app, (int)event.button.x, (int)event.button.y);
			struct wire click[2] = {
					{DEV_POINTER, 0, EV_KEY, code,
					 event.type == SDL_EVENT_MOUSE_BUTTON_DOWN}};
			send_input(app, click, 1);
			break;
		}
		case SDL_EVENT_MOUSE_WHEEL: {
			struct wire wheel[3] = {
					{DEV_POINTER, 0, EV_REL, REL_WHEEL, event.wheel.integer_y},
					{DEV_POINTER, 0, EV_REL, REL_HWHEEL, event.wheel.integer_x},
			};
			send_input(app, wheel, 2);
			break;
		}
		default:
			if (event.type != app->wake) {
				break;
			} else if (!event.user.data1) {
				app->error = event.user.data2;
				goto out;
			}
			av_frame_free(&app->frame);
			app->frame = event.user.data1;

			/* A newer frame is already queued; this one would never be
			   seen, so do not spend a draw on it. */
			if (!SDL_HasEvent(app->wake)) {
				show(app);
			}
			break;
		}
	}

out:
	kill(app->child, SIGTERM);
	pthread_join(app->decoder, NULL);

	av_frame_free(&app->frame);
	SDL_DestroyTexture(app->texture);
	SDL_DestroyRenderer(app->renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
}

static void
usage(FILE *out) {
	fputs("usage: scrssh [options] [--] <ssh arguments...>\n"
		  "\n"
		  "options:\n"
		  "  -d <PATH>  DRM device to capture      [default: /dev/dri/card0]\n"
		  "  -c <N>     capture a specific CRTC\n"
		  "  -p <N>     capture a specific plane\n"
		  "  -f <N>     capture frame rate         [default: 30]\n"
		  "  -b <RATE>  capped bitrate             [default: 15M]\n"
		  "  -e <NAME>  force an encoder           [default: ask the host]\n"
		  "             h264_vaapi, h264_nvenc, h264_v4l2m2m, libx264\n"
		  "  -h         show this help\n",
		  out);
}

int
main(int argc, char **argv) {
	struct config config = {
			.device = "/dev/dri/card0",
			.crtc = "",
			.plane = "",
			.fps = "30",
			.bitrate = "15M",
			.encoder = "",
	};

	for (int option; (option = getopt(argc, argv, "+d:c:p:f:b:e:h")) != -1;) {
		switch (option) {
		case 'd':
			config.device = optarg;
			break;
		case 'c':
			config.crtc = optarg;
			break;
		case 'p':
			config.plane = optarg;
			break;
		case 'f':
			config.fps = optarg;
			break;
		case 'b':
			config.bitrate = optarg;
			break;
		case 'e':
			config.encoder = optarg;
			break;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind == argc) {
		usage(stderr);
		return 2;
	}

	signal(SIGPIPE, SIG_IGN);

	struct app app = {0};
	ssh_spawn(&app, argv + optind, argc - optind);
	send_command(app.input_fd, &config);
	fcntl(app.input_fd, F_SETFL, O_NONBLOCK);

	ui_run(&app);
	waitpid(app.child, NULL, 0);
	if (app.error) {
		die("%s", app.error);
	}
	return 0;
}
