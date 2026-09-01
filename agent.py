# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Enno Boland <g@s01.de>

import fcntl
import os
import signal
import struct
import subprocess

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
FMT_WIRE = "!HHi"
WIRE_SIZE = struct.calcsize(FMT_WIRE)

IOC_ALT = os.uname().machine[:3] in ("ppc", "mip", "spa", "alp")
IOC_NONE, IOC_WRITE = (1, 4) if IOC_ALT else (0, 1)
IOC_DIRSHIFT = 29 if IOC_ALT else 30

def ioc(direction, nr, size):
	return (direction << IOC_DIRSHIFT) | (size << 16) | (ord("U") << 8) | nr

UI_DEV_CREATE = ioc(IOC_NONE, 1, 0)
UI_DEV_SETUP = ioc(IOC_WRITE, 3, struct.calcsize(FMT_INPUT_ID_SETUP))
UI_ABS_SETUP = ioc(IOC_WRITE, 4, struct.calcsize(FMT_ABS_SETUP))
UI_SET_EVBIT = ioc(IOC_WRITE, 100, 4)
UI_SET_KEYBIT = ioc(IOC_WRITE, 101, 4)
UI_SET_RELBIT = ioc(IOC_WRITE, 102, 4)
UI_SET_ABSBIT = ioc(IOC_WRITE, 103, 4)

def read_exactly(size):
	data = b""
	while len(data) < size:
		chunk = os.read(0, size - len(data))
		if not chunk:
			raise EOFError
		data += chunk
	return data

def create_device(name, product, ev_bits, key_bits, rel_bits, abs_bits):
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

def capture():
	return (
		["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "kmsgrab",
		 "-device", device, "-framerate", fps]
		+ (["-crtc_id", crtc] if crtc else [])
		+ (["-plane_id", plane] if plane else [])
		+ ["-i", "-"]
	)

def command(encoder, options, probe):
	return (
		capture() + ["-c:v", encoder] + options
		+ ["-b:v", bitrate, "-maxrate", bitrate, "-g", "60", "-bf", "0"]
		+ (["-frames:v", "1"] if probe else [])
		+ ["-f", "mpegts", "-flush_packets", "1", "-muxdelay", "0",
		   "-muxpreload", "0", "pipe:1"]
	)

def run(argv, **kwargs):
	return subprocess.run(argv, stdin=subprocess.DEVNULL,
						  stderr=subprocess.DEVNULL, **kwargs)

def find_download():
	for download in DOWNLOADS:
		chain = download + ",format=yuv420p"
		if not run(capture() + ["-vf", chain, "-frames:v", "1",
								"-f", "null", "-"],
				   stdout=subprocess.DEVNULL).returncode:
			return chain
	return DOWNLOADS[-1] + ",format=yuv420p"

download = None

def resolve(candidate):
	global download
	name, options = candidate
	if not any("%s" in option for option in options):
		return candidate
	if download is None:
		download = find_download()
	return name, [option.replace("%s", download) for option in options]

def choose(candidates):
	for candidate in candidates[:-1]:
		candidate = resolve(candidate)
		if run(command(*candidate, True), stdout=subprocess.PIPE).stdout:
			return candidate
	return resolve(candidates[-1])

os.write(1, b"\xff\xfeagent\xfe\xff\n")
subprocess.run(["modprobe", "uinput"], env={"PATH": "/sbin:/usr/sbin:/bin:/usr/bin"},
			   stdout=subprocess.DEVNULL)
size = struct.unpack("!H", read_exactly(2))[0]
device, crtc, plane, fps, bitrate, encoder, _ = (
	read_exactly(size).decode().split("\0")
)

candidates = [c for c in ENCODERS if not encoder or c[0] == encoder]
if not candidates:
	raise SystemExit("unknown encoder " + encoder)

fd = create_device("scrssh", 0x0001, [EV_KEY, EV_REL, EV_ABS, EV_SYN],
				   list(range(1, KEY_ADVERTISE_MAX + 1))
				   + [BTN_LEFT, BTN_RIGHT, BTN_MIDDLE],
				   [REL_WHEEL, REL_HWHEEL], [ABS_X, ABS_Y])

name, options = choose(candidates)
signal.signal(signal.SIGCHLD, lambda *_: os._exit(0))
process = subprocess.Popen(
	command(name, options, False), stdin=subprocess.DEVNULL)
os.close(1)

try:
	while True:
		record = read_exactly(WIRE_SIZE)
		typ, code, value = struct.unpack(FMT_WIRE, record)
		os.write(fd, struct.pack(FMT_INPUT_EVENT, 0, 0, typ, code, value))
except EOFError:
	pass
process.terminate()
process.wait()
