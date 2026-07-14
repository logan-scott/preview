#!/bin/sh
# Assertion-based regression suite for preview.
#
# Structural assertions (grep for expected markup), not golden-byte
# comparisons, so results are stable across platforms and library
# versions (mupdf's PNG output, for instance, varies by version).
#
# Usage: test/run.sh [path-to-preview-binary]   (default: ./preview)
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
PREVIEW=${1:-$ROOT/preview}
FIX="$HERE/fixtures"

if [ ! -x "$PREVIEW" ]; then
    echo "error: preview binary not found at $PREVIEW" >&2
    exit 2
fi

echo "generating fixtures..."
python3 "$HERE/make_fixtures.py" || exit 2

PASS=0
FAIL=0
TMP=$(mktemp)

# render <fixture> -> $TMP ; returns preview exit code
render() {
    "$PREVIEW" --dump-html "$FIX/$1" >"$TMP" 2>/dev/null
}

ok()   { PASS=$((PASS + 1)); }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL: %s\n' "$1"; }

has()      { if grep -qF -- "$2" "$TMP"; then ok; else bad "$1: expected '$2'"; fi; }
has_re()   { if grep -qi -- "$2" "$TMP"; then ok; else bad "$1: expected /$2/"; fi; }
hasnt()    { if grep -qF -- "$2" "$TMP"; then bad "$1: unexpected '$2'"; else ok; fi; }
hasnt_re() { if grep -qi -- "$2" "$TMP"; then bad "$1: unexpected /$2/"; else ok; fi; }
count_is() { n=$(grep -oF -- "$2" "$TMP" | wc -l | tr -d ' '); if [ "$n" = "$3" ]; then ok; else bad "$1: expected $3 x '$2', got $n"; fi; }

# --- markdown ---------------------------------------------------------------
render doc.md
has "md" "<h1>Test Document</h1>"
has "md" "<strong>bold</strong>"
has "md" "language-c"
has "md" "<td>two</td>"
has "md" "checkbox"
has "md" "data:image/png;base64,"
has "md" "Content-Security-Policy"

# --- json / csv / tsv / code / image / binary -------------------------------
render data.json
has "json" "&quot;name&quot;: &quot;ann&quot;"
has "json" "&quot;meta&quot;: {}"

render table.csv
has "csv" "<thead>"
has "csv" "note, with comma"
has "csv" "says &quot;hi&quot; ok"

render table.tsv
has "tsv" "<th>a</th>"

render sample.c
has "code" "language-c"
has "code" "&lt;stdio.h&gt;"

render pix.png
has "png" "data:image/png;base64,"

render blob.bin
has "bin" "No preview available"
has "bin" "00000000"

# --- security: hostile markdown ---------------------------------------------
render evil.md
has     "evil" "<h1>Doc</h1>"
hasnt   "evil" "<script>fetch"
has     "evil" "&lt;script&gt;"
hasnt_re "evil" 'href="javascript'
hasnt_re "evil" 'href="data:'
has     "evil" 'href="https://example.com/ok"'
has     "evil" 'href="docs/page.md"'
has     "evil" 'href="#blocked"'
hasnt   "evil" "TOP SECRET sentinel"
has     "evil" "data:image/png;base64,"

# --- docx -------------------------------------------------------------------
render report.docx
has "docx" "<h1>Quarterly Report</h1>"
has "docx" "<b>Bold</b>"
has "docx" "<ul><li>bullet one"
has "docx" "<ol><li>first"
has "docx" 'colspan="2"'
has "docx" "<sup>2</sup>"
has "docx" "color:#C00000"
has "docx" 'href="https://example.com/x"'
has "docx" 'href="#blocked"'
has "docx" "data:image/png;base64,"

# --- xlsx -------------------------------------------------------------------
render sheet.xlsx
has "xlsx" "<h2>Revenue</h2>"
has "xlsx" "Notes &amp; Caveats"
has "xlsx" "<td>Product</td>"
has "xlsx" "<td>1234.5</td>"
has "xlsx" "<td>TRUE</td>"
has "xlsx" "<td>inline text</td>"
has "xlsx" "<td>2025-01-01</td>"   # serial 45658, date-styled cell

# --- pptx -------------------------------------------------------------------
render deck.pptx
has "pptx" "<h2>First Slide</h2>"
has "pptx" "<h2>Second Slide</h2>"
has "pptx" "<p>point one</p>"
has "pptx" "data:image/png;base64,"

# --- pdf (skip strict checks when built without mupdf) ----------------------
render three.pdf
if grep -qF "pdfjsLib" "$TMP"; then
    # Built without mupdf: PDFs render client-side via bundled pdf.js, so
    # --dump-html carries the machinery, not pre-rendered images.
    echo "  (pdf.js fallback build; asserting the viewer, not pixels)"
    has "pdf-fallback" "getDocument"
    has "pdf-fallback" "GlobalWorkerOptions.workerSrc"
    hasnt "pdf-fallback" "not built into this binary"
    render corrupt.pdf   # a PDF, even a broken one, still routes to pdf.js
    has "corrupt-pdf-fallback" "pdfjsLib"
else
    # mupdf build: pages are rendered server-side to PNGs.
    count_is "pdf" "data:image/png;base64," 3
    has "pdf" "3 pages"
    render corrupt.pdf
    has "corrupt-pdf" "Could not render PDF"
fi

# --- adversarial / corrupt --------------------------------------------------
render bomb.docx
has "zip-bomb" "survived"      # body renders; oversized part refused, no OOM

render bad.docx
has "bad-docx" "Not a valid DOCX"

# --- offline mode -----------------------------------------------------------
"$PREVIEW" --dump-html "$FIX/doc.md" >"$TMP" 2>/dev/null
has "default" "img-src data: https: http:;"          # remote images allowed
"$PREVIEW" --no-remote --dump-html "$FIX/doc.md" >"$TMP" 2>/dev/null
has "no-remote" "img-src data:;"                       # data URIs only
hasnt "no-remote" "img-src data: https:"

# --- syscall sandbox --------------------------------------------------------
# The selftest applies the sandbox then tries to exec a program: a working
# sandbox kills the process (Linux seccomp) or fails the exec (macOS).
"$PREVIEW" --sandbox-selftest >/dev/null 2>&1
sbec=$?
if [ "$sbec" -eq 2 ]; then
    echo "  (syscall sandbox unavailable on this platform; skipping)"
elif [ "$sbec" -eq 4 ] || [ "$sbec" -ge 128 ]; then
    ok   # exec blocked, by error or by termination
else
    bad "sandbox: exec was not blocked (exit $sbec)"
fi

# --- CLI --------------------------------------------------------------------
"$PREVIEW" --version >"$TMP" 2>&1; has "version" "preview "
if "$PREVIEW" --help >/dev/null 2>&1; then ok; else bad "help: nonzero exit"; fi

rm -f "$TMP"
echo
echo "passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ]
