# scrssh

Viable TeamViewer/RustDesk alternative for Linux in under 1000 lines of C.
`scrcpy` for the linux admin.

Works on Bazzite:

![Bazzite](./bazzite.jpg)

Works even on the Raspberry Pi 2:

![Raspberry Pi 2](./rpi2.jpg)

scrssh lets you interact with remote desktops and servers
over SSH. No remote setup needed.

```
usage: scrssh [options] [--] <ssh arguments...>

options:
  -s         run the agent with `sudo`
  -d <PATH>  DRM device to capture      [default: /dev/dri/card0]
  -C <N>     capture a specific CRTC
  -P <N>     capture a specific plane
  -f <N>     capture frame rate         [default: 30]
  -B <RATE>  capped bitrate             [default: 500K]
  -e <NAME>  force an encoder           [default: ask the host]
             h264_vaapi, h264_nvenc, h264_v4l2m2m, libx264
  -h         show this help
```

# Requirements

On the host:
1. `sshd` running
2. `CAP_SYS_ADMIN` aka `root`
3. ffmpeg and python installed
4. a screen or a HDMI dummy plug connected

On the client:

1. `ssh`
2. `scrssh`

# Encoders

The host picks the encoder itself. It captures a single frame with all of
them at once and keeps the first that works:

* `h264_vaapi`: AMD and Intel
* `h264_nvenc`: NVIDIA
* `h264_v4l2m2m`: Raspberry Pi 
* `libx264`: Software

`-e` forces one encoder and skips the search:

```bash
scrssh -e libx264 user@example.com
```

Usage Examples
-----------

Connect to a remote server and view its console:

```bash
scrssh user@example.com
```

scrssh works with all `ssh` command line options, so it's easy to use custom ports
or a jump hosts:

```bash
scrssh -- -p 2222 -J user@jumphost.com user@example.com
```

If root is unavailable, `-s` runs the agent under `sudo`:

```bash
scrssh -s -- -p 2222 user@example.com
```

# Troubleshooting

```
[in#0 @ 0x5566dacb3f80] No handle set on framebuffer: maybe you need some additional capabilities?
[in#0 @ 0x5566dacb3c80] Error opening input: Invalid argument
Error opening input file -.
Error opening input files: Invalid argument
[mpegts @ 0x7f939c010940] Could not detect TS packet size, defaulting to non-FEC/DVHS
the remote stream contains no video
```

The remote host doesn not have required permissions. Either log in as root or add the `-s`
flag to scrcpy to enable `sudo` mode.

```
[in#0 @ 0x560f3e28ff80] Framebuffer pixel format 30334241 is not a known supported format.
[in#0 @ 0x560f3e28fc80] Error opening input: Invalid argument
```

Disable HDR on the remote host.
