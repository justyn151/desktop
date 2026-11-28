# blue

A Bluetooth manager for the terminal, over BlueZ's D-Bus API. It looks and
works like `net`: it lists the devices the adapter knows and the ones nearby,
pairs, connects, trusts, renames and removes them, and turns Bluetooth on and
off. While it runs it is BlueZ's pairing agent, so codes are asked for and
shown here. The bar module and dwl rule come in 0.3; the plan is in
[docs/sound-roadmap.md](../docs/sound-roadmap.md).

## Usage

```sh
deal --app-id blue --columns 84 --rows 26 -e blue
```

| Key | Action |
| --- | --- |
| Enter | Pair a nearby device, or connect or disconnect a known one |
| s | Search for nearby devices for a minute, or stop |
| t | Trust or untrust: a trusted device may connect on its own |
| r | Rename (an empty name gives back the one the device announces) |
| x | Remove the device; it will need pairing again |
| v | Make this computer visible to other devices for 3 minutes, or hide it |
| p | Turn Bluetooth on or off |
| Arrows, j/k | Move |
| Home/End, g/G | First or last device |
| q, Escape | Quit |

The second line shows the adapter's state: on with its name and how many
devices are connected, off, turning on or off, blocked by rfkill, missing,
or BlueZ not running. Everything is live from BlueZ's signals: devices that
connect from their side, batteries, a restarted `bluetooth.service`.

The list holds the devices the adapter knows (paired, bonded, trusted or
connected), connected first, then by name. Below them, under Nearby, are the
devices found by searching, strongest signal first; ones without a name are
counted in the heading and left out. They stay a little while after the
search ends, until BlueZ forgets them. Each row has the name, a type
from BlueZ's icon (Headphones, Headset, Speaker, Keyboard, Phone...), the
battery when the device reports one, and the state: Pairing, Connecting,
Leaving, Connected, Failed after an attempt that didn't work, Not trusted, or
for nearby devices the signal (Strong, Good, Weak). The reason goes on
the line above the hints, for example "Couldn't connect to WH-1000XM4: the
device is off or out of range". Connecting waits up to 45 seconds.

## Pairing

Put the device in pairing mode, press `s`, select it under Nearby and press
Enter. Depending on the device, blue then:

- pairs without asking (most headphones and speakers),
- asks whether the device shows the same six digits (phones): `y` or `n`,
- shows six digits or a PIN to type on the device (keyboards), counting the
  keys as they're typed; Escape cancels,
- asks for a PIN (older devices; often 0000) or a passkey the device shows.

After pairing, blue trusts the device and connects it. While blue is open
it is the default agent, so pairing that a phone starts, and services an
untrusted device wants, are asked about here as well. Only BlueZ may ask:
the D-Bus policy refuses other callers, and blue checks the sender too.

Searching makes the adapter pairable if it wasn't, and puts that back when
the search stops or blue quits. Searching ends after a minute, or with `s`.

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

0.2:

- [x] `s` searches: "searching" in the status, Nearby fills with named
      devices strongest first, unnamed ones counted; `s` again stops
- [x] Rename sets the alias, and an empty name gives back the device's own
- [x] `x` asks first; Escape keeps the device
- [x] A call to the agent from anything but BlueZ is refused
- [ ] Pair headphones (no code), a phone (compare digits) and a keyboard
      (type the code) if there is one; each ends trusted and connected
- [ ] Cancel a pairing from the device's side: "Pairing canceled"
- [ ] Remove a paired device and pair it again
- [ ] `v`: the phone sees this computer, and doesn't after `v` again

0.1:

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
