# preview — cross-platform document viewer
#
# macOS: clang + WebKit framework (no extra deps)
# Linux: gcc/clang + WebKitGTK (see README for package names)

CC      ?= cc
CXX     ?= c++
BUILD   := build
BIN     := preview

# install layout (override PREFIX for a user-local install, e.g.
# `make install PREFIX=$HOME/.local`; DESTDIR is honored for packaging)
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1
INSTALL ?= install

WARN    := -Wall -Wextra
# _DEFAULT_SOURCE: under -std=c11 glibc hides POSIX symbols (realpath,
# strcasecmp, fileno, nanosleep, ...); this re-exposes them. No effect on
# macOS, where they are visible by default.
CFLAGS  += -std=c11 $(WARN) -O2 -Ivendor -Ivendor/md4c -Isrc -I$(BUILD) \
           -D_DEFAULT_SOURCE -DMINIZ_NO_ZLIB_APIS
CXXFLAGS+= -std=c++14 $(WARN) -O2 -Ivendor
LDLIBS  +=

UNAME_S := $(shell uname -s)

# --- PDF support (mupdf, with a pdf.js fallback) ---
# Resolution order: NO_PDF=1 skips mupdf; MUPDF_PREFIX=<dir> points at an
# install prefix (include/ + lib/); otherwise Homebrew (macOS) or
# pkg-config (Linux) is probed. Without mupdf the build still renders PDFs
# via the bundled pdf.js fallback (see conv_pdf.c).
ifeq ($(NO_PDF),)
  ifeq ($(MUPDF_PREFIX),)
    ifeq ($(UNAME_S),Darwin)
      MUPDF_PREFIX := $(shell brew --prefix mupdf 2>/dev/null)
    endif
  endif
  ifneq ($(MUPDF_PREFIX),)
    CFLAGS += -I$(MUPDF_PREFIX)/include
    LDLIBS += -L$(MUPDF_PREFIX)/lib -lmupdf -lmupdf-third
    HAVE_PDF := 1
  else ifeq ($(shell pkg-config --exists mupdf 2>/dev/null && echo yes),yes)
    CFLAGS += $(shell pkg-config --cflags mupdf)
    LDLIBS += $(shell pkg-config --libs mupdf)
    HAVE_PDF := 1
  endif
endif
ifeq ($(HAVE_PDF),)
  CFLAGS += -DPREVIEW_NO_PDF
  $(warning mupdf not found: PDFs will render via the bundled pdf.js fallback. For native rendering install mupdf (macOS: brew install mupdf | Debian/Ubuntu: apt install libmupdf-dev) or set MUPDF_PREFIX=<dir>.)
endif

ifeq ($(UNAME_S),Darwin)
  LDLIBS += -framework WebKit -ldl
else ifeq ($(UNAME_S),Linux)
  # Pick the newest WebKitGTK present: GTK4/webkitgtk-6.0, then GTK3/4.1, then GTK3/4.0
  WEBKIT_PC := $(shell for pc in "gtk4 webkitgtk-6.0" "gtk+-3.0 webkit2gtk-4.1" "gtk+-3.0 webkit2gtk-4.0"; do \
                 if pkg-config --exists $$pc 2>/dev/null; then echo "$$pc"; break; fi; done)
  ifeq ($(WEBKIT_PC),)
    ifneq ($(MAKECMDGOALS),clean)
      $(error No WebKitGTK development package found. Debian/Ubuntu: apt install libgtk-3-dev libwebkit2gtk-4.1-dev | Fedora: dnf install gtk3-devel webkit2gtk4.1-devel | Arch: pacman -S webkit2gtk-4.1)
    endif
  endif
  CXXFLAGS += $(shell pkg-config --cflags $(WEBKIT_PC))
  LDLIBS   += $(shell pkg-config --libs $(WEBKIT_PC)) -ldl -lpthread
endif

C_SRCS   := src/main.c src/detect.c src/str.c src/page.c src/convert.c \
            src/conv_pdf.c src/conv_docx.c src/conv_xlsx.c src/conv_pptx.c \
            src/xmlmini.c src/ziputil.c src/sandbox.c src/stb_impl.c
MD4C_SRCS := vendor/md4c/md4c.c vendor/md4c/md4c-html.c vendor/md4c/entity.c
MINIZ_SRCS:= vendor/miniz/miniz.c vendor/miniz/miniz_zip.c \
             vendor/miniz/miniz_tdef.c vendor/miniz/miniz_tinfl.c
CXX_SRCS := src/webview_impl.cc

OBJS := $(C_SRCS:src/%.c=$(BUILD)/%.o) \
        $(MD4C_SRCS:vendor/md4c/%.c=$(BUILD)/md4c_%.o) \
        $(MINIZ_SRCS:vendor/miniz/%.c=$(BUILD)/mz_%.o) \
        $(CXX_SRCS:src/%.cc=$(BUILD)/%.oo)

ASSET_HDRS := $(BUILD)/asset_hljs_js.h $(BUILD)/asset_hljs_css.h
PDFJS_HDRS := $(BUILD)/asset_pdfjs_js.h $(BUILD)/asset_pdfjs_worker_js.h

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/md4c_%.o: vendor/md4c/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/mz_%.o: vendor/miniz/%.c | $(BUILD)
	$(CC) $(CFLAGS) -Ivendor/miniz -c -o $@ $<

$(BUILD)/%.oo: src/%.cc | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# --- embedded assets (highlight.js) ---
$(BUILD)/embed: tools/embed.c | $(BUILD)
	$(CC) -O2 -o $@ $<

# dark theme wrapped in a media query so one stylesheet serves both modes
$(BUILD)/hljs_theme.css: assets/hljs-github.min.css assets/hljs-github-dark.min.css | $(BUILD)
	{ cat assets/hljs-github.min.css; \
	  printf '@media(prefers-color-scheme:dark){'; \
	  cat assets/hljs-github-dark.min.css; \
	  printf '}'; } > $@

$(BUILD)/asset_hljs_js.h: assets/hljs.min.js $(BUILD)/embed
	$(BUILD)/embed ASSET_HLJS_JS $< > $@

$(BUILD)/asset_hljs_css.h: $(BUILD)/hljs_theme.css $(BUILD)/embed
	$(BUILD)/embed ASSET_HLJS_CSS $< > $@

# pdf.js (only embedded when building without mupdf — see conv_pdf.c)
$(BUILD)/asset_pdfjs_js.h: assets/pdf.min.js $(BUILD)/embed
	$(BUILD)/embed ASSET_PDFJS_JS $< > $@

$(BUILD)/asset_pdfjs_worker_js.h: assets/pdf.worker.min.js $(BUILD)/embed
	$(BUILD)/embed ASSET_PDFJS_WORKER_JS $< > $@

$(BUILD)/page.o: $(ASSET_HDRS)
ifeq ($(HAVE_PDF),)
$(BUILD)/conv_pdf.o: $(PDFJS_HDRS)
endif
$(OBJS): $(wildcard src/*.h)

$(BUILD):
	mkdir -p $(BUILD)

test: $(BIN)
	sh test/run.sh $(abspath $(BIN))

check-deps:
	sh tools/check-deps.sh

install: $(BIN)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	$(INSTALL) -d $(DESTDIR)$(MANDIR)
	$(INSTALL) -m 644 man/preview.1 $(DESTDIR)$(MANDIR)/preview.1
ifeq ($(UNAME_S),Linux)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/applications
	$(INSTALL) -m 644 packaging/preview.desktop \
	  $(DESTDIR)$(PREFIX)/share/applications/preview.desktop
endif

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MANDIR)/preview.1
	rm -f $(DESTDIR)$(PREFIX)/share/applications/preview.desktop

# macOS: assemble a double-clickable Preview.app that opens documents via
# "Open With". Bundles the CLI binary and a tiny launcher (see
# packaging/macos_launcher.m). Drag the result to /Applications.
APP := Preview.app
macos-app: $(BIN)
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS
	cp packaging/Info.plist $(APP)/Contents/Info.plist
	cp $(BIN) $(APP)/Contents/MacOS/preview
	$(CC) -O2 -framework Cocoa -o $(APP)/Contents/MacOS/PreviewLauncher \
	  packaging/macos_launcher.m
	@echo "Built $(APP). Try: open $(APP) --args <file>, or drag to /Applications."

clean:
	rm -rf $(BUILD) $(BIN) test/fixtures $(APP)

.PHONY: all clean test install uninstall macos-app
