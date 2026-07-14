# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/), and the project
aims to follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Windows support (MinGW/MSYS2 + WebView2; PDF via pdf.js), verified in CI.
- New formats: Jupyter notebooks (`.ipynb`), OpenDocument
  (`.odt`/`.ods`/`.odp`), and RTF.
- File-manager integration: `make macos-app` builds a `Preview.app`
  bundle for "Open With"; `make install` installs a `preview.desktop`
  entry with MIME associations on Linux.
- Prebuilt release binaries (macOS arm64/x64, Linux x64, Windows x64) and
  a GitHub Release workflow.
- Light/dark demo screenshots, `CHANGELOG.md`, `CONTRIBUTING.md`, issue/PR
  templates, and a dependency-freshness check (`make check-deps`).

## [0.2.1] - 2026-07-13

### Added
- Syscall sandbox around the PDF render worker: seccomp-BPF on Linux and a
  Seatbelt profile on macOS block exec and network access, so a bug in
  mupdf cannot spawn a shell or reach the network.
- `pdf.js` fallback: builds without mupdf render PDFs client-side in the
  web view instead of showing an error, with no native dependency.

### Fixed
- Homebrew builds now link mupdf. The formula passes `MUPDF_PREFIX`
  explicitly, since the Makefile's `brew --prefix mupdf` probe returns
  nothing inside Homebrew's build sandbox.

## [0.2.0] - 2026-07-13

### Added
- Continuous integration on macOS and Linux plus an AddressSanitizer /
  UndefinedBehaviorSanitizer job, with an assertion-based test suite.
- `make install` / `uninstall`, a man page, an MIT `LICENSE`, and a
  Homebrew formula.
- XLSX number and date formatting (Excel serials render as dates).
- DOCX run color and highlight.
- `--watch` to re-render on file changes; `--no-remote` offline mode.
- PDF rendering isolated in a subprocess with a CPU limit.

### Fixed
- Linux build: replaced `usleep`/`useconds_t` with `nanosleep` and defined
  `_DEFAULT_SOURCE` so glibc exposes POSIX symbols under `-std=c11`.

## [0.1.0] - 2026-07-12

### Added
- Initial release. Native viewer window (WKWebView / WebKitGTK) rendering
  Markdown, images, text and source code, JSON, CSV/TSV, HTML, PDF, DOCX,
  XLSX, and PPTX by converting each type to HTML.
- Security model treating documents as untrusted: no script execution from
  Markdown, URL-scheme allow-list, image-only inlining, a restrictive
  Content-Security-Policy, and bounded ZIP extraction.

[Unreleased]: https://github.com/logan-scott/preview/compare/v0.2.1...HEAD
[0.2.1]: https://github.com/logan-scott/preview/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/logan-scott/preview/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/logan-scott/preview/releases/tag/v0.1.0
