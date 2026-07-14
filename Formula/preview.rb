# Homebrew formula for preview.
#
# This formula ships in the project repo rather than homebrew-core, so it
# must be tapped first. Homebrew 6.0+ also requires trusting a third-party
# tap before its formula will run:
#
#   brew tap logan-scott/preview https://github.com/logan-scott/preview
#   brew trust logan-scott/preview
#   brew install preview
#
# For the latest commit instead of the tagged release, add --HEAD.
class Preview < Formula
  desc "Cross-platform CLI document viewer"
  homepage "https://github.com/logan-scott/preview"
  url "https://github.com/logan-scott/preview/archive/refs/tags/v0.3.0.tar.gz"
  sha256 "71c09d444679c1d490f99b58eacf81b31992e019e248b232361ba945b9eb5b00"
  license "MIT"
  head "https://github.com/logan-scott/preview.git", branch: "main"

  depends_on "pkg-config" => :build
  # mupdf enables PDF preview. It is AGPL-licensed; omit it (and build the
  # formula with `--without-pdf` semantics via HEAD + NO_PDF) for a binary
  # free of that obligation. On macOS the web view is a system framework,
  # so no GTK/WebKit dependency is needed.
  depends_on "mupdf"

  def install
    # Point the build at mupdf explicitly. The Makefile otherwise locates
    # it on macOS via `brew --prefix mupdf`, which returns nothing inside
    # Homebrew's build sandbox, silently disabling PDF support.
    system "make", "MUPDF_PREFIX=#{formula_opt_prefix("mupdf")}",
           "PREFIX=#{prefix}", "install"
  end

  test do
    assert_match "preview #{version}", shell_output("#{bin}/preview --version")

    (testpath/"t.md").write("# Hello\n")
    html = shell_output("#{bin}/preview --dump-html #{testpath}/t.md")
    assert_match "<h1>Hello</h1>", html

    # mupdf must actually be linked: assert a server-rendered page image,
    # which the pdf.js fallback (used when mupdf is absent) would not
    # produce. mupdf repairs the missing xref, so a bare object tree is
    # enough to render.
    (testpath/"t.pdf").write(<<~PDF)
      %PDF-1.4
      1 0 obj
      << /Type /Catalog /Pages 2 0 R >>
      endobj
      2 0 obj
      << /Type /Pages /Kids [3 0 R] /Count 1 >>
      endobj
      3 0 obj
      << /Type /Page /Parent 2 0 R /MediaBox [0 0 144 144] >>
      endobj
      trailer
      << /Size 4 /Root 1 0 R >>
      %%EOF
    PDF
    pdf = shell_output("#{bin}/preview --dump-html #{testpath}/t.pdf")
    assert_match "data:image/png", pdf
  end
end
