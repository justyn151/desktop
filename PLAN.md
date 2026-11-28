# Desktop Plan

Roadmap for the custom Arch Linux + dwl desktop in `~/Projects/desktop`.

Ground rules that apply to every phase:

- One `.c` and one `.h` per app, with as few external libraries as possible.
- Shared code lives in `desk/` (one `.c`/`.h` pair) once Phase 3 starts.
- Each stage gets built and manually tested before the next one starts.
- Build on working code. Don't reset it.

---

## Where things stand (2026-11-28)

**Now:** `mark`'s features are all in (0.1 to 0.5: parser, GitHub extensions, pager, links and search, config and palette), and so are `view`'s. What's left for both is 1.0, mostly live checks and a week of daily use, so they run alongside the next app instead of blocking it. The next app is `files`, the graphical file manager, pulled forward from Phase 5 the way view and mark were: it's the last big piece of daily use still handled by something outside the desktop. Details for view and mark are in [docs/view-roadmap.md](docs/view-roadmap.md) and [docs/mark-roadmap.md](docs/mark-roadmap.md).

**Next, in order:**

1. `files`: 0.1 from [docs/files-roadmap.md](docs/files-roadmap.md): one directory with keyboard navigation and live refresh. It brings desk's font layer (`-DDESK_FONT`, Phase 3 step 5 started early), a shared xkb keyboard and an xdg-shell window helper, with view moved onto the last two in the same change. Worth installing `shared-mime-info` before 0.3, which reads its MIME globs to pick applications.
2. Alongside `files`, `mark` toward 1.0: the 0.1 to 0.5 live checks in deal (`mark/README.md`), a week of daily use, fuzzing the parser and the pager's input, final README, tag `mark-1.0.0`
3. Alongside `files`, `view` toward 1.0:
   - 0.5 live checks: `make -C view install`, `make -C dwl install`, restart dwl, then the four checks in `view/README.md` (floating, `run`, wallpaper recolor; `xdg-open` waits for xdg-utils)
   - 0.4 live checks once there are sample files: a portrait phone photo, a WebP, an animated GIF
   - A week as the default image viewer, with annoyances in `docs/view-papercuts.md`
   - Fuzz `desk_image_load` (PNG, JPEG, GIF, WebP, EXIF) under ASan/UBSan for a few hours, especially the hand-written GIF decoder
   - Final README pass and tag `view-1.0.0`
4. Back to the open items: deal 0.10.4 and 0.11.0 (Phase 1), the week on deal and removing foot (Phase 1.4), shot 0.5 and 1.0 (Phase 4), then Phase 3

**Recently finished:**

| What | Result |
|---|---|
| shot 0.1–0.3, 0.5 binding | Monitor and region capture to PNG and the clipboard, Super+Shift+S. 0.4 (window capture) skipped |
| view 0.1 | One image fitted to the window, sharp at any scale |
| view 0.2 | Zoom (keys and scroll around the pointer), pan, rotate, flip; smooth shrinking, crisp enlarging; key repeat |
| view 0.3 | Browse a directory or a list, preload neighbours, reload a replaced file |
| view 0.4 | GIF (built-in decoder) and WebP, EXIF orientation, checkerboard behind transparency, unsupported formats named. Live checks for portrait photos, WebP and GIF still to do once there are sample files |
| view 0.5 | `view.conf` (background, transparency, fit_enlarge, filter, start, wrap, sort), palette background that follows the wallpaper, no-argument start directory, dwl float rule, `view.desktop` and `make default`, shot's `[save] open` |
| mark 0.1 | CommonMark parser (all 652 spec examples), wrapped output with deal's bold, italic, underline colors and OSC 8 links, shaded code |
| mark 0.2 | GitHub tables (box drawn, columns fitted to the width), strikethrough, task lists, bare-URL autolinks; all 21 GitHub examples |
| mark 0.3 | The pager: scrolling by key and wheel, resize reflow that keeps your place, sideways scrolling of wide tables, status line with the current heading, clean exit on every key and signal. Code blocks got padding and wrap instead of being cut |
| mark 0.4 | Tab through links and follow them (other markdown files open in the pager, anchors jump, the rest go to `xdg-open`), Backspace back, `/` and `?` search with highlights, `]`/`[` heading jumps, an outline on `o` |
| mark 0.5 | `mark.conf`, colors from the wallpaper's palette that follow it live, reload on save as a live preview, `e` to edit in lead at the current line (lead learned `+N`), desktop entry |
| deal 0.13.3 | Box-drawing characters drawn from the cell size instead of the font, so table and frame lines meet exactly |
| Fonts | monospace is Noto Sans Mono, sans-serif Noto Sans, serif Libertinus Serif, with Nerd Font symbols as a fallback (`~/.config/fontconfig/fonts.conf`); bar, wall and run follow `monospace` |
| desk | Image decoding moved from wall (`desk_image_load`), key repeat (`DeskRepeat`), alongside shot's atomic files, shm, INI config and buffer pool |

---

## Phase 1: Finish deal and replace foot

Goal: deal is the only terminal on the system.

### 1.1 Verify 0.10.3 (Clipboard, Selection and Mouse)

The code is already in the repo. What's left is the manual checklist in `deal/README.md`.

- [x] Work through every item in the "Manual verification on dwl" checklist
- [x] Fix whatever fails
- [x] Tick the checklist and tag 0.10.3

### 1.2 0.10.4: Scaling and rendering polish

dwl already supports `wp_fractional_scale_v1` and `wp_viewporter`.

- [ ] Integer scale with `wl_surface.preferred_buffer_scale` and `wl_surface_set_buffer_scale`
- [ ] Fractional scale with `wp_fractional_scale_v1` (scale = N/120) and `wp_viewport_set_destination`
- [ ] Rasterize glyphs at `font_size × scale`
- [ ] Compute the cell size in physical pixels first, then the grid, so there are no seams at 1.25× or 1.5×
- [ ] Re-rasterize when the window moves to an output with a different scale

### 1.3 0.11.0: Wayland feature parity

- [ ] Compose and dead keys with `xkb_compose_*` (part of libxkbcommon, no new dependency)
- [ ] Urgency bell (`xdg_activation_v1` or dwl urgency)
- [ ] Decide what to do about IME: implement it or defer it to after 1.0

### 1.4 Make deal the default terminal

- [x] `dwl/config.def.h`: `termcmd` → `deal`
- [x] `bar.conf`: the network `command` → `deal --app-id net --columns 84 --rows 26 -e net`
- [ ] dwl floating rule: confirm it matches app ID `net` when launched from deal
- [x] `run`: desktop entries with `Terminal=true` launch in deal (`terminal = deal -e`)
- [ ] Use only deal for a week, then uninstall foot

### Deferred deal milestones

- **0.12.0 (wallpaper-derived theming):** after Phase 3 step 1, so it uses `desk_palette_load` instead of adding a 6th palette parser
- **0.13.0 (optimization):** after Phase 3
- **1.0.0 (daily driver):** once 0.12 and 0.13 are done and nothing has needed foot for a while

**Phase 1 is done when:** foot is uninstalled and nothing in the repo references it.

---

## Phase 2: Bug fixing

Goal: fix existing problems before refactoring, so the migration to desk starts from known-good behavior.

### Known issues

- [x] **dwl config mismatch:** `config.def.h` still uses `MODKEY = Alt` and `Alt+Shift+Enter` for the terminal. The README documents a Super-based layout (Super+Enter, Super+Q) that only exists in the gitignored `config.h`. Commit the real layout as `config.def.h`. `config.h` is tracked too, so the running layout is in git.
- [ ] **Remaining foot references:** sweep the repo after Phase 1.4 (`grep -rn foot`)
- [ ] **dwl TODO/FIXME:** review the 4 markers in `dwl.c`

### My own list

- [ ] …
- [ ] …

**Phase 2 is done when:** every app builds without warnings, and its README checklist passes on the current session.

---

## Phase 3: Design desk and migrate to it

Goal: one copy of shared code instead of five or six.

### Shape

```text
desk/
├── desk.h
├── desk.c
└── README.md
```

- Each app compiles `../desk/desk.c` into its own binary. No `.so`, nothing extra to install.
- Feature macros keep dependencies opt-in:
  - **Layer 0** (always): paths, atomic write, INI config, palette, inotify watch, argv split and spawn, UTF-8
  - **Layer 1** (`-DDESK_WAYLAND`): shm buffer pool, layer-shell setup, scale tracking, keyboard (xkb, compose, repeat timerfd)
  - **Layer 2** (`-DDESK_FONT`): Fontconfig/FreeType loading, glyph cache, text measuring, drawing into ARGB buffers
- Rules: no event loop, no global state, no widgets, no printing (return errors), nothing app-specific.

### Shared configuration

Precedence order, implemented once in `desk_conf_load`:

1. Command-line options
2. `~/.config/<app>/<app>.conf`
3. `~/.config/desktop/desktop.conf` (shared `[font]`, `[theme]`)
4. Built-in defaults

Opacity is configured per application today: `[window] opacity` in deal and `[appearance] opacity` in wall's browser. Both apply it to the background only. Move it to `[theme] opacity` in `desktop.conf` in step 3, so every app reads one value. A compositor-wide setting in dwl (`wlr_scene_buffer_set_opacity`) would be simpler, but it fades text and images too, so the shared app setting is the better fit.

### Migration order

Migrate one step at a time. Behavior must stay identical, verified with each app's README checklist.

| Step | Code | Duplication today | Apps |
|---|---|---|---|
| 1 | Palette load, save and watch | 5 copies | net, dwl, then bar, run, wall |
| 2 | INI config parser | 6 copies | all except lead |
| 3 | Shared `desktop.conf` | n/a | all |
| 4 | shm buffers and layer-shell | 3 copies | bar, run, wall |
| 5 | Font rasterizing and glyph cache | 4 copies | bar, run, wall, deal (rasterizer only; GL atlas stays in deal) |
| 6 | Keyboard, repeat and compose | 2 copies | run, deal |

deal 0.12.0 started `desk/` early with `desk_palette_load` and `desk_watch_*`. deal already uses them, so step 1 now means moving net, dwl, bar, run and wall onto the same functions. The palette has no `ansi[16]`, so deal derives its ANSI colors from the roles.

shot and view have grown desk further ahead of this phase. What's already there and who uses it:

| desk piece | From | Used by | Step it pre-empts |
|---|---|---|---|
| Palette and file watch | deal 0.12.0 | deal, view (background since 0.5) | 1 |
| INI parser, config paths, `desk_write_default` | shot 0.2 | shot, view | 2 (the other apps still have their own) |
| shm buffer pool (`-DDESK_WAYLAND`) | shot 0.2 | shot, view | 4 (layer-shell setup still in each app) |
| Key repeat (`DeskRepeat`, a timerfd) | view 0.2 | view | 6 (keymap and compose still in each app) |
| Atomic files, `desk_shm_open` | shot 0.1 | shot | none |
| Image decoding (`-DDESK_IMAGE`, PNG, JPEG, optional GIF and WebP, EXIF orientation) | wall, moved for view 0.1 | wall, view | none |

**Phase 3 is done when:** no app contains its own palette parser, config parser, shm buffer code or font loader.

---

## Phase 4: shot

Built on desk from its first commit, so it's also a test of whether the desk API is pleasant to use. The detailed plan is in [docs/shot-roadmap.md](docs/shot-roadmap.md).

dwl already supports `ext-image-copy-capture-v1`, `ext-image-capture-source-v1` and `ext-foreign-toplevel-image-capture-source`.

| Version | Milestone | Status |
|---|---|---|
| 0.1 | Full-monitor capture to PNG (libpng, already used by wall) | Done (multi-monitor and rotation untested) |
| 0.2 | Region capture: freeze all outputs first, select on a layer-shell overlay, crop from the frozen frame | Done (cancel, edge and scale checks pending) |
| 0.3 | Clipboard (`image/png`). The process stays in the background until the selection is cancelled | Done |
| 0.4 | Skipped: focused-window capture, not needed for now | Skipped |
| 0.5 | Config, Super+Shift+S for region capture, save to `~/Pictures/Screenshots/%Y-%m-%d_%H-%M-%S.png` and copy | Binding added, pending a dwl restart; install left |
| 1.0 | Checklists, exit codes, README | Not started |

Notes:
- Handle XRGB8888 and XBGR8888 shm formats
- For v1, limit a region to the output where the drag started
- Write to a temp file, then rename

---

## Phase 5: Remaining core apps

Rough order, based on what gets touched most often day to day:

1. `lock`
2. `notify`
3. `clip`
4. `vol`
5. `power`
6. `mount`
7. `view`: pulled forward; features done, 1.0 in progress (see below)
   - `mark` followed `view`, ahead of the rest of this list; features done, 1.0 in progress
8. `files`: pulled forward next, ahead of `lock` to `mount`
9. `ctl`
10. Then everything else from the project list below

### view

The detailed plan is in [docs/view-roadmap.md](docs/view-roadmap.md). Built on desk: image decoding, shm buffers, key repeat and the file watch.

| Version | Milestone | Status |
|---|---|---|
| 0.1 | One image fitted to the window, scale-correct; decoders move from wall to desk | Done (fractional scale check skipped: 1× only for now) |
| 0.2 | Zoom, pan, rotate, flip; area-average shrinking from mip levels, bilinear then nearest enlarging; key repeat | Done |
| 0.3 | Browse a file's directory, a directory or a list; preload neighbours; reload a replaced file | Done |
| 0.4 | GIF (built-in) and WebP, EXIF orientation, checkerboard, unsupported formats named | Done (live checks wait for sample files) |
| 0.5 | Config, palette background, start directory, dwl float rule, desktop entry, shot's open-after-capture | Code done, live checks pending |
| 1.0 | Live checks, a week of use, fuzzing the decoders, README, tag | In progress alongside `mark` |

### files

A graphical file manager with Miller columns (parent, current, preview) and H/J/K/L navigation, pulled forward after view and mark. The detailed plan is in [docs/files-roadmap.md](docs/files-roadmap.md). Icons are Nerd Font glyphs in palette colors, so it needs no icon theme; opening files resolves `mimeapps.list` and desktop entries itself, so it doesn't need xdg-utils.

| Version | Milestone | Status |
|---|---|---|
| 0.1 | One directory, keyboard navigation, live refresh; desk gains the font layer, keyboard and window, view moves onto them | Not started |
| 0.2 | Miller columns, previews of directories, images and text, status line | Not started |
| 0.3 | Opening with the default application, Open with, filter, path entry, bookmarks, sorting | Not started |
| 0.4 | Selection, copy, cut, paste, rename, new, trash, undo, progress, shared clipboard | Not started |
| 0.5 | Mouse and drag and drop, thumbnail grid, config, palette, desktop entry | Not started |
| 1.0 | Checklists, a week of use, fuzzing, README, tag | Not started |

### mark

A terminal markdown viewer, started once `view`'s features were done (0.5), while view 1.0's week of use runs. It renders headings, emphasis, lists, tables, code blocks and links in a pager, using deal's text attributes (real bold and italic faces, strikethrough, colored underlines, OSC 8 links). The detailed plan is in [docs/mark-roadmap.md](docs/mark-roadmap.md). Built on desk's Layer 0 only (INI config, palette, file watch), so it links nothing beyond libc.

| Version | Milestone | Status |
|---|---|---|
| 0.1 | CommonMark block and inline parsing, rendering with deal's attributes, print to stdout | Done (live checks in deal pending) |
| 0.2 | GitHub tables, strikethrough, task lists, extended autolinks | Done (live checks in deal pending) |
| 0.3 | Pager: alternate screen, scrolling, mouse wheel, resize reflow, synchronized output | Done (live checks in deal pending) |
| 0.4 | Following links, back stack, search, heading jumps, outline | Done (live checks in deal pending) |
| 0.5 | Config, palette colors, reload on save, `e` to edit, desktop entry | Done (live checks in deal pending) |
| 1.0 | Spec corpus, fuzzing, a week of use, README, tag | In progress alongside `files` |

### note (idea, not scheduled)

Noted 2026-11-28 so it isn't forgotten. No roadmap yet, and no place in the order above.

- A note-taking app taking after Obsidian (plain-text notes in a folder, links between them) and Excalidraw (hand-drawn-looking freehand drawing on a canvas, which is the part to learn most from).
- Notes are written in a new markup language of its own:
  - Markdown-like for ordinary writing.
  - LaTeX-like for math and math diagrams, rendered by the app itself. A real TeX install is too big to depend on.
- mark's CommonMark parser is the natural starting point for the text half.
- Open questions for the roadmap: the language's syntax and how drawings are stored in the same file, GUI on desk vs a terminal front end, and how much of TeX's math layout is worth reimplementing.

---

## Phase 6: Gaps (last)

These are the pieces a laptop daily driver needs that aren't one of the 27 projects. They come after the core apps exist.

| Gap | Current state | Route |
|---|---|---|
| Volume keys | **Not working** (dwl config only has `volume-up_EXAMPLE`) | `vol` + XF86Audio bindings in dwl. Optional stopgap: bind `wpctl` in `config.h` |
| Brightness keys + OSD | Missing | backlight via logind `SetBrightness`, layer-shell popup |
| Idle dim/lock/suspend | Missing | `ext-idle-notify-v1` (dwl supports it) → `lock` |
| Polkit agent | Missing (net's README notes one may be needed) | polkit over D-Bus (sd-bus) |
| Portals | Missing | `xdg-desktop-portal-wlr` for screen sharing; later `files` as the FileChooser backend |
| Display configuration | Missing | `wlr-output-management` (dwl supports it) |
| Night light | Missing | `wlr-gamma-control` (dwl supports it) |
| Battery in bar | Missing | `/sys/class/power_supply` |

---

## The 27 projects

### Existing (7)

| # | Project | What it does | Status |
|---|---|---|---|
| 1 | `dwl` | Wayland compositor: tiling, directional focus and swap, gaps, IPC, wallpaper-derived borders | Daily driver |
| 2 | `bar` | Status bar: workspaces, network status, clock, live palette | Daily driver |
| 3 | `run` | Launcher for desktop entries and `$PATH` commands, with fuzzy search | Daily driver |
| 4 | `wall` | Wallpaper renderer and browser, transitions, palette generation | Daily driver |
| 5 | `net` | Wi-Fi manager TUI over NetworkManager | Daily driver |
| 6 | `deal` | Terminal emulator, native Wayland, tabs, wallpaper colors | 0.13.3, Phase 1 |
| 7 | `lead` | Modal terminal text editor (installed as `editor`) | Early (Phase 0.3) |

### Planned (20)

| # | Project | What it does |
|---|---|---|
| 8 | `desk` | Shared library: config, palette, paths, buffers, fonts, keyboard |
| 9 | `shot` | Screenshots: region, monitor, window; to file and clipboard |
| 10 | `lock` | Screen locker on `ext-session-lock-v1` |
| 11 | `notify` | Notification daemon (`org.freedesktop.Notifications`) |
| 12 | `clip` | Clipboard manager and history |
| 13 | `vol` | Audio and volume control |
| 14 | `power` | Session and power controls: logout, suspend, reboot, shutdown |
| 15 | `mount` | Storage and removable-device manager (udisks2) |
| 16 | `view` | Image viewer |
| 17 | `files` | Graphical file manager with Miller columns and H/J/K/L navigation; roadmap in `docs/files-roadmap.md` |
| 18 | `ctl` | Desktop configuration tool for fonts, theme and shared settings |
| 19 | `music` | Local music player |
| 20 | `video` | Video frontend, with mpv as the playback backend |
| 21 | `read` | PDF and document reader |
| 22 | shell | Lightweight Fish-inspired command interpreter |
| 23 | `login` | Login screen, as a greetd greeter |
| 24 | `pkg` | Build and install tool for the desktop repo |
| 25 | toolkit | Shared GUI widgets, extracted from `desk` once `files` shows what is really common |
| 26 | `mark` | Markdown viewer for the terminal: renders headings, emphasis, lists, tables, code blocks and links with deal's text attributes, in a pager |
| 27 | `note` (working name) | Note-taking app in the spirit of Obsidian and Excalidraw, on its own markup language |
