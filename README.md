# scrssh

Viable TeamViewer/RustDesk alternative for Linux.
`scrcpy` for the linux admin.

scrssh lets you interact with remote desktops and servers
over SSH. No remote daemon besides ssh needed. No need
to have a graphical environment on the remote machine.

# Requirements

On the host:
1. a running ssh daemon
2. CAP_SYS_ADMIN on the target
3. a hardware H.264 encoder (see Encoders), or enough CPU for software
4. ffmpeg and python installed
5. kms and uinput enabled (both should be the default on modern linux systems)
6. a connected monitor or a hdmi dummy plug

On the client:

1. `ssh`
2. `scrssh`

```
scrssh [options] [--] <ssh arguments...>
```

# Encoders

`-e` picks the ffmpeg encoder:

* `h264_vaapi`: AMD and Intel. The default.
* `h264_nvenc`: NVIDIA with the proprietary driver.
* `h264_v4l2m2m`: Raspberry Pi 4 and CM4.
* `libx264`: software, for everything else including the Pi 5.

For example on the rpi4:

```bash
scrssh -d /dev/dri/card1 -e h264_v4l2m2m user@raspberrypi sudo
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
