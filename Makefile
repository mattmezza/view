.DEFAULT_GOAL := all
CC ?= cc
PYTHON ?= python3
VERSION ?= $(shell git -c safe.directory="$(CURDIR)" describe --tags --exact-match 2>/dev/null || printf dev)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
DESTDIR ?=
BUILD ?= build
STATIC ?= 0
PDFIUM ?= $(BUILD)/pdfium
PKG_CONFIG ?= pkg-config

PACKAGES := x11 libwebp
ifeq ($(STATIC),1)
CODEC_DEPS := $(BUILD)/static-deps/.ready
CODEC_CFLAGS := -I$(abspath $(BUILD)/static-deps/include)
CODEC_LIBS := $(BUILD)/static-deps/lib/libtiff.a $(BUILD)/static-deps/lib/libavif.a $(BUILD)/static-deps/lib/libview-jpeg.a -laom -lz
PKG_FLAGS := --static
LDFLAGS += -static
else
PACKAGES += libtiff-4 libavif
endif

CPPFLAGS += -D_GNU_SOURCE -DFPDF_STATIC -Ivendor/cui -Ivendor/stb -Ivendor/nanosvg -I$(PDFIUM)/include
CPPFLAGS += $(CODEC_CFLAGS) $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections
LDFLAGS += -fuse-ld=lld -Wl,--gc-sections
LDLIBS += -Wl,--start-group $(PDFIUM)/lib/libpdfium.a $(PDFIUM)/lib/libc++.a \
    $(PDFIUM)/lib/libc++abi.a $(CODEC_LIBS) \
    $(shell $(PKG_CONFIG) $(PKG_FLAGS) --libs $(PACKAGES)) -pthread -lm -Wl,--end-group
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(BUILD)/%.o)
ICONS := $(wildcard assets/icons/*.svg)
FONTS := assets/fonts/Geist-Regular.ttf assets/fonts/Geist-SemiBold.ttf assets/fonts/GeistMono-Regular.ttf
TARGET := $(BUILD)/view

.PHONY: all install clean release FORCE
ifneq ($(filter clean,$(MAKECMDGOALS)),)
.NOTPARALLEL:
endif
all: $(TARGET)

$(TARGET): $(OBJECTS) $(BUILD)/assets.o $(PDFIUM)/.ready $(CODEC_DEPS)
	$(CC) $(LDFLAGS) $(OBJECTS) $(BUILD)/assets.o $(LDLIBS) -o $@

$(BUILD)/%.o: src/%.c $(PDFIUM)/.ready $(CODEC_DEPS) $(BUILD)/version.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -include $(BUILD)/version.h -MMD -MP -c $< -o $@

$(BUILD)/cui.o $(BUILD)/decoders.o: CFLAGS += -Wno-unused-function
$(BUILD)/decoders.o: CFLAGS += -Wno-pedantic -Wno-sign-compare

$(BUILD)/version.h: FORCE
	@mkdir -p $(BUILD)
	@printf '#define VIEW_VERSION "%s"\n' '$(VERSION)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp

$(PDFIUM)/.ready: scripts/pdfium.sh
	sh $< $(PDFIUM)

$(BUILD)/static-deps/.ready: scripts/static-deps.sh
	sh $< $(abspath $(BUILD)/static-deps)

$(BUILD)/assets.S: scripts/embed.py $(FONTS) $(ICONS) assets/view.svg
	@mkdir -p $(BUILD)
	$(PYTHON) scripts/embed.py font_regular=$(word 1,$(FONTS)) font_bold=$(word 2,$(FONTS)) \
	    font_mono=$(word 3,$(FONTS)) view_icon=assets/view.svg \
	    $(foreach icon,$(ICONS),icon_$(subst -,_,$(basename $(notdir $(icon))))=$(icon)) > $@

$(BUILD)/assets.o: $(BUILD)/assets.S
	$(CC) -c $< -o $@

install: $(TARGET)
	@case "$(BINDIR):$(DATADIR)" in /*:/*) ;; *) echo 'BINDIR and DATADIR must be absolute.' >&2; exit 2;; esac
	install -Dm755 $(TARGET) "$(DESTDIR)$(BINDIR)/view"
	$(TARGET) --desktop-entry "$(BINDIR)/view" > $(BUILD)/view.desktop
	install -Dm644 $(BUILD)/view.desktop "$(DESTDIR)$(DATADIR)/applications/view.desktop"
	install -Dm644 assets/view.svg "$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/view.svg"
	@if [ -z "$(DESTDIR)" ] && command -v update-desktop-database >/dev/null; then \
	    update-desktop-database "$(DATADIR)/applications"; fi

release:
	@printf '%s\n' '$(name)' | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$$' || \
	    { echo 'Usage: make release name=v0.1.0' >&2; exit 2; }
	@test -z "$$(git status --porcelain)" || { echo 'Commit local changes first.' >&2; exit 2; }
	@test "$$(git rev-parse HEAD)" = "$$(git ls-remote origin refs/heads/main | cut -f1)" || \
	    { echo 'Push the current commit to origin/main first.' >&2; exit 2; }
	gh release create '$(name)' --repo mattmezza/view --target "$$(git rev-parse HEAD)" \
	    --title '$(name)' --generate-notes

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d)
