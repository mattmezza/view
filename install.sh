#!/bin/sh
set -eu

usage() {
    echo 'Usage: sh install.sh [--default]'
    echo 'Set VIEW_VERSION to a release tag, or VIEW_BIN_DIR to an absolute install directory.'
}

defaults=false
case "${1-}" in
    --default) defaults=true; shift ;;
    --help|-h) usage; exit 0 ;;
esac
if [ "$#" -ne 0 ]; then usage >&2; exit 2; fi

if [ "$(uname -s)" != Linux ]; then
    echo 'View runs on Linux/X11.' >&2
    exit 1
fi
case "$(uname -m)" in
    x86_64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) echo "No release for $(uname -m)" >&2; exit 1 ;;
esac

base=https://github.com/mattmezza/view/releases
if [ -n "${VIEW_VERSION-}" ]; then
    if ! printf '%s\n' "$VIEW_VERSION" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo 'VIEW_VERSION must look like v0.1.0.' >&2
        exit 2
    fi
    base="$base/download/$VIEW_VERSION"
else
    base="$base/latest/download"
fi
target=${VIEW_BIN_DIR:-"$HOME/.local/bin"}
data=${XDG_DATA_HOME:-"$HOME/.local/share"}
case "$target" in /*) ;; *) echo 'VIEW_BIN_DIR must be absolute.' >&2; exit 2 ;; esac
case "$data" in /*) ;; *) data="$HOME/.local/share" ;; esac
if $defaults && ! command -v xdg-mime >/dev/null 2>&1; then
    echo 'xdg-mime is required for --default.' >&2
    exit 1
fi

binary="view-linux-$arch"
tmp=$(mktemp -d)
new_binary=
new_desktop=
new_icon=
cleanup() {
    rm -rf "$tmp"
    [ -z "$new_binary" ] || rm -f "$new_binary"
    [ -z "$new_desktop" ] || rm -f "$new_desktop"
    [ -z "$new_icon" ] || rm -f "$new_icon"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
curl -fsSL --retry 2 "$base/$binary" -o "$tmp/$binary"
curl -fsSL --retry 2 "$base/$binary.sha256" -o "$tmp/$binary.sha256"
expected=$(awk 'NR == 1 && NF == 2 {print $1}' "$tmp/$binary.sha256")
measured=$(sha256sum "$tmp/$binary" | cut -d ' ' -f1)
if [ "$expected" != "$measured" ]; then
    echo 'Release checksum mismatch.' >&2
    exit 1
fi

desktop="$data/applications"
icons="$data/icons/hicolor/scalable/apps"
mkdir -p "$target" "$desktop" "$icons"
new_binary=$(mktemp "$target/.view.XXXXXXXX")
new_desktop=$(mktemp "$desktop/.view.XXXXXXXX")
new_icon=$(mktemp "$icons/.view.XXXXXXXX")
install -m 755 "$tmp/$binary" "$new_binary"
installed_version=$("$new_binary" --version)
"$new_binary" --desktop-entry "$target/view" > "$new_desktop"
"$new_binary" --icon > "$new_icon"
chmod 644 "$new_desktop" "$new_icon"
mv -f "$new_binary" "$target/view"
mv -f "$new_desktop" "$desktop/view.desktop"
mv -f "$new_icon" "$icons/view.svg"
new_binary= new_desktop= new_icon=
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$desktop"
fi
if $defaults; then
    "$target/view" --mime-types > "$tmp/mime-types"
    while IFS= read -r mime; do xdg-mime default view.desktop "$mime"; done < "$tmp/mime-types"
fi
printf 'Installed View %s at %s/view\n' "$installed_version" "$target"
