# deal roadmap

Implementation plan for every deal milestone from 0.10.3 to 1.0.0. It expands Phase 1 and the deferred deal milestones in [PLAN.md](../PLAN.md), and adds milestones for gaps found in the current code.

## Where deal stands (0.10.3)

deal is one `terminal.c` (about 5,900 lines) and one `terminal.h`. It uses xdg-shell, EGL and fixed-function desktop OpenGL (`glBegin`/`glEnd` quads over a `GL_ALPHA8` glyph atlas), FreeType and Fontconfig, xkbcommon, and a Linux PTY.

Working today:

- Parsing: ANSI/DEC, 256-color and RGB, alternate screens (47/1047/1049), focus reports (1004), bracketed paste (2004), mouse modes 1000/1002/1003 with SGR (1006), DECRQM replies, OSC titles, and OSC 8 links.
- Screen: UTF-8, wide cells, two combining marks per cell, scrollback reflow with a 64 MiB budget.
- Input: XKB text, key repeat that follows the compositor, zoom, and configurable shortcuts.
- Clipboard: `wl_data_device` clipboard, primary selection, and selection by word or line.
- Config: INI file with live reload through Ctrl+Shift+R or SIGHUP.

Gaps found while reading the code. These feed the milestones below.

| Gap | Evidence in `terminal.c` |
|---|---|
| Bold changes only the color. No bold or italic face is loaded. | `Rendition.bold` is used only by `color_source` |
| No italic, strikethrough, overline, curly/dotted underline or colored underline | SGR handles 0, 1, 2, 4, 7, 22, 24, 27, 39 and 49 only |
| No synchronized output (mode 2026). TUIs can tear mid-frame. | No `2026` case |
| No OSC 52 clipboard (needed for copy over SSH and from Neovim) | No `52` OSC handler |
| No OSC 7 working directory, so new windows can't open in the shell's directory | None |
| No OSC 133 prompt marks, so there's no jump to the previous or next prompt | None |
| No scrollback search | None |
| No progressive keyboard protocol (kitty `CSI u`) and no `modifyOtherKeys` | None |
| Scale-1 buffers only | No `wl_output` scale handling or `wp_fractional_scale_v1` |
| Immediate-mode GL draws every cell as separate quads | `glBegin` in the draw path |
| No urgency bell, no compose, no IME | Listed as deferred in the README |

---

## 0.10.3: Verify clipboard, selection and mouse

The code is already merged. This milestone is the manual checklist in [`deal/README.md`](../deal/README.md).

### Work

1. Build clean with `make CFLAGS+=-Werror` so any warnings show up now and not in Phase 2.
2. Work through the 17 checklist items in order. Keep a scratch file listing each failure, with steps to reproduce it.
3. Fix each failure in a separate commit (`deal: fix <thing>`), so a regression can be bisected to one change.
4. Selection and reflow are where bugs are most likely, because the README already says "Selection highlighting can clear during reflow". Decide whether that's acceptable for 0.10.3 or needs fixing. My recommendation: keep it, but make sure copying after a reflow never returns garbage.
5. Tick the checklist, update the README version, and tag `deal-0.10.3`.

### Done when

Every checklist item is ticked on dwl and the tag is pushed. **Done 2026-11-27.**

---

## 0.10.4: Scaling and rendering polish

### Protocols

| Protocol | Use |
|---|---|
| `wl_surface.preferred_buffer_scale` (wl_compositor v6) | The integer scale the compositor wants |
| `wl_surface.enter`/`leave` + `wl_output.scale` | Fallback for older compositors |
| `wp_fractional_scale_v1` | Preferred scale as N/120 |
| `wp_viewporter` | `wp_viewport_set_destination` maps a physical buffer to the logical size |

Add the two staging XML files to the Makefile the same way xdg-shell and primary selection are generated now.

### Design

- Keep a single `double scale` in `Platform`. Fractional scale wins when it's present, then preferred buffer scale, then the largest `wl_output.scale` among outputs the surface has entered, then 1.
- **Physical pixels are the source of truth.** Rasterize fonts at `round(font_size × scale)`. Measure the cell in physical pixels (integer width and height). Compute padding in physical pixels as `round(padding × scale)`. Derive the grid from `physical_width = round(logical_width × scale)`. This keeps every cell an exact integer number of pixels, which removes seams at 1.25× and 1.5×.
- With fractional scale, call `wl_egl_window_resize` with the physical size, set `wl_surface_set_buffer_scale(1)`, and call `wp_viewport_set_destination(logical_w, logical_h)`. With integer scale and no viewporter, use `wl_surface_set_buffer_scale(n)`.
- Input coordinates (pointer and selection) arrive in logical units. Multiply by scale before mapping to cells.
- On a scale change: drop the glyph atlas, reload faces at the new pixel size, recompute metrics and resize the grid. Reuse the zoom path, since zoom already re-rasterizes and keeps the window size.
- Zoom and scale combine: the effective pixel size is `zoom_size × scale`.

### Edge cases

- The window straddles two outputs with different scales. Use the fractional value if the compositor sends one; otherwise use the highest scale.
- Scale changes during a pending configure. Apply it with the next configure ack, not halfway through a frame.
- Size hints (`xdg_toplevel_set_min_size`) stay in logical units.

### Manual checks

- Output scale 1, 1.25, 1.5 and 2 (`wlr-randr --output X --scale 1.5`). Look for gaps between box-drawing characters (`─│┼`) and between background-colored cells.
- Drag a window between outputs with different scales and confirm the text re-sharpens.
- `stty size` stays correct after every scale change.

---

## 0.10.5: Text attributes and font styles (new)

This one was missing from the plan. Neovim, git and most TUIs already send these, and deal drops them silently.

- **Bold, italic and bold italic faces.** Ask Fontconfig for `weight=bold` and `slant=italic` variants of the configured family. Fall back to synthetic bold (`FT_GlyphSlot_Embolden`) and synthetic italic (`FT_Outline_Transform` shear) when there's no real face. Add `[font] bold_is_bright = true` to keep today's behavior available.
- **SGR additions:** 3/23 italic, 9/29 strikethrough, 53/55 overline, 5/25 blink (render it, or ignore it deliberately), 8/28 conceal, `4:0`–`4:5` underline styles (single, double, curly, dotted, dashed), and 58/59 underline color.
- **Cell storage:** `TerminalCell` already uses bitfields. Add an `attributes` byte (italic, strike, overline, conceal) plus `underline_style : 3`, and an underline color source/RGB. Check the scrollback budget: the cell grows by about 4 bytes, so recompute how many lines fit in 64 MiB.
- **Glyph cache key** becomes `(codepoint, face_style)`.
- ~~**Box drawing and block elements** drawn procedurally~~. Dropped: deal already scales these glyphs to the cell edges using the font's own full-length strokes. Revisit only if seams show up at fractional scales in 0.10.4.

Manual checks: `printf '\e[3mitalic\e[0m \e[1mbold\e[0m \e[9mstrike\e[0m \e[4:3;58:2::255:0:0mcurl\e[0m\n'`, Neovim diagnostics (undercurl), and `git diff --color-words`.

---

## 0.11.0: Compose and urgency bell

### Compose and dead keys

- `xkb_compose_table_new_from_locale` and `xkb_compose_state_new`. It's part of libxkbcommon, so there's no new dependency. deal forces `LC_CTYPE=C.UTF-8`, so the table's locale comes from `LC_ALL`, `LC_CTYPE` or `LANG`, falling back to `en_US.UTF-8`.
- In the key handler, feed each keysym through `xkb_compose_state_feed`. On `COMPOSING`, send nothing. On `COMPOSED`, send `xkb_compose_state_get_utf8`. On `CANCELLED`, reset and drop the key. On `NOTHING`, use the normal path.
- Key repeat must not repeat a compose sequence's intermediate keys.
- Reset compose state on keyboard focus loss and on keymap change.

### Urgency bell

- Bind `xdg_activation_v1`. On BEL while unfocused and with `[bell] urgency = true`, get a token and call `xdg_activation_v1_activate`. dwl turns that into the urgent flag on the client.
- Checked: dwl's `urgent()` handler ignores the focused client and only sets `isurgent` and the urgent border on others. It never steals focus, so no dwl patch is needed.
- Rate-limit urgency to once a second so `yes $'\a'` can't flood it.
- Audible bell: keep it unsupported. There's no system sound path yet (`vol` doesn't exist). Leave the setting accepted, as now.

### IME decision

Recommendation: **defer `zwp_text_input_v3` to after 1.0**, but write the code shape down now. dwl would need `text-input-v3` and `input-method-v2` relays, and nothing on this system uses an IME today. Compose covers accented Latin input. Revisit if CJK input is ever needed.

### Manual checks

- Compose key + `'` + `e` gives `é`, and a dead-key layout (us intl) types `ê`.
- `sleep 3; printf '\a'` in an unfocused window marks it urgent in dwl and the bar.

---

## 0.11.1: Remote clipboard, synchronized output and working directory (new)

Split out of 0.11.0.

- **OSC 52 clipboard write**, so Neovim's `"+y` works over SSH. Decode base64 and publish to the clipboard (`c`) or primary (`p`). Writes are on by default (`[selection] osc52`). Reads (`?`) are never supported, because reading the clipboard is a data-exfiltration path. Payloads use their own buffer, capped at 16 MiB of base64, because the normal OSC buffer holds only 2,304 bytes.
- **Synchronized output (DECSET 2026):** while it's set, hold frame submission until it's reset or a 150 ms timeout. Report it in DECRQM. This removes tearing in Neovim and other fast TUIs.
- **OSC 7:** store the reported `file://host/path` working directory when the host is local. Add a "new window" shortcut (default Ctrl+Shift+N) that spawns `deal --working-directory <cwd>`, falling back to `/proc/<shell>/cwd` when the shell doesn't report one.

### Manual checks

- In Neovim over `ssh`, `"+y` reaches the local clipboard.
- Neovim redraws without tearing while scrolling a large file.
- `cd /tmp`, then Ctrl+Shift+N opens a window in `/tmp`.

---

## 0.11.2: Keyboard protocol (new)

Modern TUIs (Neovim, Helix, fish 4, and later `lead`) can tell Ctrl+I from Tab and Ctrl+Shift+letter from Ctrl+letter only with an extended keyboard protocol.

- Implement the kitty progressive enhancement protocol: `CSI > flags u` push, `CSI < u` pop, `CSI ? u` query, and `CSI = flags ; mode u` set. Support flags 1 (disambiguate), 2 (report events: press, repeat and release), 8 (report all keys as escapes) and 4 (alternate keys) where the xkb data allows.
- Keep one flags stack per screen (main and alternate), as the spec requires.
- Also support xterm `modifyOtherKeys` level 2 (`CSI > 4 ; 2 m`), which some programs use instead.
- This matters for `lead`: the editor can depend on deal sending unambiguous keys.

Manual check: `kitten show-key` or a small test script that sends `CSI > 1 u` and prints raw input.

---

## 0.11.3: Scrollback navigation (new)

- **Search:** Ctrl+Shift+F opens a one-line search bar drawn over the bottom row. Search history plus the screen, case-insensitive by default, with Enter/Shift+Enter for next/previous, highlight all matches, and Escape to close. Search the logical (unwrapped) lines so matches across a soft wrap are found.
- **Prompt marks (OSC 133):** record `A`/`B`/`C`/`D` marks per line. Ctrl+Shift+Up/Down jumps to the previous or next prompt. Add a small fish/bash snippet to the README that emits the marks.
- **Copy last command output** (Ctrl+Shift+O): the range between the last `C` and `D` mark.

---

## 1.x preparation: Make deal the default terminal (Phase 1.4)

This isn't a deal release, but it gates foot's removal.

1. `dwl/config.def.h`: `termcmd` → `{ "deal", NULL }`. Commit the real Super-based layout at the same time (it's Phase 2's first item, and it touches the same file).
2. `bar.conf`: network command → `deal --app-id net --columns 84 --rows 26 -e net`.
3. Confirm dwl's floating rule matches `appid = "net"`. deal sets the app ID from `--app-id` before the first commit, so the rule should match at map time. Verify it with dwl IPC or by watching the window float.
4. `run`: find how it launches `Terminal=true` entries. If foot is hardcoded, read `$TERMINAL` with a `deal` default.
5. `make install` deal, and set `TERMINAL=deal` in the session environment.
6. Daily-driver week: keep a `docs/deal-papercuts.md` list of every annoyance. Anything that sends you back to foot is a blocker before uninstalling it.
7. `pacman -Rns foot`, then `grep -rn foot` across the repo until it returns nothing.

---

## 0.12.0: Wallpaper-derived theming

Depends on Phase 3 step 1 (`desk_palette_load` and the watch).

- Replace deal's `[colors]`/`[palette]` defaults with the desk palette: map `ansi[0..15]` onto palette entries 1–16, and the roles (`background`, `foreground`, `cursor`, `selection_bg`, `selection_fg`) onto the matching `[colors]`/`[selection]` keys.
- Precedence follows the shared config rules: an explicit value in `deal.conf` beats the palette, and the palette beats built-in defaults. Add `[colors] source = palette | config` to opt out.
- Watch the palette file with `desk`'s inotify helper. On change, reuse the reload path. It already updates colors live without touching the PTY.
- Contrast guard: if the generated foreground/background contrast is below 4.5:1, nudge the foreground lightness. Keep the guard in desk so bar and run share it.
- **OSC 4/10/11/12 queries** reply with the current colors, so programs like Neovim can detect light or dark backgrounds. Also handle OSC 4/10/11 sets and OSC 104/110/111 resets.

Manual check: change the wallpaper with `wall` and confirm open deal windows recolor within a second, with no flicker or lost scrollback.

---

## 0.13.0: Optimization

After Phase 3, once the font rasterizer comes from desk.

### Renderer

- Replace immediate-mode GL with **one dynamic vertex buffer per frame**: background quads, then glyph quads, then decorations, drawn with `glDrawArrays`. This stays fixed-function and needs no shaders, so it works on GL 1.5 and later. Only move to a GL 2.1/ES 2.0 shader path if profiling shows it's needed.
- **Damage tracking:** keep a per-row dirty bit, set by the parser and cleared after drawing. Use `eglSwapBuffersWithDamageKHR` (or `wl_surface_damage_buffer`) with the dirty rows. Buffer age (`EGL_EXT_buffer_age`) lets unchanged rows skip redrawing.
- **Atlas:** grow it by repacking or a second page instead of flushing everything when it fills. Keep ASCII in the atlas permanently.

### Parser and PTY

- Read the PTY in 64 KiB chunks and parse the whole chunk before rendering once. Render at most once per frame callback, which is already the pacing model.
- Fast path for runs of printable ASCII: write cells straight into the row without going through the full state machine per byte.
- Make sure scrolling is a pointer change in the circular buffer and not a memmove. `first_row` already exists, so check every path uses it.

### Measurements (before and after, saved in `docs/deal-bench.md`)

| Test | Command |
|---|---|
| Throughput | `time cat 100MB-file.txt` and `vtebench` if available |
| Latency | `typometer`, or a camera on a 240 Hz recording for a rough number |
| Idle CPU | `pidstat -p <pid> 1` with a blinking cursor and without one |
| Memory | RSS after 100k lines of scrollback |

Goal: under 1% idle CPU with blink off (already zero frames when idle), and throughput at least matching foot.

---

## 1.0.0: Daily driver

Entry criteria:

- 0.12 and 0.13 are done.
- foot has been uninstalled for at least four weeks and nothing needed it.
- `docs/deal-papercuts.md` is empty or every remaining item is explicitly deferred.

Release work:

1. **Compatibility pass:** run `vttest` (menus 1, 2, 3, 6, 8 and 11) and record the results. Run `esctest` if it's convenient. Fix crashes and serious mis-renders. Document known deviations in the README.
2. **Fuzzing:** a small `-DDEAL_FUZZ` build that feeds stdin to the parser with no Wayland. Run it under AFL++ or libFuzzer with ASan/UBSan for a few hours. The parser has the most attack surface (any `cat` of an untrusted file reaches it).
3. **terminfo:** default `TERM` changes from `xterm-color` to `deal`, falling back to `xterm-256color` (not `xterm-color`, which advertises 8 colors). Add `Tc`/`RGB`, `Smulx`, `Setulc`, `Ss`/`Se` (cursor shape) and `Sync` capabilities to match what deal supports.
4. **README rewrite:** drop milestone language, and document every config key, shortcut and escape sequence deal supports.
5. Tag `deal-1.0.0`.

---

## After 1.0 (unscheduled)

- IME with `zwp_text_input_v3` (and dwl relays)
- Text shaping and ligatures (HarfBuzz would be a new dependency, so think hard about it)
- Terminal graphics: kitty graphics protocol or sixel, mainly for `view` previews inside the terminal
- Tabs or splits: probably never. dwl tiling covers this.
- Single-instance daemon mode (`deal --server`), to cut startup time and memory for many windows

## Milestone summary

| Version | Theme | Status |
|---|---|---|
| 0.10.3 | Verify clipboard, selection, mouse | Done |
| 0.10.4 | HiDPI and fractional scaling | Planned |
| 0.10.5 | Text attributes, bold/italic faces | Done |
| 0.11.0 | Compose and dead keys, urgency bell (off by default) | Done (Compose untested) |
| 0.11.1 | OSC 52, synchronized output, OSC 7 | Code done, checklist pending |
| 0.11.2 | Kitty keyboard protocol | **New** |
| 0.11.3 | Scrollback search and prompt marks | **New** |
| (Phase 1.4) | deal becomes the default, foot removed | Planned |
| 0.12.0 | Palette from desk, OSC color queries | After Phase 3 step 1 |
| 0.13.0 | Renderer, damage and parser optimization | After Phase 3 |
| 1.0.0 | vttest, fuzzing, terminfo, docs | Final |
