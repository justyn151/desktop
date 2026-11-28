# vol

Audio and volume control: a command line that reads and changes the default
output or input and follows it for bar, and a mixer window for every output,
input, playing stream and sound card. The popup comes with `notify`; config and the
bar click come in 0.4. The plan is in
[docs/sound-roadmap.md](../docs/sound-roadmap.md).

vol talks to PipeWire through `pipewire-pulse` with libpulse. Changes go to
the default device the way pavucontrol's would, so WirePlumber saves and
restores them per route (speakers, headphones, HDMI) as usual.

## Usage

```text
vol [-s] [COMMAND]
vol [-s] --follow

  (none)                 in a terminal, the same as mixer; in a script or
                         pipe, the same as get
  mixer                  open the mixer window (-s opens it on Input)
  get                    print the level, as wpctl does: Volume: 0.30 [MUTED]
  up [N] | down [N]      change it by N percent (default: [volume] step, 5)
  set N                  set it to N percent (0 to 150)
  mute [on|off|toggle]   mute or unmute (default: toggle)

  -s, --source           the default input (the mic) instead of the output
  -f, --follow           print the level again on every change, until killed
```

`N` may end in `%`. `up` stops at `[volume] max` and never lowers a level
that is already above it (after `set 120`, say). `down` stops at 0. Changing
the level scales every channel together, so the balance stays as it is.
Raising the volume doesn't unmute.

`--follow` prints the current line, then a new one whenever the default
device's level or mute changes or another device becomes the default.
Identical lines are skipped. With no default device it prints `No output` (or
`No input`). If the server goes away, it keeps trying every second and prints
the level again once it's back. bar's `[volume] command` runs it.

Exit status: 0 on success, 1 when the sound server can't be reached within
5 seconds or there is no default device, 2 for a usage error.

## Mixer

```sh
vol mixer
```

A Wayland window (app ID `vol`, title "Sound"), drawn like files: tabs
across the top, a row per device or stream. Four views, switched with a click
on their tab, Tab, Shift+Tab, Ctrl+Page Up/Down, or `1` to `4`:

| View | Rows | Click / Enter |
| --- | --- | --- |
| Output | Every output; the filled dot is the default | The dot makes it the default |
| Input | Every input except outputs' monitors | The dot makes it the default |
| Playback | Each application's stream, what it plays, and where | The output on the right moves it to another |
| Devices | Each sound card and its profile | The profile box picks one (HDMI, Bluetooth A2DP or HFP with its codec, Off) |

Each row with a level has a speaker (or mic) button that mutes, a slider and
the percentage. Choices open as a list over the window; click one, or click
outside it to close it.

| Key | Action |
| --- | --- |
| Arrows, j/k | Move |
| Left/Right, h/l, -/+ | Level by `[volume] step`; up stops at `max` |
| m | Mute or unmute |
| Enter, d | As in the table above |
| Home/End, g/G | First or last row |
| Escape | Back out of a choice, or quit |
| q, Ctrl+Q, Ctrl+W | Quit |

With the mouse: click or drag on a slider to set the level, use the wheel
over a slider or mute button to step it, and the wheel anywhere else to
scroll. Middle click on a row mutes it, as on bar.

On Output and Input, what's playing or being picked up shows as a thin line
under the slider, in the highlight color: on Input
that's the mic, so speak and watch it move. The mic's meter keeps the mic
open while the Input view is on screen; outputs' meters don't keep an idle
output awake. Everything follows the server live: streams come and go,
devices appear when headphones connect, a card's profile change adds or
removes its outputs and inputs. If the server goes away the mixer says so
and reconnects every second.

The colors are the wallpaper's palette when wall has made one, and change
while the mixer is open; the defaults otherwise.

## Configuration

`~/.config/vol/vol.conf` is optional:

```ini
[volume]
step = 5     # percent for up and down without N, 1 to 100
max = 100    # where up stops, 1 to 150
```

Unknown keys and bad values are reported on stderr and the defaults kept.

## Build and install

Needs libpulse (from `libpulse`; PipeWire serves it through `pipewire-pulse`),
wayland-client, xkbcommon, freetype2 and fontconfig, wayland-scanner and
wayland-protocols to build. The mute icons come from Symbols Nerd Font
through fontconfig's fallback.

```sh
make
make install PREFIX=~/.local
```

dwl's volume keys and bar's `[volume]` defaults call `vol`, so it must be on
the session's `PATH`.

## Checks

Mixer (0.3), checked with two temporary null outputs and noise played into
one, driven by a virtual pointer:

- [x] Output lists every output with the default marked; a click on a
      slider (50%), a drag (26%), the wheel (+10%) and the mute button
      change the right output
- [x] The meter shows the noise; Input shows the mic and its meter
- [x] Playback shows the stream with its output; the output's dropdown,
      then the other output, moves it while it plays
- [x] Devices shows both cards; the HDMI profiles show as unavailable; a
      click outside closes the choice
- [x] SIGTERM closes it with no meter streams left on the server
- [ ] Keyboard: Tab, 1–4, j/k, h/l, m, Enter, Escape (no key injection here)
- [x] `vol | cat`, `vol get` and `--follow` behave as before
- [ ] A browser tab's stream moved to headphones stays there
- [ ] Switching a headset to HFP adds its mic under Input
- [ ] Removing the default output moves its streams to the next one

Command line:

- [x] Each command against `wpctl get-volume`: up, down, up N, set, the max
      stop, set above max, mute on/off/toggle, and the same with `--source`
- [x] Exit 1 with one message when no server answers
      (`PULSE_SERVER=unix:/nonexistent vol`), 2 for `vol bogus`
- [x] `--follow` prints every change once, and again after
      `systemctl --user restart pipewire pipewire-pulse wireplumber`
- [x] bar runs `vol --follow` and nothing is left behind when bar exits
- [ ] After a dwl restart: the volume keys, held keys, Mic Mute
- [ ] Scrolling and middle-clicking bar's volume
- [ ] `--follow` switches to headphones when they become the default
