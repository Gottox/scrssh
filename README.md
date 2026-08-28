# scrssh

Viable TeamViewer/RustDesk alternative for Linux in under 1000 lines of C.
`scrcpy` for the linux admin.

![Bazzite](./bazzite.jpg)
![Raspberry Pi 2](./rpi2.jpg)

scrssh lets you interact with remote desktops and servers
over SSH. No remote configuration needed.

# Requirements

On the host:
1. a running ssh daemon
2. CAP_SYS_ADMIN on the target
3. a hardware H.264 encoder (see Encoders), or enough CPU for software
4. ffmpeg and python installed
5. kms and uinput available (both should be the default on modern linux systems)
6. a connected monitor or a hdmi dummy plug

On the client:

1. `ssh`
2. `scrssh`

```
scrssh [options] [--] <ssh arguments...>
```

# Encoders

The host picks the encoder itself. It captures a single frame with all of
them at once and keeps the first that works:

* `h264_vaapi`: AMD and Intel.
* `h264_nvenc`: NVIDIA with the proprietary driver.
* `h264_v4l2m2m`: Raspberry Pi 4 and CM4.
* `libx264`: software, for everything else including the Pi 5.

The dry runs are concurrent, so the search costs about one encoder start,
and `libx264` is accepted untested, so it never fails on a host with a
normal ffmpeg.
`-e` forces one encoder and skips the search:

```bash
scrssh -e libx264 user@example.com
```

The DRM device is not detected, so the rpi4 still needs the right `-d`:

```bash
scrssh -d /dev/dri/card1 user@raspberrypi sudo
```

Usage Examples
-----------

Connect to a remote server and view its console:

```bash
scrssh user@example.com
```

Connect to a remote server with a specific port. scrssh's own options stop
at the first non-option, so ssh's arguments go after `--`:

```bash
scrssh -- -p 2222 user@example.com
```

You can also tunnel through sudo if root is unavailable.
Please note, that it only works with passwordless sudo.

```bash
scrssh -- -p 2222 user@example.com sudo
```
