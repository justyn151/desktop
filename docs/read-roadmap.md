# read roadmap

Implementation plan for read, the document viewer (project 21 in [PLAN.md](../PLAN.md#the-30-projects)), from 0.1 to 1.0. It is pulled forward in Phase 5, right after `vol` and `blue`. Nothing here is built yet.

## Starting point (2026-11-28)

read is one `read.c` and one `read.h`, built on `desk/` like `view` and `files`: a normal xdg-shell window (app ID `read`), mouse first, with keys for everything. It opens PDFs first, then the other formats its library already understands.

There is no document viewer on the system now. `application/pdf` has no default handler, so PDFs from the browser or files open nowhere, or in Helium.

### Decisions

- **libmupdf for parsing and rendering, not poppler.** MuPDF is one C library with a plain C API. It renders a page straight into an RGB(A) pixmap we can copy into a desk canvas, and it covers PDF, XPS, EPUB, MOBI, FB2, CBZ and SVG with the same calls, so the other formats come almost free. poppler would bring cairo and either glib or a C++ API for PDF alone. libmupdf is in `extra` (1.28.6). It's large (about 56 MiB with tesseract and the other dependencies), but it's one package, and it parses and renders PDFs; we don't write a PDF parser. Revisit only if the package goes away.
- **A window on desk, not a terminal viewer.** Pages are images, and terminal image protocols aren't worth it here. read uses the window, keyboard and font layers (`-DDESK_WAYLAND -DDESK_WINDOW -DDESK_KEYBOARD -DDESK_FONT`). The font is for read's own interface (page number, search field, outline). The documents carry their own fonts, which MuPDF renders.
- **Rendering on a worker thread.** A heavy page (scanned, vector-dense maps) can take hundreds of milliseconds. The window shows the last good render scaled, or a blank page with its real size, while the thread works. MuPDF needs one `fz_context` per thread, cloned from the main one with lock callbacks (`fz_locks_context`). Pages are drawn through a display list, built once per page and then rendered at any zoom.
- **Scale-correct like view.** Pages are rendered at physical pixels (zoom × output scale), so text is sharp at 1.25× and 1.5×.
- **Reflowable formats are laid out, not paged.** EPUB, MOBI and FB2 have no fixed pages. MuPDF lays them out for a page size and font size we choose (`fz_layout_document`), and read keeps your place across relayouts with MuPDF's location bookmarks.

### What already exists

| Piece | Where | read uses it for |
|---|---|---|
| xdg-shell window, fractional scale, shm buffers | desk (`DeskWindow`, `DeskBuffer`) | Every frame (0.1) |
| Keyboard, key repeat | desk (`DeskKeyboard`, `DeskRepeat`) | Keys and held scrolling (0.1, 0.2) |
| Fonts with Nerd Font fallback, text width and fitting | desk (`-DDESK_FONT`) | Page counter, outline, search field (0.1, 0.3, 0.4) |
| Canvas fill, clip, blit | desk | Page backgrounds, gaps, the sidebar |
| Zoom around the pointer, pan, mip-level shrinking | `view/view.c` | Zoom and pan (0.2) |
| Cursor shapes, pointer and wheel handling, tabs, the sidebar with a draggable edge, the text field | `files/files.c` | Pointer handling, the outline sidebar (0.3), the search field (0.4) |
| Clipboard (`wl_data_device` source) | `files/files.c` (0.4) | Copying selected text (0.4) |
| A worker thread handing results back to the loop | `files/files.c` (copy and folder sizes) | Page rendering (0.2) |
| INI config, palette, file watch, state paths | desk | Config, dark mode, reload, last page per file (0.5, 0.6) |
| Float rule and centering by exact app ID | dwl (`net`, `blue`, `vol`) | Not needed: read tiles like a browser. Opened from files, it already floats over it |

### What desk gains

Only when a second app really shares it (desk's rule). Candidates:

| Piece | Now in | Shared with |
|---|---|---|
| Clipboard source and offer | files | read (copy text), later `clip` and `note` |
| Single-line text field (cursor, selection, editing keys) | files (path and search fields) | read (search, go to page). This is where the toolkit (project 25) starts |
| Worker thread with an eventfd back to the main loop | files | read |

The text field and clipboard are the first real pieces of the toolkit. read is the second app that needs them, so they move to desk when read 0.4 lands, not before.

---

## 0.1: Open a PDF and turn pages

### Command line

```text
read [-p PAGE] FILE
  FILE    PDF (the other formats come in 0.5). "-" reads stdin.
```

No file prints usage and exits 2. A file MuPDF can't open prints its message and exits 1.

### Work

- Open with `fz_open_document`, count pages, render the current one with `fz_new_pixmap_from_page` at fit-width zoom × scale, convert to the buffer's format, center it on a background.
- Page Down/Page Up, Space/Shift+Space, Left/Right and the wheel turn pages. Home/End go to the first or last page.
- The window title is `name — page N of M`.
- Rendering happens on the main thread in this release. The worker thread comes in 0.2.

### Checks

- [ ] A text PDF, a scanned PDF and a large one (500+ pages) open; the first page is sharp at 1× and fractional scale
- [ ] A damaged or non-PDF file says why and exits 1
- [ ] `-p 40` opens on page 40; out of range clamps

## 0.2: Continuous scrolling and zoom

- Pages stacked vertically with a gap, scrolled smoothly by the wheel, touchpad, arrows, j/k and dragging with the middle button.
- Zoom: Ctrl+wheel and Ctrl+/- around the pointer; `w` fit width, `z` fit page, `0` 100% (actual size at 96 dpi). The zoom is kept when turning pages.
- Rendering moves to a worker thread with its own cloned `fz_context`. Visible pages are rendered first, then one screen ahead and behind. A cache holds rendered pages at the current zoom, bounded in memory (oldest first out). While zooming, the old renders are scaled until the new ones arrive.
- Display lists are cached per page, so re-rendering at a new zoom doesn't parse the page again.
- A page counter in a corner (`12 / 340`) while scrolling, fading after a second.

### Checks

- [ ] Holding j through a 500-page document stays smooth; memory stays bounded
- [ ] Ctrl+wheel keeps the point under the pointer still
- [ ] A slow page (a big scanned map) never blocks scrolling

## 0.3: Navigation

- **Outline:** a sidebar (F9 or the toolbar button) with the document's table of contents (`fz_load_outline`), folded and unfolded by click, the current section highlighted as you scroll. The edge can be dragged, like files' sidebar.
- **Go to page:** `g` or Ctrl+G opens a small field for a page number or a page label (`iv`, `A-3`).
- **Links:** internal links jump; external ones open with `xdg-open`. The pointer turns into a hand over links.
- **History:** Alt+Left/Right and the mouse's side buttons go back and forward across jumps, as in files.

### Checks

- [ ] A book with a deep outline: clicking entries jumps there, the current section follows scrolling
- [ ] A paper's citation links jump and Back returns to the same spot
- [ ] A web link opens in the browser

## 0.4: Text: search, select, copy

- **Search:** Ctrl+F or `/` opens a field. Matches are highlighted on every rendered page in the accent color, Enter/n and Shift+Enter/N step through them, and the count shows (`3 of 41`). Search runs on the worker thread page by page, so a long document doesn't freeze, and results appear as they're found.
- **Select:** dragging selects text in reading order (`fz_stext_page`), double-click a word, triple-click a line. Ctrl+C copies it, and the primary selection is set too, like deal.
- The text field and clipboard code move from files to desk here (see above).

### Checks

- [ ] Searching a 500-page book for a common word stays responsive and finds what `mutool` finds
- [ ] A two-column paper copies in reading order, not across columns
- [ ] Copied text pastes into deal, lead and Helium

## 0.5: More documents

- **Formats:** EPUB, MOBI, FB2, XPS, CBZ and SVG, all through MuPDF. Reflowable books get a font size (Ctrl+/- changes it instead of zooming) and a page width from the window; resizing relays out and keeps your place.
- **Passwords:** an encrypted PDF asks for its password in the window.
- **Reload:** when the file changes on disk (desk's file watch), read reloads it and keeps the page and zoom, for LaTeX and Typst workflows. A half-written file doesn't break it: the old document stays until the new one opens.
- **Dark reading:** `i` toggles a recolored mode, where page white becomes the palette background and black becomes the foreground, applied to the rendered pixmap (images can be left alone with MuPDF's tinting). Off by default.

### Checks

- [ ] An EPUB opens, reflows on resize and font change, and stays on the same paragraph
- [ ] A comic (CBZ) shows one image per page
- [ ] Recompiling a LaTeX paper while it's open reloads it on the same page
- [ ] A locked PDF opens with the right password and refuses a wrong one

## 0.6: Config and integration

- `~/.config/read/read.conf`: `[view] zoom` (fit-width, fit-page or a percent), `page_gap`, `dark` (off, on, follow), `background`; `[render] cache_mb`; `[outline] show`, `width`.
- **Remembered place:** the last page and zoom per file, in `$XDG_STATE_HOME/read/places`, keyed by path and size and modification time. A file that changed opens on the same page if it still exists.
- Colors from the wallpaper's palette, live.
- `read.desktop` with the document MIME types, and `make default` setting it for `application/pdf`, `application/epub+zip` and the others in `mimeapps.list`, so files, Helium's downloads and `xdg-open` open them in read.
- files' right-click "Open with" already lists it through the desktop entry.

### Checks

- [ ] Reopening a book returns to the last page
- [ ] Double-clicking a PDF in files opens read floating over files
- [ ] A PDF downloaded in Helium opens in read from the downloads list

## 1.0: Daily driver

- The checklists above in a fresh session.
- A week as the only PDF and EPUB reader: papers, manuals, a long book, scanned forms. Annoyances go in `docs/read-papercuts.md`.
- Fuzz read's own input: the places file, the command line, the go-to-page field. MuPDF's parsers are fuzzed upstream (OSS-Fuzz), so read's job is to survive its errors, with every `fz_try` in place and a broken page shown as an error box instead of a crash.
- ASan/UBSan run through the sample set, including the worker thread under ThreadSanitizer.
- README, tag `read-1.0.0`.

## After 1.0 (unscheduled)

- Thumbnails: a page grid in the sidebar.
- Two pages side by side (book spread), and a presentation mode (fullscreen, one page, click to advance).
- Forms and annotations (highlight, note), saved with MuPDF's incremental save.
- Printing, once there is a print path on the desktop at all.
- SyncTeX: click in the PDF to jump to the source line in lead, and back.
- Opening from `note` and showing PDFs in `files`' previews.

## Milestone summary

| Version | Milestone |
|---|---|
| 0.1 | One PDF, turn pages, fit width, sharp at any scale |
| 0.2 | Continuous scrolling, zoom around the pointer, rendering on a worker thread with a bounded cache |
| 0.3 | Outline sidebar, go to page, links, back and forward |
| 0.4 | Search with highlights, text selection and copy; text field and clipboard move into desk |
| 0.5 | EPUB, MOBI, FB2, XPS, CBZ, SVG with reflow; passwords; reload on change; dark reading |
| 0.6 | Config, remembered place per file, palette, desktop entry and default for documents |
| 1.0 | Checks, a week of use, fuzzing and sanitizers, README, tag |
