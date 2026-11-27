# mark

`mark` shows markdown in the terminal. Version 0.2 parses a file and prints it laid out for the terminal's width, with real bold and italic, underlined links you can Ctrl+click in deal, shaded code blocks and box-drawn tables. The pager, link following and the config file come next; what's left is in [docs/mark-roadmap.md](../docs/mark-roadmap.md).

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
| `--plain` | No escape sequences at all: no colors, attributes or links |
| `-h`, `--help` | Show help |
| `-v`, `--version` | Show the version |

Without `--width`, text is as wide as the terminal up to 80 columns, so long lines stay readable in a wide window. When the output isn't a terminal, the width is 80.

mark prints the document and exits. The output keeps its escape sequences when piped (`mark README.md | less -R` works); `--plain` drops them. Setting `NO_COLOR` drops the colors but keeps bold, italic and underlines.

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
| Code blocks | Indented two columns on a shaded band the width of the text; the language, when given, top right. Long lines are cut with `…`. Tabs expand to 4 columns |
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

A table gets each column's natural width when it fits. When it doesn't, each column gets at least its longest word and the rest of the room goes to the columns with the most text, whose cells wrap (if even the words don't fit, the widest columns shrink together, none below 6 columns); if any row then wraps, a rule separates every row. A table that still doesn't fit, with many columns, is cut with `…` until the pager can scroll it sideways.

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
