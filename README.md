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
3. AMD or Intel graphics
4. ffmpeg and python installed
5. kms and uinput enabled (both should be the default on modern linux systems)
6. a connected monitor or a hdmi dummy plug

On the clinet:

1. `ssh`
2. `scrssh`

```
scrssh [options] [--] <ssh arguments...>
```

Usage Examples
-----------

Connect to a remote server and view its console:

```bash
scrssh user@example.com
```

Connect to a remote server with a specific port:

```bash
scrssh -p 2222 user@example.com
```

You can also tunnel through sudo if root is unavailable.
Please note, that it only works with passwordless sudo.

```bash
scrssh -p 2222 user@example.com sudo
```
