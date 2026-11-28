# blue

A Bluetooth manager for the terminal, over BlueZ's D-Bus API. It looks and
works like `net`. 0.1 lists the devices the adapter already knows, connects
and disconnects them, and turns Bluetooth on and off. Scanning and pairing
come in 0.2, the bar module and dwl rule in 0.3; the plan is in
[docs/sound-roadmap.md](../docs/sound-roadmap.md).

## Usage

```sh
deal --app-id blue --columns 84 --rows 26 -e blue
```

| Key | Action |
| --- | --- |
| Enter | Connect the selected device, or disconnect it when it's connected |
| p | Turn Bluetooth on or off |
| Arrows, j/k | Move |
| Home/End, g/G | First or last device |
| q, Escape | Quit |

The second line shows the adapter's state: on with its name and how many
devices are connected, off, turning on or off, blocked by rfkill, missing,
or BlueZ not running. Everything is live from BlueZ's signals: devices that
connect from their side, batteries, a restarted `bluetooth.service`.

The list holds the devices the adapter knows (paired, bonded, trusted or
connected), connected first, then by name. Each row has the name, a type
from BlueZ's icon (Headphones, Headset, Speaker, Keyboard, Phone...), the
battery when the device reports one, and the state: Connecting, Leaving,
Connected, or Failed after an attempt that didn't work. The reason goes on
the line above the hints, for example "Couldn't connect to WH-1000XM4: the
device is off or out of range". Connecting waits up to 45 seconds.

Until 0.2, pair new devices with `bluetoothctl` (`agent on`, `scan on`,
`pair`, `trust`, `connect`); they show up in blue as soon as they're paired.

## Configuration

`~/.config/blue/blue.conf` is optional, with the same keys as net's:

```ini
[appearance]
background = #353535
foreground = #c5ad81
highlight = #ead4aa
muted = #887d69
selection_background = #c5ad81
selection_foreground = #353535

[theme]
source = wallpaper   # or manual for the colors above
```

With `source = wallpaper` (the default), the colors come from wall's
palette and change while blue is open when the wallpaper does. `-c PATH`
reads another file.

## Build and install

Needs libsystemd for sd-bus.

```sh
make
make install PREFIX=~/.local
```

## Checks

- [x] Power: `p` turns the adapter off and on, and the status line follows
      through "turning off..." to "off" and back
- [x] No devices: the empty list says to pair with bluetoothctl
- [x] A device that becomes known shows up with its type, and goes away
      live when it no longer is (checked by trusting and untrusting a
      nearby TV with bluetoothctl)
- [x] In deal: palette colors, the selection bar, the headings
- [ ] Connect and disconnect real headphones; the battery matches the phone's
- [ ] Connecting to headphones that are off fails with "off or out of range"
- [ ] `sudo systemctl restart bluetooth` while open: "BlueZ isn't running",
      then the list comes back
- [ ] `rfkill block bluetooth`: "Bluetooth blocked", and `p` says to unblock
