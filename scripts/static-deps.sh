#!/bin/sh
set -eu

prefix=$1
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

fetch() {
    curl -fsSL --retry 3 "$1" -o "$scratch/$2"
    echo "$3  $scratch/$2" | sha256sum -c
    tar -xf "$scratch/$2" -C "$scratch"
}

fetch https://download.osgeo.org/libtiff/tiff-4.7.2.tar.xz tiff.tar.xz \
    4996f0c4f93094719b1ca5c6279b20e588773ba8a247533e486416fb662ddb88
fetch https://codeload.github.com/AOMediaCodec/libavif/tar.gz/refs/tags/v1.4.2 avif.tar.gz \
    2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d

cmake -S "$scratch/tiff-4.7.2" -B "$scratch/tiff" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=OFF -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-contrib=OFF \
    -Dtiff-docs=OFF -Dtiff-cxx=OFF -Djbig=OFF -Dlerc=OFF -Dlzma=OFF -Dzstd=OFF -Dlibdeflate=OFF
cmake --build "$scratch/tiff" --parallel 4
cmake --install "$scratch/tiff"

# PDFium supplies a different libjpeg. Keep its internal symbols independent
# from the libjpeg ABI the system TIFF headers were compiled against.
jpeg=$(cc -print-file-name=libjpeg.a)
nm -g --defined-only "$jpeg" |
    awk 'NF == 3 {print $3, "view_tiff_" $3}' | sort -u > "$scratch/jpeg-symbols"
objcopy --redefine-syms="$scratch/jpeg-symbols" "$jpeg" "$prefix/lib/libview-jpeg.a"
objcopy --redefine-syms="$scratch/jpeg-symbols" "$prefix/lib/libtiff.a"

cmake -S "$scratch/libavif-1.4.2" -B "$scratch/avif" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=OFF -DAVIF_CODEC_AOM=SYSTEM -DAVIF_CODEC_AOM_ENCODE=OFF \
    -DAVIF_LIBYUV=OFF -DAVIF_LIBSHARPYUV=OFF -DAVIF_LIB_USE_CXX=OFF
cmake --build "$scratch/avif" --parallel 4
cmake --install "$scratch/avif"

mkdir -p "$prefix/licenses"
cp "$scratch/tiff-4.7.2/LICENSE.md" "$prefix/licenses/libtiff.txt"
cp "$scratch/libavif-1.4.2/LICENSE" "$prefix/licenses/libavif.txt"
touch "$prefix/.ready"
