# vol

Audio and volume control. 0.1 is the command line: read and change the
default output or input, and follow it for bar. The OSD (0.2) and the mixer
(0.3) come next; the plan is in [docs/sound-roadmap.md](../docs/sound-roadmap.md).

vol talks to PipeWire through `pipewire-pulse` with libpulse. Changes go to
the default device the way pavucontrol's would, so WirePlumber saves and
restores them per route (speakers, headphones, HDMI) as usual.

## Usage

```text
vol [-s] [COMMAND]
vol [-s] --follow

  (none)                 print the level, as wpctl does: Volume: 0.30 [MUTED]
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

## Configuration

`~/.config/vol/vol.conf` is optional:

```ini
[volume]
step = 5     # percent for up and down without N, 1 to 100
max = 100    # where up stops, 1 to 150
```

Unknown keys and bad values are reported on stderr and the defaults kept.

## Build and install

Needs libpulse (from `libpulse`; PipeWire serves it through `pipewire-pulse`).

```sh
make
make install PREFIX=~/.local
```

dwl's volume keys and bar's `[volume]` defaults call `vol`, so it must be on
the session's `PATH`.

## Checks

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
