import fcntl
import os
import struct
import subprocess
import threading

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
ABS_X, ABS_Y = 0x00, 0x01
REL_HWHEEL, REL_WHEEL = 0x06, 0x08
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112
BUS_VIRTUAL = 0x06
KEY_ADVERTISE_MAX = 248
ABS_RANGE_MAX = 65535

FMT_INPUT_ID_SETUP = "@HHHH80sI"
FMT_ABS_SETUP = "@H2x6i"
FMT_INPUT_EVENT = "@llHHi"
FMT_WIRE = "!BBHHi"
WIRE_SIZE = struct.calcsize(FMT_WIRE)

def ioc(direction, nr, size):
	return (direction << 30) | (size << 16) | (ord("U") << 8) | nr

UI_DEV_CREATE = ioc(0, 1, 0)
UI_DEV_SETUP = ioc(1, 3, struct.calcsize(FMT_INPUT_ID_SETUP))
UI_ABS_SETUP = ioc(1, 4, struct.calcsize(FMT_ABS_SETUP))
UI_SET_EVBIT = ioc(1, 100, 4)
UI_SET_KEYBIT = ioc(1, 101, 4)
UI_SET_RELBIT = ioc(1, 102, 4)
UI_SET_ABSBIT = ioc(1, 103, 4)

def read_exactly(size):
	data = b""
	while len(data) < size:
		chunk = os.read(0, size - len(data))
		if not chunk:
			raise EOFError
		data += chunk
	return data

def create_device(name, product, ev_bits, key_bits, rel_bits, abs_bits):
	subprocess.run(["modprobe", "uinput"], env={"PATH": "/sbin:/usr/sbin:/bin:/usr/bin"})
	fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
	for ev in ev_bits:
		fcntl.ioctl(fd, UI_SET_EVBIT, ev)
	for key in key_bits:
		fcntl.ioctl(fd, UI_SET_KEYBIT, key)
	for rel in rel_bits:
		fcntl.ioctl(fd, UI_SET_RELBIT, rel)
	for axis in abs_bits:
		fcntl.ioctl(fd, UI_SET_ABSBIT, axis)
		fcntl.ioctl(
			fd,
			UI_ABS_SETUP,
			struct.pack(FMT_ABS_SETUP, axis, 0, 0, ABS_RANGE_MAX, 0, 0, 0),
		)
	fcntl.ioctl(
		fd,
		UI_DEV_SETUP,
		struct.pack(
			FMT_INPUT_ID_SETUP, BUS_VIRTUAL, 0x1D6B, product, 1,
			name.encode("ascii"), 0,
		),
	)
	fcntl.ioctl(fd, UI_DEV_CREATE)
	return fd

def forward(fds):
	try:
		while True:
			record = read_exactly(WIRE_SIZE)
			dev, _pad, typ, code, value = struct.unpack(FMT_WIRE, record)
			if dev < len(fds):
				os.write(fds[dev], struct.pack(FMT_INPUT_EVENT, 0, 0, typ, code, value))
	except EOFError:
		pass

DOWNLOADS = (
	"hwmap=derive_device=vaapi,scale_vaapi=format=nv12,hwdownload,format=nv12",
	"hwdownload,format=bgra",
	"hwdownload,format=rgb0",
	"hwdownload,format=rgba",
	"hwdownload,format=bgr0",
)

ENCODERS = (
	("h264_vaapi", ["-vf", "hwmap=derive_device=vaapi,scale_vaapi=format=nv12",
					"-rc_mode", "CBR"]),
	("h264_nvenc", ["-vf", "%s,hwupload_cuda",
					"-preset", "p1", "-tune", "ll", "-zerolatency", "1"]),
	("h264_v4l2m2m", ["-vf", "%s"]),
	("libx264", ["-vf", "%s", "-preset", "ultrafast",
				 "-tune", "zerolatency"]),
)

def command(encoder, options, probe):
	return (
		["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "kmsgrab",
		 "-device", device, "-framerate", fps]
		+ (["-crtc_id", crtc] if crtc else [])
		+ (["-plane_id", plane] if plane else [])
		+ ["-i", "-", "-c:v", encoder] + options
		+ ["-b:v", bitrate, "-maxrate", bitrate, "-g", "60", "-bf", "0"]
		+ (["-frames:v", "1"] if probe else [])
		+ ["-f", "mpegts", "-flush_packets", "1", "-muxdelay", "0",
		   "-muxpreload", "0", "pipe:1"]
	)

def choose(candidates):
	probes = [
		subprocess.Popen(command(name, options, True),
						 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
						 stderr=subprocess.DEVNULL)
		for name, options in candidates[:-1]
	]
	chosen = candidates[-1]
	for candidate in candidates[:-1]:
		if probes.pop(0).communicate()[0]:
			chosen = candidate
			break
	for probe in probes:
		probe.kill()
	return chosen

size = struct.unpack("!H", read_exactly(2))[0]
device, crtc, plane, fps, bitrate, encoder, _ = (
	read_exactly(size).decode().split("\0")
)

candidates = [
	(name, [option.replace("%s", download + ",format=yuv420p") for option in options])
	for name, options in ENCODERS
	if not encoder or name == encoder
	for download in (DOWNLOADS if any("%s" in o for o in options) else ("",))
]
if not candidates:
	raise SystemExit("unknown encoder " + encoder)

fds = (
	create_device("scrssh keyboard", 0x0001, [EV_KEY, EV_SYN],
				  range(1, KEY_ADVERTISE_MAX + 1), [], []),
	create_device("scrssh pointer", 0x0002, [EV_KEY, EV_REL, EV_ABS, EV_SYN],
				  [BTN_LEFT, BTN_RIGHT, BTN_MIDDLE], [REL_WHEEL, REL_HWHEEL],
				  [ABS_X, ABS_Y]),
)

threading.Thread(target=forward, args=(fds,), daemon=True).start()

name, options = choose(candidates)
process = subprocess.Popen(
	command(name, options, False), stdin=subprocess.DEVNULL)
process.wait()
