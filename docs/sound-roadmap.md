# Bluetooth and audio roadmap

Plan for Bluetooth and volume control, from the terminal stopgaps to two apps: `vol` (audio, project 13) and `blue` (Bluetooth, project 30). It expands items 1 and 2 of "Next" in [PLAN.md](../PLAN.md) and the Volume keys and Bluetooth rows of Phase 6.

## Starting point (checked 2026-11-28)

| Piece | State |
|---|---|
| `bluez`, `bluez-utils` 5.88 | Installed. `bluetooth.service` is **already enabled and active**, controller `hci0` powered, not rfkill-blocked, not pairable or discoverable |
| PipeWire 1.6.11, WirePlumber 0.5.19, `pipewire-pulse`, `pipewire-alsa` | Running. Built-in Audio Analog Stereo is the default sink (volume 0.30); the NVIDIA HDMI card is there too |
| PipeWire's Bluetooth plugin (`spa-0.2/bluez5`) | Loaded. The adapter already advertises Audio Sink, Handsfree Audio Gateway and A/V Remote Control, so headphones will come up as PipeWire nodes once paired |
| `libpulse` 17 | Installed (it's where `pactl` comes from). PipeWire serves it through `pipewire-pulse`, socket-activated |
| `rtkit` | Missing. PipeWire and WirePlumber log RTKit errors at startup and run without realtime priority. Bluetooth audio is the first place that shows up as crackles |
| UPower | Missing. WirePlumber wants it for headset battery levels. Optional: BlueZ's `Battery1` gives the same thing |
| dwl | No audio keys bound, only the commented `volume-up_EXAMPLE`. Bindings repeat when held, so held volume keys work without anything extra |
| bar | Workspaces, network (sd-bus to NetworkManager), clock. No volume or Bluetooth module |

### Decisions

- **Two apps, split by what they own.** `blue` owns the radio: power, scanning, pairing, trust, connect. `vol` owns sound: devices, levels, streams, and the card profile, which includes switching headphones between A2DP (music) and HFP (calls with the mic). A Bluetooth headset therefore shows up in both, the way it does in BlueZ and PipeWire themselves.
- **`blue` is a TUI over BlueZ's D-Bus API with sd-bus**, the same shape as `net` (a floating deal window opened from bar). That means no new library. It's separate from net and not a tab inside it, because net is NetworkManager only and already 4,600 lines.
- **`vol` talks to PipeWire through libpulse, not libpipewire.** Setting a hardware device's volume correctly means changing its active Route, which WirePlumber's mixer API does and raw libpipewire leaves to the client. pipewire-pulse already does that work behind a small, stable, documented API with event subscription. libpulse is one library and it's already installed. `pa_mainloop` gets its own fd so it fits into the app's poll loop. Revisit if libpulse support gets dropped.
- **The bar stays free of libpulse.** bar's volume module runs a command and shows the last line it printed. During the stopgap that command is a shell loop over `pactl subscribe`; from `vol` 0.1 on it's `vol --follow`. bar's Bluetooth module talks to BlueZ over sd-bus directly, the way the network module talks to NetworkManager.
- **The volume popup (OSD) belongs to `notify`, not `vol`** (decided 2026-11-28). `notify` draws popups anyway, and brightness keys (Phase 6) want the same thing, so one daemon draws all of them. vol 0.2 is skipped; see below.

### What desk gains

| desk piece | Needed by | Phase 3 step it pre-empts |
|---|---|---|
| Layer-shell surface setup (anchor, margins, no input, exclusive zone 0) | Nothing here now that the OSD moved to `notify`; `notify` will need it | Step 4 (bar, run and wall still have their own) |
| Terminal raw mode, alternate screen, key and mouse input decoding (`-DDESK_TERM`), if blue and vol share it without strain | blue 0.1 | None yet. net and mark each have their own; blue is the third copy. vol's mixer became a window, so it needs none |

---

## Stage 0: Stopgaps (now, before the browser)

No new app. This is the part of PLAN's "Next" items 1 and 2 that makes the laptop usable today.

### 0.1 Bluetooth from the terminal

1. `sudo pacman -S rtkit` and reboot (or restart the user's `pipewire` and `wireplumber` units). The RTKit errors should be gone from `journalctl --user -u wireplumber -b`.
2. Check `/etc/bluetooth/main.conf`: `AutoEnable` defaults to true in 5.88, so the controller powers on at boot. Leave the file alone unless it doesn't.
3. Pair a pair of headphones:
   ```text
   bluetoothctl
   agent on
   default-agent
   scan on          # wait for the device, note its address
   pair XX:XX:...
   trust XX:XX:...  # so it reconnects on its own
   connect XX:XX:...
   scan off
   ```
4. In `wpctl status` the headphones are a sink and WirePlumber made them the default. Play something and check the sound comes out of them, then disconnect and check it falls back to the built-in speakers.
5. Profiles: `wpctl status` lists the card, `wpctl set-profile <card-id> <index>` switches A2DP and HFP. Check that the mic works in HFP (`pw-record` a few seconds, then `pw-play` it).
6. Reconnecting: turn the headphones off and on, suspend and resume, reboot. They should connect again each time without touching bluetoothctl.

Done when headphones work after a reboot without bluetoothctl. Write any surprises in a "Bluetooth notes" section at the end of this file.

### 0.2 Volume keys in dwl

In `dwl/config.h` (and `config.def.h`, which tracks the real layout), with modifier 0:

| Key | Command |
|---|---|
| `XKB_KEY_XF86AudioRaiseVolume` | `wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+` |
| `XKB_KEY_XF86AudioLowerVolume` | `wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-` |
| `XKB_KEY_XF86AudioMute` | `wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle` |
| `XKB_KEY_XF86AudioMicMute` | `wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle` |

Use argv arrays like `shotcmd`, not `SHCMD`, so no shell is started per press. Replace the `volume-up_EXAMPLE` comment. `-l 1.0` stops at 100%. Play, Next and Previous wait for MPRIS (see "After").

Checks: each key works and holding Raise goes up smoothly to 100% and stops. The keys follow the default sink, so with headphones connected they change the headphones. Check the ASUS keyboard sends these keysyms at all (`wev`, or dwl's log); some laptops need Fn.

### 0.3 Volume in bar

A `[volume]` module in bar that shows a command's output instead of talking to audio itself:

| Key | Meaning |
|---|---|
| `enabled` | `true` or `false` |
| `command` | Started once at startup; every line it prints replaces the text. Restarted with backoff if it exits |
| `click`, `middle_click`, `scroll_up`, `scroll_down` | Commands run on those pointer actions |
| `muted_color` | Text color when the line ends in `[MUTED]` (wpctl's wording; `vol --follow` prints the same) |
| `custom_text` | The label before the level, `Vol` by default |

Stopgap `command`:

```sh
sh -c 'v() { wpctl get-volume @DEFAULT_AUDIO_SINK@; }; v; pactl subscribe | grep --line-buffered -E "on (sink|server)" | while read -r _; do v; done'
```

bar turns `Volume: 0.30` into `Vol 30%` and a muted line into `Vol muted` (bar's font has no Nerd Font fallback, so text like the network's `WiFi 82%`, not an icon). Scrolling runs the same `wpctl` commands as the keys, middle click mutes, and a left click does nothing until `vol` 0.3 has a mixer.

Checks: the level updates immediately for keys, for scrolling the bar, and for changes made in another tool. Muting changes the color. Killing the command restarts it. Changing the default sink (headphones connecting) updates the text.

Done when the volume keys work and the bar shows the level. Then PLAN moves on to the browser.

---

## vol

Audio and volume control, project 13. One `vol.c` and `vol.h` on desk Layer 0 (INI config, palette) plus libpulse. It has no Wayland surface of its own: the popup is `notify`'s.

### 0.1: Command line

```text
vol                       print the default sink: "Volume: 0.30" or "Volume: 0.30 [MUTED]"
vol up [N] | down [N]     change the default sink by N percent (default from config, 5)
vol set N                 set it to N percent
vol mute [on|off|toggle]  default toggle
vol --source ...          the same commands for the default source (the mic)
vol --follow              print the line again on every change to the default sink or which sink is default; runs until killed
```

Exit 1 if the server can't be reached, 2 for a usage error. `up` stops at `[volume] max` (100 by default; up to 150 if set). Changes are made on the default sink through pipewire-pulse, which keeps the Route volume WirePlumber saves and restores.

Work: connect with `pa_context` on a `pa_mainloop`, look up the default sink with `pa_context_get_server_info`, change it, wait for the operation, exit. `--follow` subscribes to sink and server events and prints only when the text changes, so bar never redraws for nothing.

Then replace the stopgaps: dwl's keys run `vol up`, `vol down`, `vol mute` and `vol --source mute`, and bar's `command` becomes `vol --follow`. The bar checks from 0.3 must still pass.

Checks: each command against wpctl's view of the same sink; `--follow` survives `systemctl --user restart pipewire pipewire-pulse wireplumber` (reconnects, prints the current level again) and goes on printing when headphones become the default.

### 0.2: The OSD (skipped, moved to notify)

Skipped on 2026-11-28: the popup is built with `notify` instead, so there is one daemon for every popup. Until then the bar's `Vol 30%` is the feedback for the keys.

What carries over to notify's roadmap:

- A popup at the bottom middle with a level bar and the percent, faded out after about 1.5 s. Muted is shown in the muted color. A new change replaces the open popup instead of stacking another (`x-canonical-private-synchronous` / `synchronous` hints, `int:value` for the level, as other volume OSDs send them).
- It shows on any change to the default sink: keys, bar scrolling, other tools. Either vol sends the notification (`vol up --notify`) or a small `vol --follow`-like watcher does it so changes from other tools show too. Decide in notify's roadmap.
- The device name when the default sink changes (headphones connecting).
- No input and an exclusive zone of 0, on the focused output, above full-screen windows. Palette colors.
- The same popup serves the brightness keys in Phase 6.

### 0.3: The mixer

`vol mixer` opens the mixer, a Wayland window built on desk like files (app ID `vol`, opened from bar's click). Four views, switched with Tab or by clicking their tab:

| View | Rows | Actions |
|---|---|---|
| Output | Every sink, the default marked | Set default, level, mute; a level meter while something plays |
| Input | Every source except monitors | Set default, level, mute, a live level meter so you can check the mic |
| Playback | Each application's stream | Level, mute, move to another output |
| Devices | Each card with its profiles | Pick a profile: Bluetooth A2DP or HFP (codec named), HDMI out, Off |

Keys: arrows to move, Left/Right or `h`/`l` for level, `m` mute, Enter or `d` default, `q` to quit. Mouse: sliders to click or drag, a mute button, a dot for the default, dropdowns for a stream's output and a card's profile, the wheel over a slider steps it. Everything is live: streams come and go, devices appear when headphones connect.

Checks: a browser tab's stream shows up, can be moved to the headphones while it plays, and stays there; switching a headset to HFP makes its mic appear under Input; removing the default device moves its streams to the next one.

### 0.4: Config and integration

- `~/.config/vol/vol.conf`: `[volume] step`, `max`; `[osd] enabled`, `position`, `timeout`, `width`; `[mixer] start_view`, `show_monitors`.
- Palette colors, live, like mark.
- `vol.desktop`, the dwl float rule for app ID `vol`, and bar's `click = vol mixer`. (Done; see the as-built notes.)

### 1.0

The checklists above in a fresh session, a week of use (calls with a headset, HDMI out, suspend with headphones connected), README, tag `vol-1.0.0`.

---

## blue

Bluetooth manager, project 30: one `blue.c` and `blue.h`, a TUI over BlueZ's D-Bus API (`org.bluez`) with sd-bus, desk Layer 0 only. Built the same way as net, opened from bar in a floating deal window with app ID `blue`.

### 0.1: Known devices

- One adapter (`org.bluez.Adapter1`): show its name and whether it's powered; `p` turns power on and off.
- The device list from `org.freedesktop.DBus.ObjectManager.GetManagedObjects`, kept live with `InterfacesAdded`, `InterfacesRemoved` and `PropertiesChanged`, so nothing polls.
- Each row: an icon from the device's `Icon` (headset, phone, keyboard, mouse...), name or alias, connected or not, battery from `org.bluez.Battery1` when the device has it.
- Enter connects or disconnects (`Device1.Connect` and `Disconnect`, asynchronous, with a "connecting" state and BlueZ's error shown on the row if it fails, for example `br-connection-page-timeout` when the device is off).
- rfkill blocked or no adapter at all: one clear line saying so instead of an empty list.

Checks against bluetoothctl's view: the headphones paired in Stage 0 show up, connect, disconnect, and the battery matches what the phone shows for them.

### 0.2: Scanning and pairing

- `s` starts and stops discovery (`StartDiscovery`, with a `SetDiscoveryFilter` that leaves out unnamed LE noise unless asked). New devices appear in a second section below the known ones as they're found, with their signal strength.
- Pairing: blue registers itself as the default agent (`org.bluez.AgentManager1.RegisterAgent` with `KeyboardDisplay`, then `RequestDefaultAgent`) and implements `org.bluez.Agent1`: `RequestConfirmation` (the "does this passkey match?" dialog), `DisplayPasskey` and `DisplayPinCode` (for keyboards), `RequestPinCode` and `RequestPasskey` (a text field), `AuthorizeService`, `Cancel`.
- After pairing it marks the device trusted and connects, the same three steps as Stage 0.
- `t` toggles trusted, `x` removes the device (asks first), `r` renames it (`Alias`).
- The adapter is made pairable only while blue scans, and discoverable only on request.

Checks: pair headphones (Just Works), a phone (numeric comparison), and a keyboard if there's one (passkey shown); cancel a pairing from the other side; removing a device and pairing it again works.

### 0.3: bar and integration

- A `[bluetooth]` module in bar, over sd-bus on the system bus like the network module: hidden when no adapter, an off icon when powered off, an on icon when idle, the connected device's name and battery when one is connected (the first one when several are). Click runs `deal --app-id blue -e blue`.
- `~/.config/blue/blue.conf`: `[scan] show_unnamed`, `timeout`; `[list] show_battery`, `sort`; palette colors, live.
- `blue.desktop` and the dwl float rule for app ID `blue`.

### 1.0

The checklists in a fresh session, a week of use, the agent's input fuzzed (D-Bus messages with odd passkeys, very long names, devices that disappear mid-pairing), README, tag `blue-1.0.0`.

---

## Order

1. Stage 0 now: 0.1 Bluetooth from the terminal, 0.2 volume keys, 0.3 the bar module. Then the browser.
2. `vol` 0.1 replaces the stopgaps (done). 0.2, the OSD, is skipped and moves to `notify`.
3. `blue` 0.1 to 0.3: until then, bluetoothctl works for the rare new pairing.
4. `vol` 0.3 and 0.4, the mixer, then both 1.0s.

## After (unscheduled)

- **Media keys and headset buttons:** BlueZ turns AVRCP buttons into Play, Pause, Next and Previous key events, and so do the laptop's keys. Binding them needs an MPRIS client (`org.mpris.MediaPlayer2.*` on the session bus over sd-bus). That's a few lines in `vol` (`vol play-pause`, `vol next`), and `music` is the first player of our own to expose it.
- **Bluetooth audio auto-switching:** WirePlumber can switch a headset to HFP when an app opens the mic and back afterwards (`bluetooth.autoswitch-to-headset-profile`). Decide after a week of calls whether to keep its default.
- **Absolute volume over AVRCP** for headphones that support it, so `vol` changes the headphones' own volume. PipeWire does this already. Check it behaves and document it.
- **notify:** the volume popup (vol 0.2's plan, above), "Headphones connected, 80% battery" and low-battery warnings.

## As built

- **Stage 0.2:** the four keys in `dwl/config.h` and `config.def.h` as argv arrays (`volupcmd` and the rest), documented in dwl's key table. The unused `volume-up_EXAMPLE` axis comment is gone.
- **Stage 0.3:** bar's `[volume]` module. The command runs in its own process group (`POSIX_SPAWN_SETPGROUP`) so stopping it with `kill(-pid)` takes the `pactl | grep | while` pipeline with it. SIGCHLD now reaps every child with `waitpid(-1)`, since click and scroll commands aren't tracked. The right side is drawn in pieces (volume, then the rest) so the volume can have its own color; `text_advance` places the network's click area behind it. Checked with a second bar on screen: `Vol 30%`, `Vol muted` in the muted color, the network text aligned, and no `pactl` left after bar exits. Clicking and scrolling on it still need a check by hand.

- **vol 0.1:** `vol/vol.c` and `vol.h` on desk Layer 0 (config path, INI) and libpulse, with `pa_mainloop`, which is enough now that vol has no surface of its own. Commands as planned, plus `get` as a synonym for no command and `N%` accepted. The config is read but not written yet (0.4). `--follow` prints `No output` when there is no default sink, so bar shows that rather than a stale level. dwl's four keys and bar's `[volume]` defaults now call `vol`. The checks in `vol/README.md` pass except the ones that need a dwl restart. Once, a restart of PipeWire 0.2 s after a burst of changes came back at 1.00. Four more restarts came back right, so it was probably WirePlumber restoring a level saved before the burst; watch for it.

- **blue 0.1:** `blue/blue.c` and `blue.h` on desk Layer 0 and sd-bus, built like net (the same rows, colors, line cache and key handling), about 1,300 lines. Pulled forward from 0.3: the config file and the live wallpaper palette, since desk made them cheap. Matches are installed synchronously before `GetManagedObjects`, so no signal is missed; `NameOwnerChanged` for `org.bluez` starts over when bluetoothd restarts. Devices are allocated one by one so a pending Connect keeps its pointer, and freeing a device drops its call. "Known" means paired, bonded, trusted or connected. Power, the empty list and live changes are checked; connecting real headphones waits until you pair yours.

- **blue 0.2:** searching, the pairing agent (`/org/blue/agent`, KeyboardDisplay, made the default agent), trust, rename, remove and visibility, as planned. The list became items (known devices, a Nearby heading, nearby devices) so headings can't be selected. Agent questions keep BlueZ's call (`sd_bus_message_ref`, method handler returns 1) until a key answers it; Display calls are answered at once and stay on screen until Pair returns. Search has no discovery filter for now: unnamed devices are hidden in the list instead, with a count. Checked: searching, rename and reset, remove's question, and that the agent refuses callers other than BlueZ. While testing, a test script's keys made the adapter visible and renamed the nearby TV for a moment; both were put back. Pairing a real device waits for yours.
- **blue 0.3:** bar's `[bluetooth]` module reads BlueZ's objects with GetManagedObjects on each relevant signal (adapter, battery, a device's Connected or Alias, objects removed), at most four times a second, so RSSI churn while searching costs nothing. Text labels (`BT`, `BT off`, `BT name 80%`, `+N`) instead of icons, as with the other modules. The right side became segments (volume, Bluetooth, network, clock) each drawn separately, so any of them can be dimmed or clicked. dwl matches app ID `blue` exactly and centers it like `net`. blue gained `[scan] show_unnamed`/`timeout` and `[list] show_battery`/`sort`, and `blue.desktop`. Checked with a second bar and screenshots while switching the adapter off and on; a connected device and the dwl rule wait for a restart and headphones.
- **vol 0.3:** the mixer is in `vol.c`. It was first a TUI and became a desk window like files (`-DDESK_WAYLAND -DDESK_WINDOW -DDESK_KEYBOARD -DDESK_FONT`), since a mixer is mostly sliders; net and blue stay TUIs. It runs on libpulse's main loop with the Wayland fd, key repeat, signals (`pa_signal`), the palette watch and an 80 ms tick as mainloop events, so there is one loop and no threads; around `pa_mainloop_iterate` the loop does Wayland's prepare_read/flush, and the display's io callback does read_events. Hit areas are rebuilt on each paint and searched last first, so the chooser over the list wins. All five lists (server, sinks, sources, sink inputs, cards) are read into a second model and swapped in when the last arrives; events during a read cause one more read. Levels change locally at once so held keys add up, then the server's answer replaces them. Meters are peak-detect record streams at 25 Hz on each output's monitor (not keeping it awake) or each input (keeping the mic open while Input is shown), only for the view on screen. Monitors are left out of Input (`show_monitors` in 0.4). `vol mixer` always opens it; `vol` with no command opens it only when stdout is a terminal, so scripts keep the old behavior, and `-s` opens it on Input. Version 0.3.0; 0.2 stays skipped. The palette colors came along now, since the mixer needs colors anyway; the config keys for them wait for 0.4.
- **vol 0.4:** `[mixer] start_view` (output, input, playback, devices; `-s` wins) and `show_monitors` (monitors listed under Input with the speaker icon; their meters don't keep the output awake). The `[osd]` keys went with the popup to notify. The palette colors were already live from 0.3, so no color keys. A lock on `$XDG_RUNTIME_DIR/vol-mixer.lock` keeps it to one window, since bar's clicks are fire-and-forget; there is no way to raise the open one without xdg-activation, so the second just exits. `vol.desktop` runs `vol mixer`; dwl floats and centers app ID `vol` (exact match, like net and blue); bar's `[volume] click` defaults to `vol mixer`. Version 0.4.0.

## Bluetooth notes

(Written during Stage 0.1.)
