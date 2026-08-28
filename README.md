# scrssh

Viable TeamViewer/RustDesk alternative for Linux in under 1000 lines of C.
`scrcpy` for the linux admin.

Works on Bazzite:
![Bazzite](./bazzite.jpg)

Works even on the Raspberry Pi 2:

![Raspberry Pi 2](./rpi2.jpg)

scrssh lets you interact with remote desktops and servers
over SSH. No remote configuration needed.

# Requirements

On the host:
1. `sshd` running
2. `CAP_SYS_ADMIN` aka `root`
3. ffmpeg and python installed
4. a screen or a HDMI dummy plug connected

On the client:

1. `ssh`
2. `scrssh`

```
scrssh [options] [--] <ssh arguments...>
```

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

# Troubleshooting

```
sudo: a terminal is required to read the password; either use the -S option to read from standard input or configure an askpass helper
sudo: a password is required
[mpegts @ 0x7f1ed4010940] Could not detect TS packet size, defaulting to non-FEC/DVHS
the remote stream contains no video
```

Currently scrssh can't handle passwords with sudo. either add `NOPASSWD:` to your /etc/sudoers for the user, or login as root directly.

```
PermissionError: [Errno 13] Permission denied: '/dev/uinput'
```

The remote host doesn not have permissions for uinput. scrssh needs to *write* to `/dev/uinput`. It also needs `CAP_SYS_ADMIN` to capture the screen.

```
[in#0 @ 0x560f3e28ff80] Framebuffer pixel format 30334241 is not a known supported format.
[in#0 @ 0x560f3e28fc80] Error opening input: Invalid argument
```

Disable HDR on the remote host.
