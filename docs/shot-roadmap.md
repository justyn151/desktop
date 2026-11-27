# shot roadmap

Implementation plan for shot, the screenshot tool, from 0.1 to 1.0. It expands [Phase 4 in PLAN.md](../PLAN.md#phase-4-shot).

## Starting point

shot is one `shot.c` and one `shot.h`, built on `desk/` from its first commit, so it's also a test of whether the desk API is pleasant to use.

Checked in `dwl/dwl.c`, the compositor already provides everything shot needs except one IPC event:

| Protocol | In dwl | shot uses it for |
|---|---|---|
| `ext-image-copy-capture-v1` | Yes (`wlr_ext_image_copy_capture_manager_v1_create`) | Copying frames into shm buffers |
| `ext-output-image-capture-source-v1` | Yes | Output sources (0.1, 0.2) |
| `ext-foreign-toplevel-image-capture-source-v1` | Yes (`capturerequest`) | Window sources (0.4) |
| `ext-foreign-toplevel-list-v1` | Yes | Window handles and their `identifier` (0.4) |
| `ext-data-control-v1` | Yes | Clipboard without a focused surface (0.3) |
| `wlr-layer-shell-unstable-v1` | Yes | The selection overlay (0.2) |
| `cursor-shape-v1` | Yes | Crosshair cursor on the overlay, with no `wayland-cursor` (0.2) |
| `xdg-output-unstable-v1` | Yes | Output logical position and size (0.2) |
| dwl IPC `active` | Yes | Default output: the focused monitor |
| dwl IPC `focused_geometry` | Yes | Fallback window crop (0.4) |
| dwl IPC focused toplevel identifier | **No** | Exact window capture (0.4) |

`wlr-screencopy` is also there, but shot uses only the ext protocols.

### What desk has and what shot needs

desk has only the palette and the file watch today (Phase 3 hasn't started). shot adds what it needs to desk as it goes, the same way deal 0.12.0 started desk early. Phase 3 then moves the other apps onto those functions instead of designing them in the abstract.

| desk piece | Layer | Needed by | Phase 3 step it pre-empts |
|---|---|---|---|
| `desk_atomic_*` (temp file in the same directory, fsync, rename, optional no-replace) | 0 | 0.1 | None (wall has its own `replace_contents`) |
| `desk_shm_open` (memfd + ftruncate + seals) | 0 | 0.1 | Step 4 |
| shm buffer pool (`wl_shm_pool`, busy tracking) | 1 | 0.2 | Step 4 |
| Layer-shell surface setup and scale tracking | 1 | 0.2 | Step 4 |
| `desk_palette_load` | 0 | 0.2 (overlay colors) | Already exists |
| INI config parser and paths | 0 | 0.5 | Step 2 |
| Shared `desktop.conf` | 0 | 0.5 | Step 3 |

shot never needs desk's font layer. The overlay draws no text before 1.0.

---

## 0.1: Full-monitor capture to PNG

### Command line

```text
shot [-o OUTPUT] [-c] [-l] [FILE]
  -o, --output NAME   Capture this output (wl_output name, e.g. eDP-1)
  -c, --cursor        Paint the cursor into the image
  -l, --list          Print outputs (name, size, focused marker) and exit
  FILE                PNG path. "-" writes to stdout. Default: ./%Y-%m-%d_%H-%M-%S.png
```

The default output is the one dwl IPC reports as `active`. Without dwl IPC it's the only output, or the first one with a warning when there are several.

### Flow

1. Bind `wl_shm`, every `wl_output` (version 4 for `name`), `ext_output_image_capture_source_manager_v1`, `ext_image_copy_capture_manager_v1` and, optionally, `zdwl_ipc_manager_v2`. Roundtrip twice so output names and IPC `active` arrive. Fail with a clear message naming any missing global.
2. `create_source(output)`, then `create_session(source, cursor ? paint_cursors : 0)`.
3. Collect `buffer_size` and the `shm_format` list until `done`. Choose XRGB8888, ARGB8888, XBGR8888 or ABGR8888, in that order. Fail clearly if only 10-bit formats are offered.
4. Allocate `width × 4 × height` with `desk_shm_open`, create the `wl_buffer`, `create_frame`, `attach_buffer`, `damage_buffer(0, 0, w, h)`, `capture`.
5. Dispatch until `ready` or `failed`. On `failed(buffer_constraints)`, wait for the next `done` and retry once. On `stopped` or a second failure, exit 1.
6. Convert to 8-bit RGB (no alpha, since the screen is opaque), undo the frame's `transform` (below), and write the PNG with libpng.
7. Write through `desk_atomic_*`, mode `0644 & ~umask`. An explicit `FILE` is replaced like `cp` would. The default timestamp name never replaces: on `EEXIST` it retries as `…-1.png`, `…-2.png`.

### Pixel formats

The shm words are little-endian 32-bit values:

| Format | Bytes in memory | To RGB |
|---|---|---|
| XRGB8888 / ARGB8888 | B G R X | `[2] [1] [0]` |
| XBGR8888 / ABGR8888 | R G B X | `[0] [1] [2]` |

### Transform

The `transform` event says what the compositor applied to the buffer, so the upright image is its inverse. For upright pixel `(x, y)` in a `w × h` image, sample the buffer at `rotate(flip(x, y))`. `flip` is `x' = w − 1 − x` for the `flipped*` variants, and the rotation is counter-clockwise by 90, 180 or 270 degrees. 90 and 270 swap the image's width and height relative to `buffer_size`.

### Edge cases

- An output unplugged mid-capture: the session sends `stopped`. Exit 1 with a message.
- The PNG write fails (disk full): the temp file is removed and the target is untouched.
- stdout is a terminal and `FILE` is `-`: refuse, as binary data would garble the terminal.

### Manual checks

- `shot` on a single-monitor session writes a PNG that matches the screen. Check it in `wall`'s browser or an image viewer.
- `shot -c` shows the cursor and `shot` doesn't.
- `shot -l` lists the outputs, and `shot -o NAME` picks the right one on two monitors.
- `shot - | file -` reports a PNG. `shot -` on a terminal refuses.
- Two `shot` runs in the same second produce `…png` and `…-1.png`.
- With a rotated output (`wlr-randr --output X --transform 90`), the image is upright. Needs `wlr-randr`, which isn't installed yet.

---

## 0.2: Region capture

Freeze first, then select, so menus, tooltips and the cursor don't change while dragging.

### Design

- `shot -r` (or `--region`) captures **every** output with 0.1's path before showing anything. The frozen frames are the source of the crop.
- Per output: a layer-shell surface on the `overlay` layer, anchored on all edges, `exclusive_zone = -1`, `keyboard_interactivity = exclusive`. It shows the frozen frame dimmed (about 50%) and the selection undimmed, outlined in the palette's `accent` (fallback `#ffffff`).
- Scale: the frozen frame is in physical pixels and the layer surface is in logical units. `scale = buffer_width / logical_width`. Draw the overlay at physical size and present it with `wp_viewporter` (destination = logical size), so it's pixel-exact at fractional scales. This is the pattern deal 0.10.4 needs too, so whatever goes into desk here should suit it.
- Pointer: `cursor-shape-v1` crosshair. Press starts the drag, motion updates it, release finishes it. A click with no drag (under 3 px) captures nothing and cancels.
- Keyboard: Escape (evdev keycode 1, so no xkbcommon dependency) or a right-click cancels with exit status 1.
- **v1 rule (from PLAN.md):** the region is limited to the output where the drag started. Motion is clamped to that output's edges. Other outputs' overlays stay dimmed.
- Crop: map the logical rectangle to physical pixels with `floor` for the start and `ceil` for the end, then crop the upright frozen frame. No resampling.
- Rendering: redraw only on motion, at most once per `wl_surface.frame` callback. Keep two shm buffers per output, with busy tracking from `wl_buffer.release`. This is where the desk shm pool comes from.
- As built: layer-surface setup stayed in shot. It's about fifteen lines and only shot uses it so far, so it moves to desk in Phase 3 step 4, when bar, run and wall show what they have in common. `wp_fractional_scale_v1` isn't needed: the buffer is the frame's physical size and the viewport maps it, so the scale is `frame width / logical width`.

### Edge cases

- An output with scale 1 next to one with scale 1.5: each overlay uses its own scale.
- The overlay must never show up in its own capture. That holds automatically, because everything is captured before the overlay maps.
- Region completely outside the output after clamping (zero size): treat it as a cancel.

### Manual checks

- Open a right-click menu, run `shot -r`, and the menu is in the frozen frame and in the result.
- Drag at scale 1 and 1.5. The saved size matches the drag in physical pixels and the edges are sharp.
- Escape and right-click cancel with exit status 1 and write no file.
- Dragging past a monitor edge stops at the edge.

---

## 0.3: Clipboard

- `shot --clipboard` (or `-C`) publishes the PNG as `image/png` through `ext-data-control-v1`. That needs no focused surface or input serial, so it works for monitor captures too, not just regions. With a `FILE` it does both.
- The PNG is encoded once into memory. Each `send` writes it to the receiving fd, non-blocking, with partial writes handled in the poll loop.
- After the capture, shot forks. The parent exits 0 so key bindings return at once, and the child serves the selection until `ext_data_control_source_v1.cancelled` (another client took the clipboard). Then it exits.
- Also offer `image/png` only. No text types, so pasting into a terminal does nothing instead of dumping binary data.
- The child closes stdin, stdout and stderr and calls `setsid`, so it doesn't hold a terminal open.
- As built: the child opens its own Wayland connection after the fork, and the parent waits on a pipe until the child holds the selection, so the exit status reports a refused clipboard. Each paste is written by a short-lived grandchild, so a stalled reader can't block the others. The PNG is encoded once in memory and shared by the file, stdout and clipboard paths. `[save] clipboard` arrived with it.

Manual checks: `shot -r -C`, paste into a browser or image editor; `wl-paste -l` lists `image/png`; copy some text and confirm the background shot process exits.

---

## 0.4: Focused-window capture

### dwl change

Add the focused toplevel's `ext_foreign_toplevel_list_v1` identifier to dwl IPC: a new `focused_identifier(string)` event on `zdwl_ipc_output_v2`, protocol version 3, sent next to `title` and `appid` when focus changes. Update both copies of `dwl-ipc-unstable-v2.xml` (`dwl/protocols` and `bar/protocols`), which are identical today. bar binds version 2, so it keeps working unchanged.

### shot

- `shot -w` (or `--window`): read the active output's `focused_identifier`, then bind `ext_foreign_toplevel_list_v1` and find the handle with that identifier.
- `ext_foreign_toplevel_image_capture_source_manager_v1.create_source(handle)`, then the same session and frame path as 0.1. It captures the window's own content, so overlapping windows and the bar don't appear.
- Fallback, when the IPC event is missing (older dwl): capture the output and crop to `focused_geometry`. Say so on stderr, since overlapping windows will show.
- No focused window: exit 1 with a message.

Manual checks: a partially covered window comes out whole; floating and fullscreen windows; an XWayland window (dwl builds its capture scene for both surface kinds); empty workspace → error.

---

## 0.5: Config, key bindings and default directory

The config file, its parser and the `~/Pictures/Screenshots` default landed early, in 0.2: shot writes the sample `shot.conf` when the user's file is missing. Shared `desktop.conf` precedence still waits for Phase 3 step 3. What's left here is the dwl bindings and install.

- Config through desk's INI parser (Phase 3 step 2 must exist by now, or shot adds it). Precedence: command line, `~/.config/shot/shot.conf`, `~/.config/desktop/desktop.conf`, built-in defaults.

  ```ini
  [save]
  directory = ~/Pictures/Screenshots
  name = %Y-%m-%d_%H-%M-%S.png   ; strftime
  clipboard = true                ; also copy every capture

  [capture]
  cursor = false

  [png]
  compression = 6                 ; 0–9, zlib level

  [overlay]
  dim = 0.5
  ```

- The default directory is created with `mkdir -p` semantics on first use. `XDG_PICTURES_DIR` from `user-dirs.dirs` replaces `~/Pictures` when it's set.
- dwl bindings in `config.def.h`:

  | Key | Command |
  |---|---|
  | Print | `shot -r` (region) |
  | Shift+Print | `shot` (focused monitor) |
  | Super+Print | `shot -w` (focused window) |

- `make install` installs `shot` and a sample `shot.conf`, following bar's `PREFIX`/`BINDIR`/`DESTDIR` conventions. Add `!/shot/shot.conf` and `/shot/shot` to `.gitignore`.

Manual checks: each binding saves into `~/Pictures/Screenshots` with the configured name; a config typo reports the line number and falls back to defaults; `compression = 0` produces a visibly larger file.

---

## 1.0: Daily driver

- Every 0.x checklist passes on dwl.
- Exit codes are documented (0 saved, 1 cancelled or failed, 2 usage) so scripts can rely on them.
- README documents every option, config key and binding.
- Tag `shot-1.0.0`.

## After 1.0 (unscheduled)

- Regions that span outputs (stitching frames at different scales)
- Selection handles: move and resize the rectangle before confirming
- A size label while dragging (would need desk's font layer)
- Delay (`--delay 3`) and a notification through `notify` once it exists
- Annotation: probably never. That belongs in an image editor.

## Milestone summary

| Version | Theme | Status |
|---|---|---|
| 0.1 | Full-monitor capture to PNG, desk atomic write and shm | Done (multi-monitor and rotation untested) |
| 0.2 | Region: freeze, layer-shell overlay, crop. Config file (pulled forward from 0.5) | Done (cancel, edge and scale checks pending) |
| 0.3 | Clipboard through `ext-data-control-v1`, background server | Code done, paste into an app pending |
| 0.4 | Focused window: dwl IPC identifier, toplevel capture source | Planned (needs a dwl change) |
| 0.5 | Print bindings in dwl, install the sample config | Planned (config itself landed in 0.2) |
| 1.0 | Checklists, exit codes, README | Final |
