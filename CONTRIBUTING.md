# Contributing to preview

Thanks for your interest in improving `preview`. This is a small,
self-contained C project; contributions of all sizes are welcome.

## Building and testing

```sh
make            # build (auto-detects mupdf; falls back to pdf.js without it)
make test       # build + run the assertion suite
make NO_PDF=1 test   # exercise the pdf.js fallback path
```

The test suite ([`test/run.sh`](test/run.sh)) renders generated fixtures
([`test/make_fixtures.py`](test/make_fixtures.py), standard library only)
and asserts on the structure of the produced HTML — not exact bytes, so it
is stable across platforms and library versions.

Before opening a pull request, please also run the sanitizer build, which
CI runs on every push:

```sh
make NO_PDF=1 test \
  CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" \
  CXX="c++ -fsanitize=address,undefined -fno-omit-frame-pointer"
```

## Adding a file format

The type-detection → converter dispatch is a plain table, so new formats
are small, localized changes:

1. Add an `FT_*` value in [`src/detect.h`](src/detect.h).
2. Map its extension (and magic bytes, if any) in
   [`src/detect.c`](src/detect.c).
3. Write a `convert_*` function that returns a complete HTML document
   (use `page_begin` / `page_end` from [`src/page.h`](src/page.h)).
4. Register it in the dispatch table in [`src/convert.c`](src/convert.c).
5. Add a fixture and assertions to the test suite.

## Code style

- C11, no external runtime dependencies beyond the system web view;
  vendor new libraries under `vendor/` with their license.
- Match the surrounding style (four-space indent, comments that explain
  *why*, not *what*).
- Treat all document content as untrusted — escape anything that reaches
  HTML, and never let a document trigger network access or code execution.
  See the Security model in the [README](README.md).

## Reporting bugs

Please include the file type, your OS, whether the build uses mupdf or the
pdf.js fallback (`preview --version` and how you built it), and a minimal
file that reproduces the problem if you can share one.
