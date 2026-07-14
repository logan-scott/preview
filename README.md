# preview

A cross-platform CLI document viewer. `preview <file>` opens a native window
and renders the file — like macOS Quick Look, but scriptable and portable.

```
preview report.docx    # formatted document
preview photo.png      # image
preview notes.md       # rendered markdown
preview data.csv       # table
preview paper.pdf      # scrollable pages
```

Everything is converted to HTML/CSS and displayed in the OS's built-in web
view (WKWebView on macOS, WebKitGTK on Linux) — one rendering surface, one
converter per file type.

## Supported formats

| Format | How it renders |
|---|---|
| Markdown (`.md`) | md4c → HTML, GitHub-style CSS, light/dark |
| Images (`.png .jpg .gif .webp .bmp .svg .ico .avif`) | native `<img>` via data URI |
| Images (`.tga .psd .hdr .pnm .ppm .pgm`) | decoded with stb_image |
| Plain text / source code | `<pre>` with syntax highlighting |
| JSON | pretty-printed |
| CSV / TSV | HTML table |
| HTML | rendered directly |
| PDF | mupdf renders pages to PNG (first 200 pages); falls back to bundled pdf.js when built without mupdf |
| DOCX | OOXML → HTML (paragraphs, headings, b/i/u/strike, sub/superscript, hyperlinks, nested lists, tables incl. colspan, inline images) |
| XLSX | one table per sheet; shared strings and cached formula values |
| PPTX | one card per slide: title, text, images (no positional layout) |

## Building

### macOS

```sh
xcode-select --install   # if you don't have clang yet
brew install mupdf       # optional, enables PDF rendering
make
```

### Linux

Needs WebKitGTK development headers (any of webkitgtk-6.0, webkit2gtk-4.1,
or webkit2gtk-4.0 — the Makefile picks the newest present):

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev
sudo apt install libmupdf-dev   # optional, enables PDF rendering

# Fedora
sudo dnf install gcc gcc-c++ make pkg-config gtk3-devel webkit2gtk4.1-devel mupdf-devel

# Arch
sudo pacman -S base-devel webkit2gtk-4.1 libmupdf

make
```

### PDF support: mupdf or the pdf.js fallback

PDF works in **every** build. mupdf, when present, renders pages natively
to sharp PNGs. When it's absent, `preview` falls back to a bundled copy of
**pdf.js** that renders the PDF client-side in the web view — no native
dependency, identical on macOS and Linux, just a larger binary (~1.4 MB
of embedded JS) and client-side rendering.

The Makefile resolves mupdf in this order (falling back to pdf.js if none
is found):

1. `make NO_PDF=1` — skip mupdf and use the pdf.js fallback.
2. `make MUPDF_PREFIX=/some/prefix` — a prefix containing `include/mupdf`
   and `lib/libmupdf.*`. Use this to link a static, from-source build
   (`make -C mupdf prefix=/some/prefix install`) for a fully portable
   binary.
3. macOS: `brew --prefix mupdf` (Homebrew ships dylibs, so the resulting
   binary depends on `/opt/homebrew/opt/mupdf/lib`).
4. Linux: `pkg-config mupdf`. On distros whose libmupdf-dev lacks a
   pkg-config file, use `MUPDF_PREFIX=/usr` (and if the static archive
   needs extra system libs, append them via `LDLIBS`).

Everything else (md4c, miniz, stb_image, highlight.js, pdf.js) is embedded
in the binary; the only unconditional dynamic dependencies are the system
web view and libc/libc++.

**License note:** mupdf is AGPL-licensed. Building `preview` with mupdf
and distributing the binary makes the combined work subject to the AGPL.
Build with `NO_PDF=1` for a binary that is free of that obligation and
still renders PDFs (via pdf.js, Apache-2.0). All other vendored libraries
are MIT/BSD.

## Installing

```sh
make
sudo make install                 # /usr/local/bin/preview + man page
make install PREFIX=$HOME/.local  # or a user-local prefix (no sudo)
```

`PREFIX`, `BINDIR`, `MANDIR`, and `DESTDIR` are all overridable;
`make uninstall` removes what `install` placed.

### Homebrew

The formula lives in this repo (not in homebrew-core), so tap it, then
install. Homebrew 6.0+ requires a one-time `brew trust` before a
third-party tap's formula will run:

```sh
brew tap logan-scott/preview https://github.com/logan-scott/preview
brew trust logan-scott/preview
brew install preview
```

(`brew install preview` on its own won't work — Homebrew only searches
homebrew-core and your tapped repos, and this formula lives in neither
until you tap it.) The formula depends on mupdf for PDF support and
builds against the system web view. `brew upgrade preview` picks up new
tagged releases.

## Usage

```
preview [options] <file>

  -h, --help          show help
  -V, --version       print version
  -w, --watch         re-render when the file changes on disk
  --dump-html         print generated HTML to stdout (no window)
  --close-after <ms>  auto-close the window; used for testing
```

With `--watch`, the window reloads automatically whenever the file is
saved — handy for editing Markdown or code alongside a live preview.

Keys in the viewer window: **Esc** closes, **space / arrows / PgUp /
PgDn** scroll. The color scheme follows the OS light/dark setting.

Notes on behavior:

- Type detection prefers the file extension, falling back to magic bytes
  (`%PDF-`, PNG/JPEG/GIF/WebP signatures, ZIP + OOXML entry sniffing);
  unknown files fall back to a text-vs-binary heuristic. Binary files get
  a hex-dump preview.
- Relative images in markdown are inlined as data URIs at convert time
  (the page is loaded from a string, so there is no base URL to resolve
  against).
- Raw HTML inside markdown is rendered as inert text, not executed (see
  Security model); `.html` files are navigated to directly so their
  relative assets work.
- Very large sources skip syntax highlighting (>400 KB); CSV stops at
  10,000 rows; PDF at 200 pages; XLSX at 5,000 rows/sheet.

## Architecture

```
preview <file>
   │
   ├─ detect.c    extension first, magic bytes as backup → filetype enum
   ├─ convert.c   dispatch table: per-type converter → HTML string
   │                markdown:       vendor/md4c
   │                docx/xlsx/pptx: vendor/miniz (unzip) + xmlmini.c
   │                                (a ~250-line XML pull parser) + mappers
   │                pdf:            mupdf → per-page PNG data URIs
   │                images:         data URI (stb_image for exotic formats)
   │                text/code:      <pre> + embedded highlight.js
   └─ main.c      vendor/webview/webview.h → WKWebView / WebKitGTK
```

`src/webview_impl.cc` is the single C++ translation unit — it instantiates
the header-only webview library; everything else is C11. Assets in
`assets/` are turned into C byte arrays at build time by `tools/embed.c`
(see the Makefile), so the binary has no runtime file dependencies.

## Security model

`preview` is designed to open files from untrusted sources (a downloaded
README, an emailed `.docx`) without letting them run code or exfiltrate
data. The threat is a malicious document, not a malicious user.

- **No script execution from documents.** Markdown is rendered with raw
  HTML disabled, so an embedded `<script>` shows as literal text rather
  than running. DOCX/XLSX/PPTX text is HTML-escaped on the way out.
- **URL scheme allow-list.** Hyperlinks (markdown and DOCX) may only use
  `http`, `https`, `mailto`, fragments, or relative paths. `javascript:`,
  `data:`, `file:` and friends are rewritten to an inert `#blocked`
  target. The check normalizes control characters and case first, so
  `jav&#9;ascript:` doesn't slip through.
- **Only images are inlined.** A document that references a local file is
  embedded only if the bytes actually decode as a known image type; a
  reference to `~/.ssh/id_rsa` or any non-image is dropped, so documents
  can't fold arbitrary local files into the page.
- **Content-Security-Policy.** Generated pages carry a restrictive CSP:
  `default-src 'none'`, no `connect-src` (no fetch/XHR/WebSocket), no
  `form-action`, no plugins, no `base-uri`. Even if an escaping bug ever
  let markup through, it has no channel to phone home. Inline CSS/JS are
  ours (the page is assembled from a trusted string); the Esc/keyboard
  handlers are injected as WebKit user scripts, which are CSP-exempt by
  design, so the policy doesn't break them.
- **Bounded resource use on ZIP formats.** DOCX/XLSX/PPTX parts are
  extracted against a 512 MB per-archive budget, so a zip bomb is refused
  before it is inflated. Spreadsheet column references, page/row/sheet
  counts, and image sizes are all capped.

Known limitations (deliberate trade-offs, not oversights):

- **Remote images load by default.** `img-src` permits `http(s)`, so a
  document that references `https://tracker/pixel.png` will fetch it when
  displayed — a tracking beacon that reveals the file was opened (and your
  IP), but not the file's contents. Pass `--no-remote` to forbid remote
  subresources entirely (the CSP `img-src` drops to `data:` only), which
  closes this vector at the cost of not showing web-hosted images.
- **`.html` files are rendered with full trust.** An `.html` argument is
  loaded directly (so its own relative assets and scripts work), *without*
  the CSP above — treat opening one like opening it in a browser.

PDFs (mupdf builds) are parsed in an **isolated child process** with a CPU
limit and a **syscall sandbox**: seccomp-BPF on Linux and a Seatbelt
profile on macOS forbid the process from executing programs or opening
network connections before it is handed a single byte of the PDF. So a
memory-corruption bug in mupdf can at worst crash the child — which
`preview` reports as an error page — and cannot spawn a shell or phone
home from inside it. (Set `PREVIEW_PDF_NOSANDBOX=1` to render in-process,
without either layer.) The child still shares the filesystem view of the
main process, so a full filesystem jail would tighten it further.

In pdf.js builds the PDF is instead parsed by pdf.js **inside the web
view**, where it is confined by the same page CSP as everything else —
`connect-src 'none'` means even a pdf.js bug cannot exfiltrate, and the
web engine's own process sandbox contains it.

## Adding a format

1. Add a `FT_*` value in `src/detect.h`.
2. Map its extension (and magic bytes if any) in `src/detect.c`.
3. Write `char *convert_foo(...)` returning an HTML string.
4. Register it in the dispatch table in `src/main.c`.

## Vendored libraries

See `vendor/VERSIONS`. All permissively licensed (MIT/BSD-style):
[webview/webview](https://github.com/webview/webview),
[md4c](https://github.com/mity/md4c),
[miniz](https://github.com/richgel999/miniz),
[stb_image](https://github.com/nothings/stb).
