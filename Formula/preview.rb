# Homebrew formula for preview.
#
# Install from this repository as a tap once it is public:
#
#   brew tap logan-scott/preview https://github.com/logan-scott/preview
#   brew install preview
#
# or point Homebrew straight at this file:
#
#   brew install --build-from-source ./Formula/preview.rb
#
# NOTE: the stable `url` below resolves only when the repository is public.
# While it is private, use the `--HEAD` build (`brew install --HEAD ...`),
# which clones over your authenticated git remote.
class Preview < Formula
  desc "Cross-platform CLI document viewer"
  homepage "https://github.com/logan-scott/preview"
  url "https://github.com/logan-scott/preview/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "e103d893acb722b50a8a97f291949811bf04c77bcb29cbc56b09890efa1aaeeb"
  license "MIT"
  head "https://github.com/logan-scott/preview.git", branch: "main"

  depends_on "pkg-config" => :build
  # mupdf enables PDF preview. It is AGPL-licensed; omit it (and build the
  # formula with `--without-pdf` semantics via HEAD + NO_PDF) for a binary
  # free of that obligation. On macOS the web view is a system framework,
  # so no GTK/WebKit dependency is needed.
  depends_on "mupdf"

  def install
    system "make", "PREFIX=#{prefix}", "install"
  end

  test do
    assert_match "preview #{version}", shell_output("#{bin}/preview --version")
    (testpath/"t.md").write("# Hello\n")
    html = shell_output("#{bin}/preview --dump-html #{testpath}/t.md")
    assert_match "<h1>Hello</h1>", html
  end
end
