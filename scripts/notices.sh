#!/bin/sh
set -eu

build=$1
cat LICENSE
for file in vendor/cui/LICENSE vendor/stb/LICENSE assets/fonts/OFL.txt assets/icons/LICENSE \
    vendor/nanosvg/nanosvg.h "$build"/pdfium/LICENSE "$build"/pdfium/licenses/* \
    "$build"/static-deps/licenses/*; do
    printf '\n==== %s ====\n\n' "$(basename "$file")"
    case "$file" in
        *.h) sed -n '2,21p' "$file" ;;
        *) cat "$file" ;;
    esac
done
for package in libc6-dev libgcc-13-dev libx11-dev libxcb1-dev libxau-dev libxdmcp-dev \
    libwebp-dev libjpeg-turbo8-dev zlib1g-dev libaom-dev; do
    file="/usr/share/doc/$package/copyright"
    if [ -f "$file" ]; then
        printf '\n==== Ubuntu %s ====\n\n' "$package"
        cat "$file"
    fi
done
for license in LGPL-2.1 LGPL-3 GPL-2 GPL-3; do
    printf '\n==== %s ====\n\n' "$license"
    cat "/usr/share/common-licenses/$license"
done
