# mark roadmap

Implementation plan for mark, the terminal markdown viewer, from 0.1 to 1.0. It expands `mark` in [Phase 5 of PLAN.md](../PLAN.md#mark). It starts while view 1.0's week of use runs.

## Starting point

mark is one `mark.c` and one `mark.h`, built on `desk/` from its first commit, the same way shot and view were. It's a terminal program, not a Wayland client: it reads a markdown file, lays it out for the terminal's width and shows it in its own pager on the alternate screen. When stdout isn't a terminal, it prints the rendered text and exits, like `cat`.

It's written for deal and uses what deal draws. Other terminals get the same escape sequences; the ones they don't understand degrade to plainer text (see "Other terminals" below).

### What already exists

| Piece | Where | mark uses it for |
|---|---|---|
| Raw mode, alternate screen, `TIOCGWINSZ`, `SIGWINCH` and signal cleanup | `net/net.c` (`ppoll` with a signal mask), `lead/editor.c` | The pager (0.3) |
| INI config, config paths, `desk_write_default` | desk (Layer 0) | Config (0.5) |
| Palette | `desk_palette_load` (Layer 0) | Colors that follow the wallpaper (0.5) |
| File watch | `desk_watch_*` (Layer 0) | Reload when the file is saved (0.5) |
| `wcwidth` for cell widths | libc, and what deal uses (`deal/terminal.c`) | Wrapping and table columns, so mark and deal agree on every width |

mark only needs desk's Layer 0. It builds without `-DDESK_WAYLAND`, `-DDESK_IMAGE` or `-DDESK_FONT`, so it links nothing beyond libc.

### What deal gives it

All of these are in deal today (0.10.3 to 0.11.1, see `deal/README.md`):

| deal feature | Sequence | mark uses it for |
|---|---|---|
| Real bold, italic and bold italic faces | SGR 1, 3 | `**strong**`, `*emphasis*`, headings |
| Strikethrough | SGR 9 | `~~deleted~~` |
| Underline styles: single, double, curly, dotted, dashed | SGR 4:1 to 4:5 | Links (single), headings (double for level 1), focused link (dashed) |
| Underline color | SGR 58:2::R:G:B | Links underlined in the accent color while the text keeps its own |
| Overline | SGR 53 | Not planned; available for a heading style if underlines look busy |
| OSC 8 hyperlinks, Ctrl+click opens HTTP(S) | `ESC ] 8 ; ; URL ST` | Every link is clickable even in plain `mark file \| cat` output |
| Synchronized output | `ESC [ ? 2026 h/l` | One complete frame per scroll, no tearing |
| SGR mouse reporting, Shift override for selection | `ESC [ ? 1000 h`, `ESC [ ? 1006 h` | Wheel scrolling and clicking links in the pager |
| 256-color and RGB | SGR 38/48 | Palette colors, code block background |

deal has no alternate-scroll mode (`?1007`), so the wheel only reaches mark through mouse reporting. That means selecting text in the pager needs Shift, as in other TUIs. `[pager] mouse = false` turns reporting off for people who'd rather select freely and scroll with keys.

### Parser

A hand-written parser, no cmark or md4c, per the ground rules. It follows the CommonMark spec (0.31) in two passes, the way the spec describes:

1. **Blocks:** read lines into a tree of containers (document, block quote, list, list item) and leaves (paragraph, heading, thematic break, code block, HTML block, table). Lazy continuation lines and list tightness follow the spec.
2. **Inlines:** parse each leaf's text into spans with the delimiter-stack algorithm for `*` and `_`, plus code spans, links and images (inline, reference and autolinks), backslash escapes, entities and hard breaks.

The aim is to render the markdown people actually write correctly, not to pass all 650 spec examples. The spec's examples become a test corpus in 1.0, and the pass rate is recorded rather than required. Known gaps get listed in the README.

GitHub extensions, since most READMEs use them: tables, strikethrough, task lists, extended autolinks (bare `https://…` and `www.…`), and footnotes after 1.0.

### Layout

1. Parse to the block tree once per file (re-parsed on reload).
2. Lay out the tree for a width into a list of display lines. Each line is a run of cells with attributes (face, colors, underline style and color, link ID). Layout runs again on resize and keeps the reader's place by anchoring to the block at the top of the screen.
3. Draw a screen by emitting only the visible lines, wrapped in synchronized output.

Text width is `min(terminal width, [layout] width)`, 80 by default, so long lines stay readable on a wide window. Wrapping is greedy at spaces, with wide characters counted by `wcwidth` and words longer than the line broken by cells.

### Rendering

| Element | Drawn as |
|---|---|
| `# H1` | Bold, accent color, double underline, a blank line after |
| `## H2` | Bold, accent color |
| `###` to `######` | Bold; level 3 in the foreground color, 4 to 6 muted with the `#` marks kept dim, so depth stays visible |
| Paragraph | Wrapped to the text width |
| `*em*`, `**strong**`, `***both***` | Italic, bold, bold italic faces |
| `~~strike~~` | Strikethrough |
| `` `code` `` | Highlight color on a slightly shaded background |
| Link | Text underlined in the accent color, wrapped in OSC 8. The URL isn't printed inline |
| Image | `[image: alt]` in muted italics, linked to its source with OSC 8 |
| Block quote | A `│` bar in muted color per level, text italic |
| Lists | `•` `◦` `▪` by depth; ordered lists keep their numbers, right-aligned to the widest |
| Task list | `☐` / `☑`, checked items muted and struck through |
| Fenced or indented code | Shaded band the full text width, two columns of padding on each side and a shaded row above and below. Long lines wrap: at the last space, else at the cell, continuing under the line's indentation. No language label |
| Table | Box drawing in the border color, header bold, columns aligned as written (`:--`, `:-:`, `--:`) |
| Thematic break | `─` across the text width, muted |
| HTML block or inline HTML | Shown as-is in muted color. No HTML rendering |
| Hard break | A line break; soft breaks become spaces |

No syntax highlighting in code blocks before 1.0 (see "After 1.0").

### Other terminals

mark only writes standard sequences, and the ones deal adds degrade quietly elsewhere: terminals without colon-form underline styles show a plain underline or none, and terminals without OSC 8 show the link text without a link. `NO_COLOR` turns off colors but keeps bold, italic and underlines. `--plain` turns off every attribute, for piping into files.

---

## 0.1: Parse and print

### Command line

```text
mark [OPTIONS] [FILE]
  FILE          Markdown file. "-" or no argument reads stdin when it isn't a terminal.
  --width N     Text width (default: terminal width up to 80, or 80 when not a terminal)
  --plain       No colors or attributes
  -h, --help    Usage
  --version     Version
```

No argument with stdin on a terminal prints usage and exits 2. A file that can't be read prints the error and exits 1. Markdown never fails to parse: every input renders as something.

### Work

1. Read the whole file into memory (refuse above 64 MiB with a message). Treat input as UTF-8; replace invalid bytes with U+FFFD; normalize CRLF and lone CR to LF; replace NUL with U+FFFD as the spec says. Tabs expand to 4-column stops for block structure.
2. Block pass: paragraphs, ATX and setext headings, thematic breaks, block quotes, bullet and ordered lists (with tight and loose), fenced and indented code blocks, HTML blocks (kept as text), link reference definitions.
3. Inline pass: code spans, emphasis and strong (delimiter stack, including the rule of 3), links and images (inline and reference, with titles), `<autolinks>`, backslash escapes, entities (named ones from a small table of the common ones, plus numeric), hard and soft breaks.
4. Layout and rendering from the table above, without tables and task lists (0.2), printed straight to stdout with the attributes and OSC 8 links. Output always ends with SGR 0.
5. Width: `--width`, else the terminal's when stdout is one, else 80. Call `setlocale(LC_CTYPE, "")` so `wcwidth` knows UTF-8.
6. `make`, `make install` following bar's `PREFIX`/`BINDIR`/`DESTDIR`, `/mark/mark` in `.gitignore`, mark in the root README's table.
7. A test directory `mark/tests/` with small `.md` files and the expected `--plain` output, run by `make check`.

### Edge cases

- Deep nesting: cap block nesting at 32 levels and emphasis delimiter runs at the spec's limits, so a crafted file can't recurse the stack away. Anything deeper renders as text.
- Pathological inputs from the spec's known slow cases (many unclosed `[`, `*` or `` ` ``) must stay linear-ish. Time them in `make check`.
- Very long lines (a minified one-line file) wrap without quadratic cost.
- Wide characters (CJK, emoji) never split across a wrap and never misalign a table.
- A file without a trailing newline renders the last line.

### As built

- The parser passes all 652 examples of CommonMark 0.31.2, so the spec corpus planned for 1.0 came forward: a test build (`-DMARK_TEST`) adds `--html`, and `make check` compares it with `tests/spec.json` through `tests/commonmark.py`. The shipped binary has no HTML output.
- The pathological inputs are cmark's list plus a few of mark's own (nested images, one 2 MB line, many short lines, wide characters). Each finishes in well under the two seconds `make check` allows, in both the HTML and the terminal output.
- Inline rendering walks the tree without recursion, since emphasis and image alt text can nest as deep as a line is long. Block nesting stops at 32 levels, as planned.
- Until 0.5 brings the palette, colors are ANSI indices: accent 4, inline code 3, muted 8, code shading 0. deal derives all sixteen from the wallpaper (8 is the muted role, 0 the background lifted a little), so in deal they follow it already. The underline color is sent as `58:5:N` and underline styles as `4:2`, colon forms that terminals without them read as one unknown parameter.
- Code blocks have no padding rows. The language label sits on a shaded row of its own above the code, and only when there is one. So the plain text and the styled text are the same lines, apart from the spaces that fill shaded code lines in styled output.
- Changed after 0.2: the label is gone, the band has a shaded row above and below and two columns of padding on the right as well as the left, and long code lines wrap instead of being cut with `…`. Plain output has no padding rows, so it still matches the styled text line for line apart from those two rows.
- Piped output keeps its escapes; only `--plain` removes them. That's what makes `mark file | cat` show links, as the "What deal gives it" table says.
- desk isn't linked yet: 0.1 uses nothing from it. It joins in 0.5 with the config file, palette and file watch.
- `-v` works as well as `--version`, like view.

### Manual checks

- `mark README.md` for every README in this repo looks right: headings stand out, code is shaded, links are underlined in the accent color and Ctrl+click opens the HTTP ones in deal.
- `mark README.md | cat` and `mark --plain README.md` print the same text, with and without escapes.
- `mark --width 40 docs/view-roadmap.md` wraps everything at 40 cells.
- A file of emoji and Japanese text wraps at the right column.

---

## 0.2: GitHub extensions

- **Tables:** parse the delimiter row and alignment, cell inlines, escaped pipes (`\|`). Column widths are the widest cell; when the table is wider than the text width, shrink the widest columns first and wrap their cells inside the box. Tables wider than the screen even after shrinking (many narrow columns) keep a minimum of 6 cells per column and scroll sideways in the pager.
- **Strikethrough:** `~~text~~` (and single `~`, as GitHub accepts it).
- **Task lists:** `- [ ]` and `- [x]` at the start of a list item.
- **Extended autolinks:** bare `https://`, `http://` and `www.` URLs and email addresses, with GitHub's trailing punctuation rules.
- **Fenced code info strings:** the first word was shown as a language label; dropped after 0.2 as noise. It returns only as the key for syntax highlighting, if that lands.

### As built

- Pulled ahead of the pager: nearly every document in this repository has tables, and 0.1 showed them as run-together paragraphs. The pager doesn't depend on them.
- Tables follow cmark-gfm: the paragraph's last line becomes the header when its cell count matches the delimiter row, which needs at least one pipe; rows run until a blank line or another block starts; short rows get empty cells and extra cells are dropped; `\|` is a pipe in a cell, even inside code. Tables have at most 256 columns.
- Column widths: each column's widest line when the table fits. Otherwise each column gets its longest word and the room left is shared in proportion to what each still wants, so a column of short labels doesn't take half the width. If even the longest words don't fit, the widest columns are capped together, none below 6 cells or its own width. When a body row still wraps, a rule separates every body row, so rows stay apart; tables whose rows fit on one line keep only the rule under the header. A table still too wide is cut with `…` until the pager can scroll it sideways.
- Single and double tildes strike through, a run pairing only with a run of the same length; three or more stay text.
- Extended autolinks also take `ftp://`, as GitHub's examples do. They're found after inline parsing, in text outside links, so `www.a.com/x_y_z` is one link however the underscores were read.
- Task lists replace the bullet with `☐` or `☑`; in ordered lists the box follows the number. Done items are muted and struck through.
- `make check` adds GitHub's 21 examples for these extensions (`tests/gfm.json`, from cmark-gfm's `test/spec.txt`), and the test build's `--commonmark` turns the extensions off for the CommonMark examples. Pathological inputs gained many rows, many columns, a wide table, tildes, bare links and at signs.

### Manual checks

- A table with a long last column wraps inside its cells; box lines meet at every joint, including with wide characters in a cell.
- `PLAN.md`'s tables (the 26 projects) fit an 80-column window and stay aligned.
- Task lists in the plan show checked and unchecked boxes.
- A bare URL in text becomes a clickable link without picking up a trailing `.` or `)`.

---

## 0.3: The pager

When stdout is a terminal, mark shows the document in its own pager instead of printing it. Writing a pager instead of piping to `less -R` is what lets mark reflow on resize, focus and follow links, and jump between headings.

### Controls

| Key | Action |
|---|---|
| `j` / Down, `k` / Up | Scroll one line |
| Space / PageDown / `f`, `b` / PageUp | Scroll one screen |
| `d`, `u` | Scroll half a screen |
| `g` / Home, Shift+`g` / End | Top, bottom |
| `→` / `l`, `←` / `h` | Scroll a table that is wider than the screen (the one at the top of the screen); code wraps instead |
| Mouse wheel | Scroll three lines per notch |
| `q`, Escape, Ctrl+C | Quit |
| `r`, Ctrl+L | Redraw |

### Design

- Raw mode with `ISIG` off (Ctrl+C handled as a key), alternate screen, hidden cursor, mouse reporting (`?1000` + `?1006`) unless disabled. Restore everything on exit, on `SIGINT`/`SIGTERM`/`SIGHUP`, and print nothing to the main screen, like `less`.
- Event loop on `ppoll` with `SIGWINCH` unblocked only while waiting, as in net. Resize re-runs layout and keeps the block at the top in place.
- Each frame: `?2026h`, cursor home, every visible line (clear to end of line after each), status line, `?2026l`. Redraw only what scrolled when the change is a few lines (scroll region and `ESC [ S`/`T`), otherwise the whole screen.
- Status line at the bottom in reverse video: file name, the current heading (the last heading above the top line), and position as a percentage or `top`/`end`.
- A document shorter than the screen is still shown in the pager, so the keys behave the same. `--no-pager` (or `[pager] enable = false`) prints it instead.
- Escape sequences in keyboard input: arrows, Home/End, PageUp/PageDown in both `CSI` and `SS3` forms, and SGR mouse reports. A lone Escape is a key after 25 ms with no follow-up.

### Manual checks

- Scrolling a long file (`docs/deal-roadmap.md`) with `j` held is smooth and never shows a half-drawn frame.
- Resizing the deal window (tile ↔ float) reflows the text and keeps the same paragraph at the top.
- Ctrl+C, `q` and `kill` all leave the shell prompt exactly as it was.
- With mouse reporting on, the wheel scrolls and Shift+drag selects text in deal.
- `mark file | less` still works (no pager, plain escapes).

---

## 0.4: Links, search and navigation

### Controls

| Key | Action |
|---|---|
| Tab, Shift+Tab | Focus the next or previous link on screen (dashed underline, reverse video) |
| Enter | Follow the focused link |
| Click on a link | Follow it (with mouse reporting on) |
| Backspace | Back to the previous document or position |
| `/`, `?` | Search forward or backward (plain text, case-insensitive unless the pattern has a capital) |
| `n`, Shift+`n` | Next, previous match |
| `]`, `[` | Next, previous heading |
| `o` | Outline: a list of headings to jump to, with `j`/`k` and Enter |

### Design

- **Following a link:**
  - `#anchor`: jump to the heading with that GitHub-style slug (lowercase, spaces to `-`, punctuation dropped, `-1`, `-2` for duplicates).
  - A relative path to a `.md` file (with or without `#anchor`): open it in mark, resolved against the current file's directory, and push the current document and position on a back stack (up to 32 entries).
  - Anything else (`http(s)://`, `mailto:`, other files): run `[links] open` with the URL as one argument, detached in its own session with stdin and stdout on `/dev/null` (shot's double fork from view 0.5). The default is `xdg-open`; it isn't installed yet, so with no opener mark shows the URL in the status line instead of failing silently.
- **Search** runs on the rendered text, not the source, so it finds what's on screen. Matches are highlighted with the selection colors. The search prompt is the status line, with Backspace, Ctrl+U and Escape to cancel. Patterns are plain text; regex waits until it's missed.
- **Outline** is an overlay drawn in the pager, heading levels indented, the current section marked.

### Manual checks

- From `PLAN.md`, Enter on `docs/view-roadmap.md` opens it, Enter on an `#anchor` link jumps, Backspace returns to the same spot in the plan.
- `/foot` finds every match in `PLAN.md`, `n` wraps with a message in the status line.
- `]` steps through headings in order; `o` lists all of them and jumps.
- An `https://` link with `open` set to `firefox` opens it and mark keeps running.

---

## 0.5: Config, theme and desktop integration

- Config through desk's INI parser at `~/.config/mark/mark.conf`, written from a sample on first run like view does. Precedence: command line, mark.conf, `desktop.conf` once Phase 3 step 3 exists, built-in defaults.

  ```ini
  [layout]
  width = 80                      ; maximum text width; 0 = the terminal's
  margin = 2                      ; blank columns on the left

  [colors]
  source = palette                ; palette | ansi
  ; palette uses wall's roles in RGB; ansi uses the 16 terminal colors

  [pager]
  enable = true
  mouse = true
  wheel_lines = 3

  [links]
  open = xdg-open
  ```

- `source = palette` maps roles to elements: `accent` for headings and link underlines, `highlight` for inline code, `muted` for quotes, rules, dim heading marks and labels, `border_inactive` for table lines, `selection_*` for search matches and the focused link, and a shade between `background` and `foreground` for code blocks. Text without its own color uses the terminal's default foreground, so it matches deal exactly. The palette is re-read through `desk_watch` and the pager redraws when the wallpaper changes, like deal.
- `source = ansi` uses colors 0 to 15 only, for terminals that set their own scheme.
- **Reload:** `desk_watch` on the open file. When it changes (saved from lead), re-parse, re-layout and keep the top block in place, so mark works as a live preview in a second tile.
- `e` opens the file in `$VISUAL`, `$EDITOR` or `editor` at the source line of the top block, passed as `+N FILE`, and mark resumes after the editor exits, re-reading the file. lead takes only a file name today, so this needs a small lead change (accept `+N`), committed separately.
- Desktop entry `mark.desktop` with `Terminal=true` and `MimeType=text/markdown;`, so `run` launches it in deal (`terminal = deal -e`). `make default` registers it with `xdg-mime` when it exists, as in view.

### Manual checks

- Changing the wallpaper recolors an open mark within a second.
- Saving the file in lead in another tile updates mark without losing the scroll position.
- `e` opens lead at the right line and returns to the same place.
- `run` lists mark and opens a file in a new deal window.
- A config typo reports the line number and falls back to defaults.

---

## 1.0: Daily driver

- Every 0.x checklist passes in deal on dwl.
- Run the CommonMark spec examples (`spec.json` from the spec repository, kept in `mark/tests/`) through an HTML-free comparison of the block and inline structure, and record the pass rate in the README. Fix whatever real files hit.
- Fuzz the parser and layout with AFL++ or libFuzzer under ASan/UBSan for a few hours, with the spec examples as the seed corpus, and check the pathological inputs stay fast.
- A week as the way to read every README and roadmap in this repo, with annoyances in `docs/mark-papercuts.md`.
- Exit codes documented (0 normal, 1 couldn't read the file, 2 usage).
- README documents every option, key and config setting, and the known gaps from the spec.
- Tag `mark-1.0.0`.

## After 1.0 (unscheduled)

- Syntax highlighting in code blocks, for a few languages (C, shell, INI, diff) with a small hand-written tokenizer, in palette colors
- Footnotes (`[^1]`) and GitHub alerts (`> [!NOTE]`)
- Copy the code block under the cursor with OSC 52 (deal supports it since 0.11.1)
- Show images inline: deal has no graphics protocol, so this would mean a deal feature first. Until then, Enter on an image opens it in `view`
- Front matter (`---` YAML at the top) shown as a muted block or hidden
- Following links to non-markdown text files (show them as a code block)
- Math, Mermaid and HTML rendering: no

## Milestone summary

| Version | Theme | Status |
|---|---|---|
| 0.1 | CommonMark block and inline parsing, rendering with deal's attributes, print to stdout | Done (652/652 spec examples; live checks in deal pending) |
| 0.2 | GitHub tables, strikethrough, task lists, extended autolinks | Done (21/21 GitHub examples; live checks in deal pending) |
| 0.3 | Pager: alternate screen, scrolling, mouse wheel, resize reflow, synchronized output | Not started |
| 0.4 | Following links, back stack, search, heading jumps, outline | Not started |
| 0.5 | Config, palette colors, reload on save, `e` to edit, desktop entry | Not started |
| 1.0 | Spec corpus, fuzzing, a week of use, README, tag | Final |
