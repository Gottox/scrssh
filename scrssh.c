#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define LENGTH(a) (sizeof(a) / sizeof(*(a)))
#define AVIO_BUFFER 65536
#define MARKER(stage) "\xff\xfe" stage "\xfe\xff"

static const char AGENT[] = {
#include "agent.h"
};

struct {
	char title[256];
	bool use_sudo;
	struct termios tio;
	pid_t child;
	int input_fd;
	int video_fd;
	Uint32 wake;
	SDL_Thread *decoder;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	AVFrame *frame;
	const char *error;
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
	const char **post_argv =
			(const char *[]){"printf '" MARKER("1") "'; exec", "sudo -S",
							 "python3 -u -c", AGENT, NULL};
	size_t post_argc = 5;
	if (!app.use_sudo) {
		post_argv[1] = post_argv[0];
		post_argv++;
		post_argc--;
	}

	char **exec_argv = calloc(argc + post_argc + 1, sizeof(char *));
	if (!exec_argv) {
		die("out of memory");
	}
	exec_argv[0] = "ssh";
	memcpy(&exec_argv[1], argv, argc * sizeof(char *));
	memcpy(&exec_argv[argc + 1], post_argv, post_argc * sizeof(char *));

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
	no_echo.c_lflag &= ~(tcflag_t)ECHO;
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
		if (poll(fds, LENGTH(fds), -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			die("could not wait for the remote agent: %s", strerror(errno));
		}

		if (fds[1].revents) {
			ssize_t n = read(in, buffer, sizeof(buffer));
			if (n > 0) {
				put(app.input_fd, buffer, (size_t)n);
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
decoder_failed(const char *message) {
	SDL_Event event = {0};
	event.type = app.wake;
	event.user.data2 = (void *)(uintptr_t)message;
	SDL_PushEvent(&event);
}

static void
publish(AVFrame *decoded) {
	AVFrame *sent = av_frame_alloc();
	if (!sent) {
		return;
	}
	av_frame_move_ref(sent, decoded);

	SDL_Event event = {0};
	event.type = app.wake;
	event.user.data1 = sent;
	if (!SDL_PushEvent(&event)) {
		av_frame_free(&sent);
	}
}

static bool
decode_packet(
		AVCodecContext *decoder, int stream, AVPacket *packet,
		AVFrame *decoded) {
	if (packet->stream_index != stream ||
		avcodec_send_packet(decoder, packet) < 0) {
		return true;
	}

	while (avcodec_receive_frame(decoder, decoded) >= 0) {
		if (decoded->format != AV_PIX_FMT_YUV420P) {
			decoder_failed(
					"the remote sent a pixel format scrssh cannot show; "
					"kmsgrab and VAAPI produce 8-bit 4:2:0");
			return false;
		}
		publish(decoded);
	}

	return true;
}

static int
decode(void *arg) {
	(void)arg;

	uint8_t *buffer = av_malloc(AVIO_BUFFER);
	AVFormatContext *format = avformat_alloc_context();
	AVIOContext *avio = buffer && format
			? avio_alloc_context(
					  buffer, AVIO_BUFFER, 0, (void *)(intptr_t)app.video_fd,
					  read_stream, NULL, NULL)
			: NULL;
	if (!avio) {
		decoder_failed("could not set up the demuxer");
		return 0;
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
		decoder_failed("could not open the remote video stream");
		goto done;
	}
	const AVCodec *codec = NULL;
	int stream = -1;
	if (avformat_find_stream_info(format, NULL) < 0 ||
		(stream = av_find_best_stream(
				 format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0) {
		decoder_failed("the remote stream contains no video");
		goto done;
	}

	packet = av_packet_alloc();
	decoded = av_frame_alloc();
	if (!packet || !decoded || !(decoder = avcodec_alloc_context3(codec)) ||
		avcodec_parameters_to_context(
				decoder, format->streams[stream]->codecpar) < 0) {
		decoder_failed("could not open the H.264 decoder");
		goto done;
	}
	decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (avcodec_open2(decoder, codec, NULL) < 0) {
		decoder_failed("could not open the H.264 decoder");
		goto done;
	}

	while (av_read_frame(format, packet) >= 0) {
		bool more = decode_packet(decoder, stream, packet, decoded);
		av_packet_unref(packet);
		if (!more) {
			goto done;
		}
	}

	decoder_failed("the remote video stream ended");

done:
	av_frame_free(&decoded);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&format);

	av_free(avio->buffer);
	avio_context_free(&avio);
	return 0;
}

enum { DEV_KEYBOARD, DEV_POINTER };

#define ABS_RANGE_MAX 65535

struct ev {
	uint8_t device;
	uint8_t pad;
	uint16_t type;
	uint16_t code;
	int32_t value;
} __attribute__((packed));

_Static_assert(sizeof(struct ev) == 10, "the wire record must not be padded");

static void
send_input(struct ev *ev, size_t ev_count) {
	ev[ev_count - 1].device = ev[ev_count - 2].device;
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

static SDL_Rect
fit(SDL_Rect area, SDL_Rect video) {
	if (video.w <= 0 || video.h <= 0 || area.w <= 0 || area.h <= 0) {
		return (SDL_Rect){0, 0, 0, 0};
	}

	SDL_Rect box = area;
	if ((long)video.w * area.h > (long)video.h * area.w) {
		box.h = (int)((long)video.h * area.w / video.w);
	} else {
		box.w = (int)((long)video.w * area.h / video.h);
	}
	box.x = area.x + (area.w - box.w) / 2;
	box.y = area.y + (area.h - box.h) / 2;
	return box;
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
send_pointer(float x, float y) {
	int w = 0, h = 0;
	SDL_RendererLogicalPresentation mode;
	SDL_GetRenderLogicalPresentation(app.renderer, &w, &h, &mode);
	struct ev move[3] = {
			{DEV_POINTER, 0, EV_ABS, ABS_X, to_abs(x, w)},
			{DEV_POINTER, 0, EV_ABS, ABS_Y, to_abs(y, h)},
	};
	send_input(move, LENGTH(move));
}

static void
initial_size(int *width, int *height) {
	SDL_Rect bounds;
	if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds)) {
		return;
	}

	bounds.w = bounds.w * 9 / 10;
	bounds.h = bounds.h * 9 / 10;
	SDL_Rect box = fit(bounds, (SDL_Rect){0, 0, *width, *height});
	if (box.w <= 0 || (*width <= box.w && *height <= box.h)) {
		return;
	}
	*width = box.w;
	*height = box.h;
}

static void
redraw(void) {
	if (SDL_HasEvent(app.wake) || SDL_HasEvent(SDL_EVENT_WINDOW_RESIZED) ||
		SDL_HasEvent(SDL_EVENT_QUIT)) {
		return;
	}
	SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
	SDL_RenderClear(app.renderer);
	if (app.texture) {
		SDL_RenderTexture(app.renderer, app.texture, NULL, NULL);
	}
	SDL_RenderPresent(app.renderer);
}

static void
retexture(void) {
	int w = app.frame->width, h = app.frame->height;
	if (!app.renderer) {
		int window_w = w, window_h = h;
		initial_size(&window_w, &window_h);

		SDL_Window *window;
		if (!SDL_CreateWindowAndRenderer(
					app.title, window_w, window_h,
					SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
					&window, &app.renderer)) {
			die("could not create the window: %s", SDL_GetError());
		}
	}
	int have_w = 0, have_h = 0;
	SDL_RendererLogicalPresentation mode;
	SDL_GetRenderLogicalPresentation(app.renderer, &have_w, &have_h, &mode);
	if (have_w == w && have_h == h) {
		return;
	}

	SDL_DestroyTexture(app.texture);
	app.texture = SDL_CreateTexture(
			app.renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w,
			h);
	if (!app.texture) {
		die("could not create the video texture: %s", SDL_GetError());
	}
	SDL_SetTextureScaleMode(app.texture, SDL_SCALEMODE_LINEAR);
	SDL_SetRenderLogicalPresentation(
			app.renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

static void
run(void) {
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		die("could not initialise SDL: %s", SDL_GetError());
	}
	app.wake = SDL_RegisterEvents(1);

	app.decoder = SDL_CreateThread(decode, "decoder", NULL);
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
					{DEV_KEYBOARD, 0, EV_KEY, to_scancode(event.key.scancode),
					 event.type == SDL_EVENT_KEY_DOWN}};
			send_input(key, LENGTH(key));
		} break;
		case SDL_EVENT_MOUSE_MOTION: {
			send_pointer(event.motion.x, event.motion.y);
		} break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			uint16_t code = to_button(event.button.button);
			if (!code) {
				break;
			}
			send_pointer(event.button.x, event.button.y);
			struct ev click[2] = {
					{DEV_POINTER, 0, EV_KEY, code,
					 event.type == SDL_EVENT_MOUSE_BUTTON_DOWN}};
			send_input(click, LENGTH(click));
		} break;
		case SDL_EVENT_MOUSE_WHEEL: {
			struct ev wheel[3] = {
					{DEV_POINTER, 0, EV_REL, REL_WHEEL, event.wheel.integer_y},
					{DEV_POINTER, 0, EV_REL, REL_HWHEEL, event.wheel.integer_x},
			};
			send_input(wheel, LENGTH(wheel));
		} break;
		default: {
			if (event.type != app.wake) {
				break;
			} else if (!event.user.data1) {
				app.error = event.user.data2;
				goto out;
			}
			av_frame_free(&app.frame);
			app.frame = event.user.data1;

			if (SDL_HasEvent(app.wake)) {
				break;
			}

			retexture();
			SDL_UpdateYUVTexture(
					app.texture, NULL, app.frame->data[0],
					app.frame->linesize[0], app.frame->data[1],
					app.frame->linesize[1], app.frame->data[2],
					app.frame->linesize[2]);
			redraw();
		} break;
		}
	}

out:
	kill(app.child, SIGTERM);
	SDL_WaitThread(app.decoder, NULL);

	av_frame_free(&app.frame);
	SDL_DestroyTexture(app.texture);
	if (app.renderer) {
		SDL_Window *window = SDL_GetRenderWindow(app.renderer);
		SDL_DestroyRenderer(app.renderer);
		SDL_DestroyWindow(window);
	}
	SDL_Quit();
}

static void
set_title(char **argv, size_t argc) {
	app.title[0] = 0;
	for (size_t i = 0; i < argc; i++) {
		if (sizeof(app.title) > strlen(app.title) + strlen(argv[i]) + 1) {
			if (i) {
				strcat(app.title, " ");
			}
			strcat(app.title, argv[i]);
		}
	}
}

static void
usage(void) {
	fputs("usage: scrssh [options] [--] <ssh arguments...>\n"
		  "\n"
		  "options:\n"
		  "  -s         run the agent under `sudo -S`\n"
		  "  -d <PATH>  DRM device to capture      [default: /dev/dri/card0]\n"
		  "  -C <N>     capture a specific CRTC\n"
		  "  -P <N>     capture a specific plane\n"
		  "  -f <N>     capture frame rate         [default: 30]\n"
		  "  -B <RATE>  capped bitrate             [default: 500K]\n"
		  "  -e <NAME>  force an encoder           [default: ask the host]\n"
		  "             h264_vaapi, h264_nvenc, h264_v4l2m2m, libx264\n"
		  "  -h         show this help\n",
		  stderr);
}

int
main(int argc, char **argv) {
	const char *config[] = {"/dev/dri/card0", "", "", "30", "500K", ""};

	for (int o; (o = getopt(argc, argv, "+d:C:P:f:B:e:sh")) != -1;) {
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
		case 's':
			app.use_sudo = true;
			break;
		default:
			usage();
			return 1;
		}
	}
	if (optind == argc) {
		usage();
		return 2;
	}

	signal(SIGPIPE, SIG_IGN);

	ssh_spawn(argv + optind, argc - optind);
	set_title(argv + optind, argc - optind);

	greeting(MARKER("1"), -1);
	echo_off();
	greeting(MARKER("2"), STDIN_FILENO);

	send_config(app.input_fd, config);
	fcntl(app.input_fd, F_SETFL, O_NONBLOCK);

	run();
	waitpid(app.child, NULL, 0);

	if (app.error) {
		die("%s", app.error);
	}
	return 0;
}
