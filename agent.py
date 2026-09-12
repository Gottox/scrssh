# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Enno Boland <g@s01.de>

from array import array
import ctypes
import fcntl
import os
import signal
import struct
import subprocess
import threading
import time

# ---- Protocol with the client
#
# stdout: the MARKER line, so scrssh knows the agent is running, then MPEG-TS.
# stdin:  a big-endian uint16 length followed by that many bytes of
#         nul-terminated settings, then a stream of FMT_WIRE input events.

MARKER = b"\xff\xfeagent\xfe\xff\n"
FMT_WIRE = "!HHi"
WIRE_SIZE = struct.calcsize(FMT_WIRE)

def read_exactly(size):
	data = b""
	while len(data) < size:
		chunk = os.read(0, size - len(data))
		if not chunk:
			raise EOFError
		data += chunk
	return data

# ---- ioctl request numbers
#
# sys/ioctl.h

IOC_ALT = os.uname().machine[:3] in ("ppc", "mip", "spa", "alp")
IOC_NONE, IOC_WRITE = (1, 4) if IOC_ALT else (0, 1)
IOC_READ = 2
IOC_DIRSHIFT = 29 if IOC_ALT else 30

def ioc(direction, nr, size, kind="U"):
	return (direction << IOC_DIRSHIFT) | (size << 16) | (ord(kind) << 8) | nr

# ---- Input replay: uinput
#
# linux/input-event-codes.h
# linux/uinput.h

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
ABS_X, ABS_Y = 0x00, 0x01
REL_HWHEEL, REL_WHEEL = 0x06, 0x08
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112
BUTTONS = [BTN_LEFT, BTN_RIGHT, BTN_MIDDLE]
BUS_VIRTUAL = 0x06
KEY_ADVERTISE_MAX = 248
ABS_RANGE_MAX = 65535

FMT_INPUT_ID_SETUP = "@HHHH80sI"  # struct uinput_setup
FMT_ABS_SETUP = "@H2x6i"  # struct uinput_abs_setup
FMT_INPUT_EVENT = "@llHHi"  # struct input_event

UI_DEV_CREATE = ioc(IOC_NONE, 1, 0)
UI_DEV_SETUP = ioc(IOC_WRITE, 3, struct.calcsize(FMT_INPUT_ID_SETUP))
UI_ABS_SETUP = ioc(IOC_WRITE, 4, struct.calcsize(FMT_ABS_SETUP))
UI_SET_EVBIT = ioc(IOC_WRITE, 100, 4)
UI_SET_KEYBIT = ioc(IOC_WRITE, 101, 4)
UI_SET_RELBIT = ioc(IOC_WRITE, 102, 4)
UI_SET_ABSBIT = ioc(IOC_WRITE, 103, 4)

class Uinput:
	def __init__(self):
		self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
		for event in (EV_KEY, EV_REL, EV_ABS, EV_SYN):
			self._ioctl(UI_SET_EVBIT, event)
		for key in list(range(1, KEY_ADVERTISE_MAX + 1)) + BUTTONS:
			self._ioctl(UI_SET_KEYBIT, key)
		for rel in (REL_WHEEL, REL_HWHEEL):
			self._ioctl(UI_SET_RELBIT, rel)
		for axis in (ABS_X, ABS_Y):
			self._ioctl(UI_SET_ABSBIT, axis)
			self._ioctl(UI_ABS_SETUP, struct.pack(
					FMT_ABS_SETUP, axis, 0, 0, ABS_RANGE_MAX, 0, 0, 0))
		# 0x1D6B is the vendor id of the Linux Foundation, as used by other
		# virtual devices in the kernel.
		self._ioctl(UI_DEV_SETUP, struct.pack(
				FMT_INPUT_ID_SETUP, BUS_VIRTUAL, 0x1D6B, 1, 1, b"scrssh", 0))
		self._ioctl(UI_DEV_CREATE)

	def _ioctl(self, request, arg=0):
		fcntl.ioctl(self.fd, request, arg)

	def inject(self, typ, code, value):
		os.write(self.fd, struct.pack(FMT_INPUT_EVENT, 0, 0, typ, code, value))

# ---- Screen source: DRM
#
# drm/drm.h
# drm/drm_mode.h

DRM_CLIENT_CAP_UNIVERSAL_PLANES, DRM_CLIENT_CAP_ATOMIC = 2, 3
DRM_OBJECT_PLANE = 0xEEEEEEEE
DRM_PLANE_PRIMARY = 1
DRM_FB_MODIFIERS = 2

FMT_CLIENT_CAP = "@2Q"  # struct drm_set_client_cap
FMT_PLANE_RES = "@QI4x"  # struct drm_mode_get_plane_res
FMT_PLANE = "@6IQ"  # struct drm_mode_get_plane
FMT_FB2 = "@17I4Q"  # struct drm_mode_fb_cmd2
FMT_PRIME = "@2Ii"  # struct drm_prime_handle
FMT_OBJ_PROPS = "@2Q3I4x"  # struct drm_mode_obj_get_properties
FMT_PROP = "@2Q2I32s2I"  # struct drm_mode_get_property

DRM_SET_CLIENT_CAP = ioc(IOC_WRITE, 0x0D, struct.calcsize(FMT_CLIENT_CAP), "d")
DRM_PRIME_TO_FD = ioc(IOC_READ | IOC_WRITE, 0x2D, struct.calcsize(FMT_PRIME), "d")
DRM_GET_PROP = ioc(IOC_READ | IOC_WRITE, 0xAA, struct.calcsize(FMT_PROP), "d")
DRM_GET_PLANE_RES = ioc(IOC_READ | IOC_WRITE, 0xB5,
						struct.calcsize(FMT_PLANE_RES), "d")
DRM_GET_PLANE = ioc(IOC_READ | IOC_WRITE, 0xB6, struct.calcsize(FMT_PLANE), "d")
DRM_GET_OBJ_PROPS = ioc(IOC_READ | IOC_WRITE, 0xB9,
						struct.calcsize(FMT_OBJ_PROPS), "d")
DRM_GET_FB2 = ioc(IOC_READ | IOC_WRITE, 0xCE, struct.calcsize(FMT_FB2), "d")

FB_WIDTH, FB_HEIGHT, FB_HANDLES = 1, 2, 5

class Drm:
	def __init__(self, device):
		self.device = device
		self.fd = os.open(device, os.O_RDWR)
		for capability in (DRM_CLIENT_CAP_UNIVERSAL_PLANES, DRM_CLIENT_CAP_ATOMIC):
			try:
				fcntl.ioctl(self.fd, DRM_SET_CLIENT_CAP,
							struct.pack(FMT_CLIENT_CAP, capability, 1))
			except OSError:
				pass

	def _ioctl(self, request, fmt, *fields):
		result = fcntl.ioctl(self.fd, request, struct.pack(fmt, *fields), True)
		return struct.unpack(fmt, result)

	def _plane_ids(self):
		# Ask for the count first, then again with a buffer of that size.
		count = self._ioctl(DRM_GET_PLANE_RES, FMT_PLANE_RES, 0, 0)[1]
		ids = array("I", [0]) * count
		if ids:
			self._ioctl(DRM_GET_PLANE_RES, FMT_PLANE_RES, ids.buffer_info()[0], count)
		return ids

	def plane(self, plane_id):
		# id, crtc_id, fb_id.
		return self._ioctl(DRM_GET_PLANE, FMT_PLANE, plane_id, 0, 0, 0, 0, 0, 0)[:3]

	def _properties(self, plane_id):
		count = self._ioctl(DRM_GET_OBJ_PROPS, FMT_OBJ_PROPS,
						   0, 0, 0, plane_id, DRM_OBJECT_PLANE)[2]
		ids = array("I", [0]) * count
		values = array("Q", [0]) * count
		self._ioctl(DRM_GET_OBJ_PROPS, FMT_OBJ_PROPS, ids.buffer_info()[0],
				   values.buffer_info()[0], count, plane_id, DRM_OBJECT_PLANE)
		named = {}
		for prop_id, value in zip(ids, values):
			name = self._ioctl(DRM_GET_PROP, FMT_PROP, 0, 0, prop_id, 0, b"", 0, 0)[4]
			named[name.split(b"\0")[0].decode()] = value
		return named

	def framebuffer(self, fb_id):
		f = self._ioctl(DRM_GET_FB2, FMT_FB2, fb_id, *([0] * 20))
		return f[:5] + (f[5:9], f[9:13], f[13:17], f[17:21])

	def export(self, handle):
		return self._ioctl(DRM_PRIME_TO_FD, FMT_PRIME, handle, 0, 0)[2]

	def scanout_plane(self, crtc, requested):
		if requested:
			return int(requested)
		for plane_id in self._plane_ids():
			_, crtc_id, fb_id = self.plane(plane_id)
			if crtc and crtc_id != int(crtc):
				continue
			kind = self._properties(plane_id).get("type")
			if fb_id and kind in (DRM_PLANE_PRIMARY, None):
				return plane_id
		raise OSError("no scanout plane with a framebuffer")

# ---- Pixel conversion: EGL and GLES
#
# EGL/egl.h
# EGL/eglext.h
# GLES2/gl2.h

EGL_PLATFORM_GBM = 0x31D7
EGL_LINUX_DMA_BUF = 0x3270
EGL_HEIGHT, EGL_WIDTH, EGL_FOURCC = 0x3056, 0x3057, 0x3271
EGL_CONTEXT_VERSION, EGL_ES_API, EGL_NONE = 0x3098, 0x30A0, 0x3038
EGL_PLANES = (
	(0x3272, 0x3273, 0x3274, 0x3443, 0x3444),
	(0x3275, 0x3276, 0x3277, 0x3445, 0x3446),
	(0x3278, 0x3279, 0x327A, 0x3447, 0x3448),
	(0x3440, 0x3441, 0x3442, 0x3449, 0x344A),
)

GL_TEXTURE_2D, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 = 0x0DE1, 0x8D40, 0x8CE0
GL_FRAMEBUFFER_COMPLETE, GL_RGBA, GL_UNSIGNED_BYTE = 0x8CD5, 0x1908, 0x1401
GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_LINEAR = 0x2801, 0x2800, 0x2601
GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_LINK_STATUS = 0x8B31, 0x8B30, 0x8B82
GL_FLOAT, GL_TRIANGLE_STRIP = 0x1406, 0x0005
GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE = 0x2802, 0x2803, 0x812F

VERTEX = b"""
attribute vec2 pos;

void main() {
	gl_Position = vec4(pos, 0.0, 1.0);
}
"""

FRAGMENT = b"""
precision highp float;

uniform sampler2D screen;
uniform vec2 size;

vec3 pixel(float x, float y) {
	return texture2D(screen, vec2(x, y) / size).rgb;
}

float luma(float x, float y) {
	vec3 c = pixel(x, y);
	return 0.0627 + dot(c, vec3(0.2568, 0.5041, 0.0979));
}

vec2 chroma_pair(float x, float y) {
	vec3 c = pixel(x, y);
	return vec2(0.5 - 0.1482 * c.r - 0.2910 * c.g + 0.4392 * c.b,
				0.5 + 0.4392 * c.r - 0.3678 * c.g - 0.0714 * c.b);
}

void main() {
	float x = gl_FragCoord.x * 4.0;
	if (gl_FragCoord.y > size.y) {
		float y = (gl_FragCoord.y - size.y) * 2.0;
		gl_FragColor = vec4(chroma_pair(x - 1.0, y), chroma_pair(x + 1.0, y));
	} else {
		float y = gl_FragCoord.y;
		gl_FragColor = vec4(luma(x - 1.5, y), luma(x - 0.5, y),
							luma(x + 0.5, y), luma(x + 1.5, y));
	}
}
"""

class Converter:
	def __init__(self, drm, width, height):
		self.drm = drm
		self._load_libraries()
		self._open_context()
		self._build_program(width, height)
		self._make_target(width // 4, height * 3 // 2)
		# One NV12 frame: the Y plane followed by the interleaved UV plane.
		self.pixels = (ctypes.c_char * (width * height * 3 // 2))()
		self.view = memoryview(self.pixels)

	def _load_libraries(self):
		# ctypes assumes every function returns an int. Pointers need their
		# real type declared or they are truncated to 32 bits.
		self.egl = ctypes.CDLL("libEGL.so.1")
		self.gl = ctypes.CDLL("libGLESv2.so.2")
		self.gbm = ctypes.CDLL("libgbm.so.1")
		self.gbm.gbm_create_device.restype = ctypes.c_void_p
		self.egl.eglGetProcAddress.restype = ctypes.c_void_p
		self.egl.eglGetPlatformDisplay.restype = ctypes.c_void_p
		self.egl.eglCreateContext.restype = ctypes.c_void_p
		self.gl.glUniform2f.argtypes = [ctypes.c_int, ctypes.c_float, ctypes.c_float]
		self.gl.glUniform1f.argtypes = [ctypes.c_int, ctypes.c_float]
		# The dma-buf import is an extension, reachable only by address.
		self.create_image = self._extension(
				b"eglCreateImageKHR", ctypes.c_void_p, ctypes.c_void_p,
				ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p,
				ctypes.POINTER(ctypes.c_uint32))
		self.destroy_image = self._extension(
				b"eglDestroyImageKHR", ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p)
		self.image_target = self._extension(
				b"glEGLImageTargetTexture2DOES", None, ctypes.c_uint, ctypes.c_void_p)

	def _extension(self, name, *signature):
		address = self.egl.eglGetProcAddress(name)
		if not address:
			raise OSError("EGL lacks " + name.decode())
		return ctypes.CFUNCTYPE(*signature)(address)

	def _open_context(self):
		# A GBM device on the DRM fd gives EGL a display without any window
		# system, and a surfaceless context is enough to render into textures.
		gbm_device = ctypes.c_void_p(self.gbm.gbm_create_device(self.drm.fd))
		self.display = ctypes.c_void_p(self.egl.eglGetPlatformDisplay(
				EGL_PLATFORM_GBM, gbm_device, None))
		if not self.display.value or not self.egl.eglInitialize(
				self.display, None, None):
			raise OSError("no EGL display for " + self.drm.device)
		self.egl.eglBindAPI(EGL_ES_API)
		version = (ctypes.c_int * 3)(EGL_CONTEXT_VERSION, 2, EGL_NONE)
		self.context = ctypes.c_void_p(self.egl.eglCreateContext(
				self.display, None, None, version))
		if not self.context.value or not self.egl.eglMakeCurrent(
				self.display, None, None, self.context):
			raise OSError("no EGL context for " + self.drm.device)
		# Kept on self because GL stores only the pointer to the quad.
		self.quad = (ctypes.c_float * 8)(-1, -1, 1, -1, -1, 1, 1, 1)
		self.gl.glEnableVertexAttribArray(0)
		self.gl.glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, self.quad)

	def _shader(self, program, kind, src):
		shader = self.gl.glCreateShader(kind)
		self.gl.glShaderSource(shader, 1, ctypes.byref(ctypes.c_char_p(src)), None)
		self.gl.glCompileShader(shader)
		self.gl.glAttachShader(program, shader)

	def _build_program(self, width, height):
		program = self.gl.glCreateProgram()
		self._shader(program, GL_VERTEX_SHADER, VERTEX)
		self._shader(program, GL_FRAGMENT_SHADER, FRAGMENT)
		self.gl.glBindAttribLocation(program, 0, b"pos")
		self.gl.glLinkProgram(program)
		linked = ctypes.c_int()
		self.gl.glGetProgramiv(program, GL_LINK_STATUS, ctypes.byref(linked))
		if not linked.value:
			raise OSError("cannot build the EGL conversion shader")
		self.gl.glUseProgram(program)
		self.gl.glUniform1i(self.gl.glGetUniformLocation(program, b"screen"), 0)
		self.gl.glUniform2f(self.gl.glGetUniformLocation(program, b"size"),
							width, height)

	def _make_target(self, width, height):
		# Bound and sized once: nothing else ever draws or changes the viewport.
		target, fbo = ctypes.c_uint(), ctypes.c_uint()
		self.gl.glGenTextures(1, ctypes.byref(target))
		self.gl.glBindTexture(GL_TEXTURE_2D, target)
		self.gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
							 GL_RGBA, GL_UNSIGNED_BYTE, None)
		self.gl.glGenFramebuffers(1, ctypes.byref(fbo))
		self.gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo)
		self.gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
									   GL_TEXTURE_2D, target, 0)
		if self.gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE:
			raise OSError("no render target for the EGL conversion")
		self.gl.glViewport(0, 0, width, height)
		self.target = width, height

	def _import_texture(self, fb):
		_, width, height, fourcc, flags, handles, pitches, offsets, modifiers = fb
		attributes = [EGL_WIDTH, width, EGL_HEIGHT, height, EGL_FOURCC, fourcc]
		fds = []
		for handle, keys, pitch, offset, modifier in zip(
				handles, EGL_PLANES, pitches, offsets, modifiers):
			if not handle:
				break
			fd = self.drm.export(handle)
			fds.append(fd)
			fd_key, offset_key, pitch_key, lo_key, hi_key = keys
			attributes += [fd_key, fd, offset_key, offset, pitch_key, pitch]
			if flags & DRM_FB_MODIFIERS:
				attributes += [lo_key, modifier & 0xFFFFFFFF, hi_key, modifier >> 32]
		attributes.append(EGL_NONE)
		image = self.create_image(self.display, None, EGL_LINUX_DMA_BUF, None,
								  (ctypes.c_uint32 * len(attributes))(*attributes))
		# EGL holds its own references from here on.
		for exported in fds:
			os.close(exported)
		if not image:
			raise OSError("cannot import the framebuffer into EGL")
		texture = ctypes.c_uint()
		self.gl.glGenTextures(1, ctypes.byref(texture))
		self.gl.glBindTexture(GL_TEXTURE_2D, texture)
		self.image_target(GL_TEXTURE_2D, image)
		# GL_LINEAR matters: the chroma pass relies on it to average 2x2 blocks.
		self.gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
		self.gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)
		self.gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE)
		self.gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE)
		return image, texture

	def convert(self, fb):
		# The returned view is overwritten by the next call.
		image, texture = self._import_texture(fb)
		self.gl.glBindTexture(GL_TEXTURE_2D, texture)
		self.gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)
		self.gl.glReadPixels(0, 0, *self.target, GL_RGBA, GL_UNSIGNED_BYTE,
							 ctypes.byref(self.pixels))
		self.gl.glDeleteTextures(1, ctypes.byref(texture))
		self.destroy_image(self.display, image)
		return self.view

# ---- Screen
#
# The scanout plane: pick it once, then hand out whatever buffer the
# compositor has flipped onto it.

class Screen:
	def __init__(self, drm, crtc, plane):
		self.drm = drm
		self.plane_id = drm.scanout_plane(crtc, plane)
		fb = self._current_framebuffer()
		if not fb:
			raise OSError("no framebuffer on the capture plane")
		# GETFB2 fills in the buffer handles only for CAP_SYS_ADMIN; everyone
		# else gets zeros and cannot export the buffer.
		if not fb[FB_HANDLES][0]:
			raise OSError("the framebuffer is not accessible; capturing needs root")
		# The mode the session started with. frame() stops when it changes.
		self.source = fb[FB_WIDTH], fb[FB_HEIGHT]
		# NV12 needs even dimensions and the luma pass packs 4 pixels per
		# texel, so trim to multiples of 4 by 2. At most 3 columns and 1 row
		# are lost.
		self.width = fb[FB_WIDTH] // 4 * 4
		self.height = fb[FB_HEIGHT] // 2 * 2
		self.converter = Converter(self.drm, self.width, self.height)
		# Convert one frame right away, so a driver that cannot import the
		# buffer fails here rather than after ffmpeg has started.
		self.frame()

	def _current_framebuffer(self):
		_, _, fb_id = self.drm.plane(self.plane_id)
		return self.drm.framebuffer(fb_id) if fb_id else None

	def frame(self):
		# None ends the stream. A resolution change would need new render
		# targets and a new ffmpeg, so the client reports the stream ended
		# instead.
		fb = self._current_framebuffer()
		if not fb or (fb[FB_WIDTH], fb[FB_HEIGHT]) != self.source:
			return None
		return self.converter.convert(fb)

# ---- Encoder: ffmpeg
#
# ffmpeg reads raw NV12 frames on its stdin and writes MPEG-TS to its stdout,
# which is the stdout scrssh reads. The table lists one hardware encoder per
# vendor plus the software fallback, each with its low-latency flags.

FFMPEG = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
# A synthetic source and a single frame, which is all it takes to tell whether
# an encoder starts on this host.
PROBE = ["-f", "lavfi", "-i", "color=black:s=256x256:r=30,format=nv12",
		 "-frames:v", "1"]

ENCODERS = (
	("h264_vaapi", ["-init_hw_device", "vaapi=va", "-filter_hw_device", "va",
					"-vf", "hwupload", "-rc_mode", "CBR"]),
	("h264_nvenc", ["-vf", "hwupload_cuda", "-preset", "p1", "-tune", "ll",
					"-zerolatency", "1"]),
	("h264_v4l2m2m", ["-vf", "format=yuv420p"]),
	("libx264", ["-preset", "ultrafast", "-tune", "zerolatency"]),
)

class Encoder:
	def __init__(self, screen, fps, bitrate, wanted):
		candidates = [c for c in ENCODERS if not wanted or c[0] == wanted]
		if not candidates:
			raise SystemExit("unknown encoder " + wanted)
		name, options = self._probe(candidates, bitrate)
		# From here on the only child is ffmpeg, and its exit ends the session.
		# Installed after the probes, which spawn children of their own.
		signal.signal(signal.SIGCHLD, lambda *_: os._exit(0))
		size = "%dx%d" % (screen.width, screen.height)
		source = ["-f", "rawvideo", "-pix_fmt", "nv12",
				  "-framerate", fps, "-video_size", size, "-i", "-"]
		self.process = subprocess.Popen(
				self._command(source, name, options, bitrate),
				stdin=subprocess.PIPE)

	def _command(self, source, encoder, options, bitrate):
		# No B-frames and no muxer buffering, so a packet leaves as soon as it
		# is encoded.
		return (
			FFMPEG + source + ["-c:v", encoder] + options
			+ ["-b:v", bitrate, "-maxrate", bitrate, "-g", "60", "-bf", "0"]
			+ ["-f", "mpegts", "-flush_packets", "1", "-muxdelay", "0",
			   "-muxpreload", "0", "pipe:1"]
		)

	def _probe(self, candidates, bitrate):
		# Encode one synthetic frame with each candidate and keep the first that
		# produces output. The last one is never probed: it is either the software
		# fallback or the single encoder forced with -e, which is used regardless.
		for name, options in candidates[:-1]:
			if subprocess.run(self._command(PROBE, name, options, bitrate),
							  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
							  stderr=subprocess.DEVNULL).stdout:
				return name, options
		return candidates[-1]

	def write(self, view):
		# False once ffmpeg is gone or replay_input closed the pipe.
		try:
			# A buffered stdin retries the short write a signal can cause.
			self.process.stdin.write(view)
			self.process.stdin.flush()
		except (OSError, ValueError):
			return False
		return True

	def close(self):
		# Also how replay_input ends a session: the next write returns False.
		self.process.stdin.close()

	def wait(self):
		try:
			self.process.wait(2)
		except subprocess.TimeoutExpired:
			self.process.kill()

# ---- Main

def stream_frames(screen, encoder, fps):
	# Convert and push frames at a steady pace. When a frame takes longer than
	# its slot, restart the clock instead of trying to catch up.
	interval = 1.0 / float(fps)
	upcoming = time.monotonic()
	while view := screen.frame():
		if not encoder.write(view):
			break
		upcoming += interval
		delay = upcoming - time.monotonic()
		if delay > 0:
			time.sleep(delay)
		else:
			upcoming = time.monotonic()

def replay_input(uinput, encoder):
	# Runs on its own thread. EOF on stdin means the client hung up: closing
	# the pipe to ffmpeg makes stream_frames stop on its next write, which
	# ends the session.
	try:
		while True:
			typ, code, value = struct.unpack(FMT_WIRE, read_exactly(WIRE_SIZE))
			uinput.inject(typ, code, value)
	except EOFError:
		pass
	encoder.close()

# scrssh ignores everything on stdout before this line, such as login
# banners.
os.write(1, MARKER)
# uinput is rarely loaded on servers. The PATH is spelled out because
# sudo and doas may hand over a minimal environment.
subprocess.run(["modprobe", "uinput"],
			   env={"PATH": "/sbin:/usr/sbin:/bin:/usr/bin"},
			   stdout=subprocess.DEVNULL)
config = read_exactly(struct.unpack("!H", read_exactly(2))[0]).decode()
device, crtc, plane, fps, bitrate, wanted, _ = config.split("\0")
uinput = Uinput()
# AttributeError covers EGL libraries too old for the entry points used
# here. SystemExit prints the bare message, which is what the README
# lists under Troubleshooting.
try:
	screen = Screen(device, crtc, plane)
	encoder = Encoder(screen, fps, bitrate, wanted)
	os.close(1)
	threading.Thread(target=replay_input, args=(uinput, encoder), daemon=True).start()
	stream_frames(screen, encoder, fps)
except (OSError, AttributeError) as error:
	raise SystemExit(error)
encoder.close()
encoder.wait()
