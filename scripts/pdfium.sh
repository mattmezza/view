#!/bin/sh
set -eu

case "$(uname -m)" in
    x86_64) arch=x64; digest=13908bb2d40a6e017c4c5a6a7baecc6efd7b1c30392c8a79e80072d2b48b18eb ;;
    aarch64) arch=arm64; digest=abe1c3d5b168ec2baaafc7a8fcddfda1a09417f39199c7993fd28d34d3a7f70e ;;
    *) echo "PDFium archive unavailable for $(uname -m)" >&2; exit 1 ;;
esac
dest=$1
mkdir -p "$dest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 3 "https://github.com/kognitos/pdfium-static/releases/download/chromium/7809/pdfium-linux-$arch-static.tgz" -o "$tmp/pdfium.tgz"
echo "$digest  $tmp/pdfium.tgz" | sha256sum -c
tar xzf "$tmp/pdfium.tgz" -C "$dest"
touch "$dest/.ready"
