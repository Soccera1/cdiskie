# cdiskie

cdiskie is a C implementation of [udiskie](https://github.com/coldfix/udiskie) for Linux. It automounts removable media through UDisks2 and provides a tray menu, desktop notifications, and command-line tools for managing drives, encrypted volumes, and disk images.

## Quick start

Build from the repository root and run cdiskie as your desktop user.

```sh
make
build/cdiskie --smart-tray
```

The daemon mounts eligible devices at startup and as they appear. `--smart-tray` shows the tray icon when a device has an available action. Use `--tray` to keep it visible, `--no-automount` for manual control, or `--no-notify` to turn off desktop notifications.

## Build and install

You need a C11 compiler, Make, and pkg-config. The Makefile detects the optional desktop libraries at build time.

| Build feature | Development libraries |
| --- | --- |
| Daemon and command-line tools | GLib/GIO (`gio-unix-2.0`) and libyaml (`yaml-0.1`) |
| GTK tray, graphical passphrase prompt, and notifications | GTK 3 (`gtk+-3.0`) and libnotify (`libnotify`) |
| AppIndicator tray backend | Ayatana AppIndicator (`ayatana-appindicator3-0.1`) |

At runtime, cdiskie needs UDisks2 on the system D-Bus. UDisks2 handles device access and authorization for your session. A tray icon also needs a panel that supports its backend. Under Wayland, cdiskie selects AppIndicator by default when a tray is requested.

Use `make install` to install all four commands under `/usr/local/bin`. For a user-local installation, run the following and ensure `$HOME/.local/bin` is on your `PATH`.

```sh
make install PREFIX="$HOME/.local"
```

The Makefile also accepts `BINDIR` and `DESTDIR`. Run `make clean && make DESKTOP=0` for a headless build, or `make clean && make INDICATOR=0` for desktop support with the GTK tray backend.

## Common tasks

The manual commands work independently of the daemon. The examples use installed commands. Prefix them with `build/` to run them from the repository.

| Task | Command |
| --- | --- |
| List eligible devices and their mount points | `cdiskie-info --all --output '{device_file} {mount_path}'` |
| Mount a partition | `cdiskie-mount /dev/sdb1` |
| Unlock an encrypted volume and mount its filesystem | `cdiskie-mount --recursive /dev/sdb2` |
| Unmount a volume by its mount point | `cdiskie-umount "/run/media/$USER/My Drive"` |
| Unmount the partitions on a USB drive and power it off | `cdiskie-umount --force --detach /dev/sdb` |
| Unmount and eject optical media | `cdiskie-umount --eject /dev/sr0` |

Use `cdiskie-info --all` to find the right device path before mounting or ejecting. Manual commands also accept device symlinks, UUIDs, and UDisks2 object paths. Each command has its own `--help`.

### Disk images

Pass an image filename to set up a loop device. `--recursive` also mounts filesystems inside a partitioned image. Pass the same filename to unmount and detach the loop device. Use `--force` when you need to unmount child partitions.

```sh
cdiskie-mount --recursive ./disk.img
cdiskie-umount --force ./disk.img
```

On a new image, `cdiskie-mount --no-recursive ./disk.img` sets up the loop device and leaves mounting to a separate command.

## Configuration

Put your settings in `~/.config/cdiskie/config.yml`, or in `$XDG_CONFIG_HOME/cdiskie/config.yml` if you set `XDG_CONFIG_HOME`. cdiskie also looks for `config.yaml` and `config.json` in that directory, then checks the corresponding `udiskie` directory. Use `--config FILE` to select a file or `--no-config` to skip configuration. Command-line settings take precedence.

```yaml
program_options:
  tray: auto
  notify: true

device_config:
  # Keep this volume available for manual, read-only mounting.
  - id_uuid: "1234-ABCD"
    automount: false
    options: [ro, nodev]
  # Hide matching backup volumes from cdiskie.
  - id_label: "BACKUP-*"
    ignore: true

notifications:
  device_mounted: 5
  device_unmounted: false
```

Device rules can match several attributes at once, such as `id_uuid`, `id_label`, `id_type`, `device_file`, and `is_external`. String matches support shell-style wildcards, and a list of values matches any item in the list. For each setting, the first matching rule that supplies a value wins. `automount: false` leaves a volume available for manual mounting. `ignore: true` also hides it from the tray and excludes it from cdiskie operations. Notification timeouts are in seconds. Use `false` to disable an event or `-1` to use the notification server's default timeout.

### Encrypted volumes

The daemon prefers a graphical passphrase prompt. `cdiskie-mount` prefers the terminal. Set `keyfile` in a matching device rule to unlock with a keyfile. Set `password_cache` under `program_options` to cache successful passphrases in memory for a specified number of minutes. Caching is disabled by default.

### Hooks and prompts

Set `event_hook` or `password_prompt` under `program_options` to run a command. Both accept an argument list or a shell-quoted command string. cdiskie substitutes device attributes such as `{device_file}` and `{id_uuid}` inside each argument. Event hooks also receive `{event}`. An external password prompt returns the passphrase on standard output.

```yaml
program_options:
  event_hook: [logger, -t, cdiskie, "{event} {device_file}"]
```
