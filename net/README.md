# net

`net` is a Wi-Fi manager for a true-color terminal on Arch Linux. It uses
NetworkManager over the system D-Bus for discovery, scanning, connections,
radio controls, connection details, and saved Wi-Fi profiles.

## Requirements

- Build: `gcc`, `make`, and `pkgconf`, all included in Arch's `base-devel`.
  `systemd-libs` provides the sd-bus headers, library, and pkg-config metadata.
- Run: `systemd-libs`, a system D-Bus, and the `networkmanager` package with
  `NetworkManager.service` running. Use a UTF-8 locale to display non-ASCII SSIDs.
- Run as your usual user. If NetworkManager policy requires approval, a Polkit
  authentication agent in your session can handle it.

## Build and run

```sh
make
./net
```

`make clean` removes the binary. `make install` and `make uninstall` use
`/usr/local/bin` by default. Both accept `PREFIX`, `BINDIR`, and `DESTDIR`.
For a per-user install, use `make install PREFIX="$HOME/.local"` and use the same
prefix when uninstalling.

## Configuration

`net` looks for `$XDG_CONFIG_HOME/net/net.conf` first, then
`$HOME/.config/net/net.conf`. The XDG directory must be an absolute path.
Use `net -c /path/to/net.conf` to read a different file.

The included [net.conf](net.conf) lists the defaults. To create a user
configuration, copy it from this directory:

```sh
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/net"
mkdir -p "$config_dir"
cp net.conf "$config_dir/net.conf"
```

Appearance colors use `#RRGGBB`. Set `default_view` to `available` or
`saved`. A missing configuration uses the project’s `net.conf` bundled
at build time. Omitted settings use the built-in defaults.
Full-line comments start with `#` or `;`. Invalid settings are reported
before the TUI opens. Changes take effect the next time you run `net`.

## Wallpaper-based colors

Set the following in your net configuration to use the palette published by `wall`.

```ini
[theme]
source = wallpaper
```

The background, foreground, highlight, muted text, and both selection colors are loaded when `net` starts. Open a new `net` window after changing the palette or source.

The palette is read from `$XDG_CACHE_HOME/wall/colors.conf`, or `$HOME/.cache/wall/colors.conf` when `XDG_CACHE_HOME` is unset, empty, or relative. Set `source = manual` to restore the colors in your application configuration. Manual is the default if the section is omitted.

If the palette is missing or invalid, the app starts with its configured manual colors. Check that `wall` has `[colors] enabled = true`, that both apps use the same cache directory, and that `wall --colors` can print a complete palette.

## bar and dwl integration

Build and install `net` with `make` and `make install`. Use an install prefix
on the Wayland session's `PATH`, or use the full executable path in bar's
command. `make uninstall` removes the binary from the same prefix.

In bar's [configuration](../bar/README.md), add these options to the existing
`[network]` section:

```ini
click_enabled = true
command = deal --app-id net --columns 84 --rows 26 -e net
```

Build and install the updated bar, then use that build in your session. Once
it is running, `pkill -HUP -x bar` reloads changes to its configuration.
The launch command runs as an argument vector. Quote individual paths or
arguments that contain spaces. The default command sets deal's app ID to `net`
and requests 84 columns by 26 rows. The font comes from deal's configuration. See
the [deal command line](../deal/README.md#command-line).

The local dwl `config.h` has a floating rule for the exact ID `net`. It opens
on the current workspace, centered in the selected monitor's usable area,
with dwl's normal focus and floating size handling. In the dwl directory, rebuild with `make` and install
with `make install` using your usual prefix. Keep your current `config.h`
when rebuilding. The rebuilt compositor takes effect when you next start dwl.

## Keys

| Key | Action |
| --- | --- |
| Up / `k` | Move up |
| Down / `j` | Move down |
| Home / `g` | Select the first network |
| End / `G` | Select the last network |
| Tab | Switch between Available and Saved |
| `r` | Request a Wi-Fi scan |
| `w` | Turn Wi-Fi on or off |
| `i` | Show details for the selected network or saved profile |
| `h` | Connect to a hidden Wi-Fi network |
| Enter | Connect to the selected network or saved profile |
| `d` | In Available, disconnect the selected connected network after confirmation with `y` |
| `a` | In Saved, toggle auto-connect for the selected profile |
| `f` | In Saved, forget the selected profile after confirmation with `y` |
| `q` / Escape | Quit from the list, or use Escape to close details or cancel a prompt |

Available combines scan results across wireless interfaces. For each SSID and
security mode, it shows the connected access point or the strongest one in
range. The Saved column marks networks with a compatible persistent profile.
Access points with unknown SSIDs remain separate.

Saved shows persistent Wi-Fi profiles, including separate rows for profiles
with the same SSID. Each view remembers its own selection and scroll position.
Saved shows auto-connect, whether that exact profile is connected, and whether
a compatible access point is in range. Enter uses the selected profile.

Auto-connect changes are saved by NetworkManager, which preserves the rest of
the profile and its credentials. Forgetting targets the selected profile and
may interrupt the connection if that profile is active. At the confirmation
prompt, Enter, `n`, or Escape cancels.

Open, WPA/WPA2 Personal, and WPA3 Personal networks are supported. Enter uses
a compatible profile when NetworkManager has one. When a password is needed,
type it in the masked field, use Backspace to edit it, and press Enter to submit
or Escape to cancel. WPA/WPA2 takes 8–63 printable ASCII characters or 64
hexadecimal digits. WPA3 SAE accepts nonempty UTF-8 passwords up to 255 bytes.

## Wi-Fi controls and details

Press `w` to enable software-disabled Wi-Fi. When the hardware is blocked, use
the wireless switch first. If Wi-Fi is connected, turning it off requires
confirmation with `y`. The network list refreshes when Wi-Fi comes back on.

Press `i` to inspect the selected network or saved profile. The details show
the information NetworkManager has for that selection, including live IP
addresses, gateways, and DNS servers for an active connection. Use the usual
movement keys to scroll and Escape to return to the list.

Press `h` and enter the SSID (1–32 bytes). Press Enter to continue, then use
`1` for Open, `2` for WPA/WPA2 Personal, or `3` for WPA3 Personal. Compatible
saved profiles are reused. New profiles are marked hidden in NetworkManager.
The same masked password prompt is used when a password is needed.

New profiles are saved by NetworkManager for the current user. NetworkManager
applies the existing profile's credential storage policy when it requests a
password during activation. `net` has no credential store. For details, see the
[NetworkManager D-Bus API](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html)
and [Wi-Fi security settings](https://networkmanager.dev/docs/api/latest/settings-802-11-wireless-security.html).
