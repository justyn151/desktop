# mark

`mark` shows markdown in the terminal. It lays a file out for the terminal's width, with real bold and italic, underlined links you can Ctrl+click in deal, shaded code blocks and box-drawn tables, and shows it in its own pager, where you can follow links between files, search, and jump between headings. The config file and the wallpaper's colors come next; what's left is in [docs/mark-roadmap.md](../docs/mark-roadmap.md).

The parser is hand-written and follows CommonMark 0.31.2, passing all 652 of the spec's examples, plus GitHub's tables, strikethrough, task lists and bare-URL autolinks, passing GitHub's 21 examples for them. It's one `mark.c` and one `mark.h` with nothing beyond the C library.

## Build

On Arch Linux the build needs `gcc` and `make`. `make check` also uses `python3` for the CommonMark examples, when it's installed.

```sh
make
make check        # golden outputs, exit codes, the spec's examples, pathological inputs
make install      # /usr/local/bin/mark; honors PREFIX, BINDIR and DESTDIR
```

## Usage

```text
mark [OPTIONS] [FILE]
```

| Option | Meaning |
|---|---|
| `FILE` | The markdown file. `-`, or no argument, reads standard input when it isn't a terminal |
| `--width N` | Lay out for `N` columns (1 to 1000) |
| `--plain` | No escape sequences at all: no colors, attributes or links. Prints instead of paging |
| `--no-pager` | Print instead of paging, even to a terminal |
| `-h`, `--help` | Show help |
| `-v`, `--version` | Show the version |

Without `--width`, text is as wide as the terminal up to 80 columns, so long lines stay readable in a wide window. When the output isn't a terminal, the width is 80.

When standard output is a terminal, mark shows the document in its pager (below). Otherwise it prints the document and exits. Printed output keeps its escape sequences (`mark README.md | less -R` works); `--plain` drops them. Setting `NO_COLOR` drops the colors but keeps bold, italic and underlines.

## The pager

The document opens on the terminal's alternate screen, so quitting leaves the shell as it was, and a status line at the bottom shows the file, the heading you're in and how far through you are (`Top`, `End`, `All` or a percentage). Keys are read from the terminal itself, so `cat notes.md | mark` pages too.

| Key | Action |
|---|---|
| `j`, Down, Enter | Down a line (also `e`, Ctrl+N, Ctrl+E) |
| `k`, Up | Up a line (also `y`, Ctrl+P, Ctrl+Y) |
| Space, `f`, PageDown, Ctrl+F | Down a screen |
| `b`, PageUp, Ctrl+B | Up a screen |
| `d`, Ctrl+D / `u`, Ctrl+U | Down / up half a screen |
| `g`, Home, `<` / `G`, End, `>` | Top / bottom |
| `l`, Right / `h`, Left | Scroll tables wider than the screen sideways |
| Tab / Shift+Tab | Focus the next / previous link; the status line shows where it goes |
| Enter | Follow the focused link (with none focused, down a line) |
| Click | Follow the link under the pointer |
| Backspace | Back to where you were before the last link or jump |
| `/`, `?` | Search forward, backward; `n` and `N` repeat it |
| `]`, `[` | Next, previous heading |
| `o` | Outline of the headings: move with `j`/`k`, Enter jumps |
| Mouse wheel | Three lines per notch; a horizontal wheel scrolls sideways |
| Escape | Clear a focused link or search highlights; with neither, quit |
| `q`, Ctrl+C | Quit |
| `r`, Ctrl+L | Redraw |
| Ctrl+Z | Suspend to the shell (`fg` to come back) |

Following a link to another markdown file (`.md`, `.markdown`) opens it in the pager, at the heading its `#anchor` names; an `#anchor` alone jumps within the document, using GitHub's heading anchors. Other links go to `xdg-open` (from `xdg-utils`), which runs detached; without it, the status line says so and shows the link. Backspace returns through up to 32 places.

Search is plain text over what's on screen, case-insensitive unless the pattern has a capital. Matches are highlighted, the current one in yellow; a phrase broken across two lines isn't found.

Resizing the window reflows the text and keeps the paragraph you were reading at the top. Code blocks wrap; only tables too wide even after their columns shrink run past the right edge, and only they move with `←` and `→`. The pager reports mouse clicks so the wheel works, so in deal select text with Shift held.

## What it draws

| Markdown | Drawn as |
|---|---|
| `# H1` | Bold, accent color, double underline |
| `## H2` | Bold, accent color |
| `### H3` | Bold |
| `####` to `######` | Bold and muted, with the `#` marks kept, dim |
| `*em*`, `**strong**` | Italic, bold (deal draws real faces for both) |
| `` `code` `` | Highlight color on a shaded background |
| `[link](url)` | Underlined in the accent color, as an OSC 8 hyperlink. The URL isn't printed |
| `![alt](src)` | `[image: alt]` in muted italics, linked to the image |
| `> quote` | A muted `│` bar per level, text in italics |
| Lists | `•` `◦` `▪` by depth; ordered lists keep their numbers, right-aligned |
| Code blocks | A shaded band the width of the text, with two columns of padding on each side and a shaded row above and below. Long lines wrap at the last space (or at the cell) and continue under their indentation. Tabs expand to 4 columns |
| `---` | A muted rule across the text width |
| Tables | Box drawn in a muted color, header bold, columns aligned as the delimiter row says (`:--`, `:-:`, `--:`) |
| `~~struck~~` | Strikethrough (one or two tildes) |
| `- [ ]`, `- [x]` | `☐` or `☑` in place of the bullet; done items muted and struck through |
| `www.example.com`, `https://…`, `me@example.com` | Links, without angle brackets, as on GitHub |
| HTML | Shown as written, muted. It isn't rendered |

Text wraps at spaces; a word longer than the line breaks where it must. Widths come from `wcwidth`, the same as deal, so wide characters (CJK, emoji) and combining accents line up.

Colors are the terminal's: blue (4) for the accent, yellow (3) for code, bright black (8) for muted text, and black (0) shading code. deal derives all sixteen from the wallpaper, where 8 is the palette's muted color and 0 the background lifted slightly, so mark already follows the wallpaper there. 0.5 switches to the palette's own colors.

Relative links point at the file next to the document (`file://host/path`), and `#anchor` links point into the document itself. Read from standard input, relative links aren't linked. deal opens only HTTP and HTTPS links on Ctrl+click.

Control characters in the file, such as an escape character, are shown as `�`, so a file can't send escape sequences to the terminal. Invalid UTF-8 shows as `�` as well.

Files up to 64 MiB open. Container blocks (quotes and lists) nest up to 32 levels; anything deeper reads as text.

A table gets each column's natural width when it fits. When it doesn't, each column gets at least its longest word and the rest of the room goes to the columns with the most text, whose cells wrap (if even the words don't fit, the widest columns shrink together, none below 6 columns); if any row then wraps, a rule separates every row. A table that still doesn't fit, with many columns, scrolls sideways in the pager and is cut with `…` when printed.

Only the most common named entities are known (`&copy;`, `&mdash;`, `&nbsp;` and about eighty more); numeric references (`&#169;`) always work, and an unknown name stays as written.

## Exit status

| Status | Meaning |
|---|---|
| 0 | The document was shown |
| 1 | The file couldn't be read, or was larger than 64 MiB |
| 2 | Usage error, or standard input is a terminal |

## Tests

`make check` runs `tests/run.sh`:

- Each `tests/NAME.md` must render to `NAME.txt` with `--plain`, and to `NAME.ansi` with escapes. A name ending in `-N` renders at width `N`.
- Exit codes, standard input, line endings, invalid UTF-8 and `NO_COLOR`.
- `tests/commonmark.py` runs the CommonMark 0.31.2 examples (`tests/spec.json`) through a test build with extra `--html` and `--commonmark` options and compares the HTML, does the same for GitHub's examples of the extensions (`tests/gfm.json`, from cmark-gfm's `test/spec.txt`), then times inputs that make naive parsers quadratic (thousands of unclosed brackets, emphasis or comments, deep nesting, one very long line, huge tables). Each must finish within two seconds.

## Manual checks (0.4)

Checked offline in a pty: following links across files and anchors and coming back, missing files and a missing opener, search and its highlights, heading jumps, the outline and clicks, and the back stack's limit, plus random input under ASan and UBSan.

- [ ] `mark PLAN.md`: Tab to `docs/view-roadmap.md` and Enter opens it; Backspace returns to the same spot. A link to an `#anchor` jumps.
- [ ] `/foot` finds every match, `n` wraps with a message, Escape clears the highlights.
- [ ] `]` steps through headings; `o` lists them, and Enter and clicks jump.
- [ ] With `xdg-utils` installed, an `https://` link opens in the browser and mark keeps running.

## Manual checks (0.3)

Checked offline in a pty: quitting by every key and signal leaves the main screen and terminal modes as they were; arrows, PageDown, Home/End and the wheel; resize keeps the top paragraph; a wide table scrolls sideways to its edge; piped input pages; tiny sizes and random input under ASan and UBSan.

- [ ] In deal, hold `j` in `mark docs/deal-roadmap.md`: smooth, never a half-drawn frame.
- [ ] Resize the window (tile ↔ float): the text reflows and the same paragraph stays at the top.
- [ ] `q`, Ctrl+C and `kill` from another terminal all leave the prompt as it was.
- [ ] The wheel scrolls; Shift+drag selects text.
- [ ] Ctrl+Z drops to the shell with the screen restored; `fg` brings the pager back.
- [ ] `mark file | less -R` and `mark --no-pager file` still print.

## Manual checks (0.2)

Checked offline: GitHub's examples and the golden tests; every README and roadmap here renders at widths 30 and 80, and 4,000 mutated documents with tables, task lists and autolinks parse and render, all under ASan and UBSan.

- [ ] In deal, `mark PLAN.md` shows its tables boxed and aligned, and the roadmaps' wide tables wrap inside their cells with rules between rows.
- [ ] Struck text and done tasks show strikethrough; bare `https://` links open with Ctrl+click.

## Manual checks (0.1)

Checked offline: every README and roadmap in this repository renders without errors at widths 37 and 80; the spec examples render at widths 1, 7 and 30, and 3,000 randomly mutated documents parse and render, all under ASan and UBSan.

- [ ] In deal, `mark README.md` for each README here: headings stand out, code is shaded, links are underlined in the accent color, and Ctrl+click opens an HTTP link.
- [ ] `mark README.md | cat` and `mark --plain README.md` show the same text, with and without escapes (apart from the spaces that fill shaded code lines).
- [ ] `mark --width 40 docs/view-roadmap.md` wraps everything at 40 columns.
- [ ] A file of emoji and Japanese text wraps at the right column in deal.
