#!/bin/sh
# Report vendored/bundled dependency versions against their latest upstream.
# Informational only — never fails. Needs curl; uses the GitHub and cdnjs
# public APIs. Run manually or from the scheduled `deps` CI workflow.
set -u

note() { printf '%-14s pinned %-14s latest %-14s %s\n' "$1" "$2" "$3" "$4"; }

# latest GitHub release tag for owner/repo ("" if none)
gh_release() {
    curl -fsSL "https://api.github.com/repos/$1/releases/latest" 2>/dev/null |
        sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1
}
# latest default-branch commit (short) for owner/repo
gh_commit() {
    curl -fsSL "https://api.github.com/repos/$1/commits?per_page=1" 2>/dev/null |
        sed -n 's/.*"sha": *"\([0-9a-f]\{7\}\).*/\1/p' | head -1
}
# latest version of a cdnjs library
cdnjs() {
    curl -fsSL "https://api.cdnjs.com/libraries/$1?fields=version" 2>/dev/null |
        sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' | head -1
}

VER="$(dirname "$0")/../vendor/VERSIONS"
pinned() { grep -i "^$1" "$VER" 2>/dev/null | head -1 | sed 's/.*: *//'; }

echo "dependency freshness (informational):"
echo

flag() { [ -n "$2" ] && [ "$1" != "$2" ] && echo "UPDATE?" || echo "ok"; }

# git-pinned single-header / source libs
wv_pin=$(pinned webview); wv_new=$(gh_release webview/webview)
note webview "$wv_pin" "${wv_new:-?}" "$(flag "$wv_pin" "$wv_new")"

md_new=$(gh_release mity/md4c)
note md4c "$(pinned md4c)" "${md_new:-none}" "release check"

mz_new=$(gh_release richgel999/miniz)
note miniz "$(pinned miniz)" "${mz_new:-?}" "$(flag "$(pinned miniz)" "$mz_new")"

echo "stb            pinned rolling         (no upstream releases; re-fetch stb_image*.h as needed)"

hl_pin=$(pinned highlight.js); hl_new=$(cdnjs highlight.js)
note highlight.js "$hl_pin" "${hl_new:-?}" "$(flag "$hl_pin" "$hl_new")"

pj_pin=$(pinned pdf.js); pj_new=$(cdnjs pdf.js)
note pdf.js "$pj_pin" "${pj_new:-?}" "note: v4+ is ESM-only"

echo
echo "Bumping a dependency is a manual, reviewed step — re-vendor, rebuild,"
echo "and run 'make test' plus the sanitizer build."
