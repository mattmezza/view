#include "view.h"
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include <webp/decode.h>
#include <webp/demux.h>
#include <tiffio.h>
#include <avif/avif.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

const char *const mime_types[] = {
    "application/pdf", "image/jpeg", "image/png", "image/apng", "image/gif", "image/webp",
    "image/avif", "image/tiff", "image/bmp", "image/x-ms-bmp", "image/x-tga", "image/vnd.microsoft.icon",
    "image/x-icon", "image/svg+xml", "image/x-portable-bitmap", "image/x-portable-graymap",
    "image/x-portable-pixmap", "image/x-portable-anymap", "image/vnd.adobe.photoshop",
    "image/x-hdr", "image/qoi", NULL
};

double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool read_file(const char *path, size_t limit, unsigned char **data, size_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 ||
        (uintmax_t)st.st_size > limit) { close(fd); return false; }
    FILE *file = fdopen(fd, "rb");
    if (!file) { close(fd); return false; }
    size_t length = st.st_size;
    unsigned char *buffer = malloc(length + 1);
    bool ok = buffer && fread(buffer, 1, length, file) == length;
    fclose(file);
    if (!ok) { free(buffer); return false; }
    buffer[length] = 0;
    *data = buffer;
    *size = length;
    return true;
}

static bool extension(const char *path, const char *ext) {
    const char *dot = strrchr(base_name(path), '.');
    return dot && !strcasecmp(dot + 1, ext);
}

bool is_image(const char *path) {
    const char *dot = strrchr(base_name(path), '.');
    if (!dot) return false;
    const char *const types[] = {"png","apng","jpeg","jpg","jpe","jfif","webp","gif","bmp","dib",
        "tga","tiff","tif","avif","svg","pbm","pgm","ppm","pnm","qoi","ico","psd","hdr","pic"};
    for (size_t i = 0; i < sizeof types / sizeof *types; ++i)
        if (!strcasecmp(dot + 1, types[i])) return true;
    return false;
}

static bool sane(int w, int h) {
    return w > 0 && h > 0 && (uint64_t)w * h <= 100000000 && w <= 32768 && h <= 32768;
}

static Image make_image(int w, int h) {
    if (!sane(w, h)) return (Image){0};
    unsigned char *pixels = malloc((size_t)w * h * 4);
    return pixels ? (Image){pixels, w, h} : (Image){0};
}

void image_free(Image *im) {
    free(im->pixels);
    *im = (Image){0};
}

Image image_resize(Image source, int w, int h) {
    Image target = make_image(w, h);
    if (target.pixels && !stbir_resize_uint8_srgb(source.pixels, source.w, source.h, 0,
            target.pixels, w, h, 0, STBIR_RGBA)) image_free(&target);
    return target;
}

static Image svg_image(unsigned char *bytes, int edge) {
    NSVGimage *svg = nsvgParse((char *)bytes, "px", 96);
    if (!svg) return (Image){0};
    Image im = {0};
    float longest = fmaxf(svg->width, svg->height);
    if (longest > 0 && isfinite(longest) && svg->width > 0 && svg->height > 0) {
        float scale = (edge > 0 ? edge : fminf(fmaxf(longest, 1024), 8192)) / longest;
        im = make_image(fmaxf(1, ceilf(svg->width * scale)), fmaxf(1, ceilf(svg->height * scale)));
        NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
        if (im.pixels && rasterizer)
            nsvgRasterize(rasterizer, svg, 0, 0, scale, im.pixels, im.w, im.h, im.w * 4);
        else
            image_free(&im);
        nsvgDeleteRasterizer(rasterizer);
    }
    nsvgDelete(svg);
    return im;
}

static Image tiff_image(const char *path) {
    TIFFOpenOptions *options = TIFFOpenOptionsAlloc();
    if (!options) return (Image){0};
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, 400000000);
    TIFF *tiff = TIFFOpenExt(path, "r", options);
    TIFFOpenOptionsFree(options);
    if (!tiff) return (Image){0};
    uint32_t w = 0, h = 0;
    TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &h);
    Image im = w <= 32768 && h <= 32768 ? make_image(w, h) : (Image){0};
    if (im.pixels && !TIFFReadRGBAImageOriented(tiff, w, h, (uint32_t *)im.pixels,
                                                ORIENTATION_TOPLEFT, 1)) image_free(&im);
    TIFFClose(tiff);
    if (im.pixels) {
        /* TIFF's RGBA reader premultiplies. The compositor takes straight alpha. */
        for (size_t i = 0; i < (size_t)im.w * im.h * 4; i += 4) {
            unsigned a = im.pixels[i + 3];
            if (a && a < 255)
                for (int c = 0; c < 3; ++c)
                    im.pixels[i + c] = fmin(255, (im.pixels[i + c] * 255u + a / 2) / a);
        }
    }
    return im;
}

static Image avif_image(const unsigned char *bytes, size_t n) {
    avifDecoder *decoder = avifDecoderCreate();
    if (!decoder) return (Image){0};
    decoder->imageSizeLimit = 100000000;
    decoder->imageDimensionLimit = 32768;
    decoder->maxThreads = 2;
    Image im = {0};
    if (avifDecoderSetIOMemory(decoder, bytes, n) == AVIF_RESULT_OK &&
        avifDecoderParse(decoder) == AVIF_RESULT_OK &&
        avifDecoderNextImage(decoder) == AVIF_RESULT_OK) {
        im = make_image(decoder->image->width, decoder->image->height);
        if (im.pixels) {
            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, decoder->image);
            rgb.format = AVIF_RGB_FORMAT_RGBA;
            rgb.depth = 8;
            rgb.pixels = im.pixels;
            rgb.rowBytes = im.w * 4;
            if (avifImageYUVToRGB(decoder->image, &rgb) != AVIF_RESULT_OK) image_free(&im);
        }
    }
    avifDecoderDestroy(decoder);
    return im;
}

static uint32_t read_le(const unsigned char *p, int bytes) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= (uint32_t)p[i] << (i * 8);
    return v;
}

static Image ico_image(const unsigned char *p, size_t n) {
    if (n < 6 || read_le(p, 2) != 0 || read_le(p + 2, 2) != 1) return (Image){0};
    unsigned count = read_le(p + 4, 2);
    if (count > (n - 6) / 16) return (Image){0};
    Image best = {0};
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *entry = p + 6 + i * 16;
        int w = *entry ? *entry : 256;
        int h = entry[1] ? entry[1] : 256;
        if (w * h <= best.w * best.h) continue;
        size_t offset = read_le(entry + 12, 4), length = read_le(entry + 8, 4);
        if (offset > n || length > n - offset || length < 8) continue;
        const unsigned char *data = p + offset;
        Image im = {0};
        if (!memcmp(data, "\211PNG\r\n\032\n", 8)) {
            if (stbi_info_from_memory(data, length, &im.w, &im.h, NULL) &&
                im.w == w && im.h == h)
                im.pixels = stbi_load_from_memory(data, length, &im.w, &im.h, NULL, 4);
        } else if (length >= 40 && read_le(data, 4) == 40 && read_le(data + 4, 4) == (unsigned)w &&
                   read_le(data + 8, 4) == (unsigned)h * 2 && read_le(data + 16, 4) == 0) {
            unsigned bpp = read_le(data + 14, 2);
            unsigned stride = ((w * bpp + 31) / 32) * 4;
            unsigned mask_stride = ((w + 31) / 32) * 4;
            if ((bpp == 24 || bpp == 32) && length >= 40u + stride * h) {
                im = make_image(w, h);
                bool alpha = false;
                for (int y = 0; im.pixels && y < h; ++y) {
                    const unsigned char *row = data + 40 + (h - y - 1) * stride;
                    for (int x = 0; x < w; ++x) {
                        const unsigned char *pixel = row + x * (bpp / 8);
                        unsigned char *out = im.pixels + ((size_t)y * w + x) * 4;
                        out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0];
                        out[3] = bpp == 32 ? pixel[3] : 0;
                        alpha |= out[3] != 0;
                    }
                }
                const unsigned char *mask = data + 40 + stride * h;
                if (!alpha && im.pixels)
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            im.pixels[((size_t)y * w + x) * 4 + 3] =
                                length >= 40u + (stride + mask_stride) * h &&
                                (mask[(h - y - 1) * mask_stride + x / 8] & (128 >> (x % 8))) ? 0 : 255;
            }
        }
        if (im.pixels) { image_free(&best); best = im; }
    }
    return best;
}

static uint32_t read_be(const unsigned char *p, int bytes) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | p[i];
    return v;
}

static Image qoi_image(const unsigned char *p, size_t n) {
    if (n < 22) return (Image){0};
    uint32_t w = read_be(p + 4, 4), h = read_be(p + 8, 4);
    if (w > 32768 || h > 32768 || (p[12] != 3 && p[12] != 4)) return (Image){0};
    Image im = make_image(w, h);
    if (!im.pixels) return im;
    unsigned char index[64][4] = {{0}}, px[4] = {0, 0, 0, 255};
    unsigned run = 0;
    size_t offset = 14, end = n - 8;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        if (run) --run;
        else {
            if (offset >= end) goto invalid;
            unsigned tag = p[offset++];
            if (tag == 0xfe || tag == 0xff) {
                unsigned channels = tag == 0xfe ? 3 : 4;
                if (end - offset < channels) goto invalid;
                memcpy(px, p + offset, channels);
                offset += channels;
            } else if (tag < 0x40) memcpy(px, index[tag], 4);
            else if (tag < 0x80) {
                px[0] += ((tag >> 4) & 3) - 2; px[1] += ((tag >> 2) & 3) - 2;
                px[2] += (tag & 3) - 2;
            } else if (tag < 0xc0) {
                if (offset >= end) goto invalid;
                int dg = (tag & 63) - 32;
                unsigned delta = p[offset++];
                px[0] += dg + (int)(delta >> 4) - 8; px[1] += dg;
                px[2] += dg + (int)(delta & 15) - 8;
            } else run = tag & 63;
            memcpy(index[(px[0]*3 + px[1]*5 + px[2]*7 + px[3]*11) % 64], px, 4);
        }
        memcpy(im.pixels + i * 4, px, 4);
    }
    if (!run && offset == end && !memcmp(p + end, "\0\0\0\0\0\0\0\1", 8)) return im;
invalid:
    image_free(&im);
    return im;
}

typedef struct { const unsigned char *cur, *end; } ByteStream;

static int pnm_number(ByteStream *input) {
    while (input->cur < input->end) {
        if (isspace(*input->cur)) ++input->cur;
        else if (*input->cur == '#') {
            while (input->cur < input->end && *input->cur != '\n') ++input->cur;
        } else break;
    }
    if (input->cur == input->end || !isdigit(*input->cur)) return -1;
    int value = 0;
    while (input->cur < input->end && isdigit(*input->cur)) {
        value = value * 10 + (*input->cur++ - '0');
        if (value > 65536) return -1;
    }
    return value;
}

static Image pnm_image(const unsigned char *bytes, size_t length) {
    int type = bytes[1] - '0';
    bool ascii = type <= 3, bitmap = type == 1 || type == 4;
    int channels = type == 3 || type == 6 ? 3 : 1;
    ByteStream stream = {bytes + 2, bytes + length};
    int w = pnm_number(&stream), h = pnm_number(&stream);
    int max = bitmap ? 1 : pnm_number(&stream);
    if (!sane(w, h) || max < 1 || max > 65535) return (Image){0};
    if (!ascii) {
        if (stream.cur == stream.end || !isspace(*stream.cur)) return (Image){0};
        if (*stream.cur++ == '\r' && stream.cur < stream.end && *stream.cur == '\n') ++stream.cur;
        size_t expected = bitmap ? (size_t)((w + 7) / 8) * h :
                          (size_t)w * h * channels * (max > 255 ? 2 : 1);
        if (expected > (size_t)(stream.end - stream.cur)) return (Image){0};
    }
    Image im = make_image(w, h);
    if (!im.pixels) return im;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char *p = im.pixels + ((size_t)y * w + x) * 4;
            for (int i = 0; i < channels; ++i) {
                int value;
                if (ascii) value = pnm_number(&stream);
                else if (bitmap) value = (stream.cur[x / 8] >> (7 - (x % 8))) & 1;
                else {
                    int count = max <= 255 ? 1 : 2;
                    value = read_be(stream.cur, count);
                    stream.cur += count;
                }
                if (value < 0 || value > max) { image_free(&im); return im; }
                p[i] = bitmap ? (1 - value) * 255 : (value * 255 + max / 2) / max;
            }
            if (channels == 1) p[1] = p[2] = p[0];
            p[3] = 255;
        }
        if (type == 4) stream.cur += (w + 7) / 8;
    }
    return im;
}

static int jpeg_orientation(const unsigned char *p, size_t n) {
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8) return 1;
    size_t i = 2;
    while (i + 4 <= n && p[i] == 0xff) {
        unsigned marker = p[i + 1];
        if (marker == 0xff) { ++i; continue; }
        if (marker == 0xda || marker == 0xd9) break;
        size_t size = read_be(p + i + 2, 2);
        if (size < 2 || size > n - i - 2) break;
        if (marker == 0xe1 && size >= 16 && !memcmp(p + i + 4, "Exif\0\0", 6)) {
            const unsigned char *tiff = p + i + 10;
            size_t len = size - 8;
            uint32_t (*read_int)(const unsigned char *, int) =
                !memcmp(tiff, "II\x2a\0", 4) ? read_le :
                !memcmp(tiff, "MM\0\x2a", 4) ? read_be : NULL;
            if (!read_int) break;
            size_t dir = read_int(tiff + 4, 4);
            if (dir > len - 2) break;
            unsigned count = read_int(tiff + dir, 2);
            dir += 2;
            if (count > (len - dir) / 12) break;
            for (unsigned j = 0; j < count; ++j) {
                const unsigned char *entry = tiff + dir + j * 12;
                if (read_int(entry, 2) == 0x112 && read_int(entry + 2, 2) == 3 &&
                    read_int(entry + 4, 4) == 1) {
                    unsigned orientation = read_int(entry + 8, 2);
                    return orientation >= 1 && orientation <= 8 ? (int)orientation : 1;
                }
            }
        }
        i += 2 + size;
    }
    return 1;
}

static void orient(Image *im, int orientation) {
    if (!im->pixels || orientation == 1) return;
    bool swap = orientation >= 5;
    Image out = make_image(swap ? im->h : im->w, swap ? im->w : im->h);
    if (!out.pixels) return;
    bool flip_x = orientation == 2 || orientation == 3 || orientation == 6 || orientation == 7;
    bool flip_y = orientation == 3 || orientation == 4 || orientation == 7 || orientation == 8;
    for (int y = 0; y < out.h; ++y) {
        int dy = flip_y ? out.h - y - 1 : y;
        for (int x = 0; x < out.w; ++x) {
            int dx = flip_x ? out.w - x - 1 : x;
            int sx = swap ? dy : dx, sy = swap ? dx : dy;
            memcpy(out.pixels + ((size_t)y * out.w + x) * 4,
                   im->pixels + ((size_t)sy * im->w + sx) * 4, 4);
        }
    }
    image_free(im);
    *im = out;
}

static Image webp_image(const unsigned char *bytes, size_t n, int edge) {
    WebPDecoderConfig config;
    Image im = {0};
    if (!WebPInitDecoderConfig(&config) || WebPGetFeatures(bytes, n, &config.input) != VP8_STATUS_OK)
        return im;
    int w = config.input.width, h = config.input.height;
    if (!sane(w, h)) return im;
    if (config.input.has_animation) {
        if ((uint64_t)w * h > 24000000) return im;
        WebPData input = {bytes, n};
        WebPAnimDecoderOptions options;
        if (!WebPAnimDecoderOptionsInit(&options)) return im;
        options.color_mode = MODE_RGBA;
        WebPAnimDecoder *decoder = WebPAnimDecoderNew(&input, &options);
        uint8_t *pixels;
        int timestamp;
        if (decoder && WebPAnimDecoderGetNext(decoder, &pixels, &timestamp)) {
            im = make_image(w, h);
            if (im.pixels) memcpy(im.pixels, pixels, (size_t)w * h * 4);
        }
        WebPAnimDecoderDelete(decoder);
        return im;
    }

    double scale = edge ? fmin(1, edge / (double)(w > h ? w : h)) : 1;
    im = make_image(fmax(1, round(w * scale)), fmax(1, round(h * scale)));
    if (!im.pixels) return im;
    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = im.pixels;
    config.output.u.RGBA.stride = im.w * 4;
    config.output.u.RGBA.size = (size_t)im.w * im.h * 4;
    config.options.use_scaling = scale < 1;
    config.options.scaled_width = im.w;
    config.options.scaled_height = im.h;
    if (WebPDecode(bytes, n, &config) != VP8_STATUS_OK) image_free(&im);
    WebPFreeDecBuffer(&config.output);
    return im;
}

Image image_load(const char *path, int edge) {
    unsigned char *bytes;
    size_t n;
    if (!read_file(path, 256u * 1024 * 1024, &bytes, &n)) return (Image){0};
    Image im = {0};
    int orientation = 1;
    if (extension(path, "svg")) {
        im = svg_image(bytes, edge);
    } else if (n >= 12 && !memcmp(bytes, "RIFF", 4) && !memcmp(bytes + 8, "WEBP", 4)) {
        im = webp_image(bytes, n, edge);
    } else if (extension(path, "tif") || extension(path, "tiff") ||
               (n >= 4 && (!memcmp(bytes, "II\x2a\0", 4) || !memcmp(bytes, "MM\0\x2a", 4)))) {
        im = tiff_image(path);
    } else if (extension(path, "avif") || (n >= 12 && !memcmp(bytes + 4, "ftypavif", 8))) {
        im = avif_image(bytes, n);
    } else if (n >= 3 && bytes[0] == 'P' && bytes[1] >= '1' && bytes[1] <= '6' &&
               isspace(bytes[2])) {
        im = pnm_image(bytes, n);
    } else if (n >= 4 && !memcmp(bytes, "qoif", 4)) {
        im = qoi_image(bytes, n);
    } else if (extension(path, "ico") || (n >= 4 && !memcmp(bytes, "\0\0\1\0", 4))) {
        im = ico_image(bytes, n);
    } else {
        int w, h;
        if (stbi_info_from_memory(bytes, n, &w, &h, NULL) && sane(w, h))
            im.pixels = stbi_load_from_memory(bytes, n, &im.w, &im.h, NULL, 4);
        orientation = jpeg_orientation(bytes, n);
    }
    free(bytes);
    orient(&im, orientation);
    if (im.pixels && edge && (im.w > edge || im.h > edge)) {
        double scale = edge / (double)(im.w > im.h ? im.w : im.h);
        Image small = image_resize(im, fmax(1, round(im.w * scale)), fmax(1, round(im.h * scale)));
        if (small.pixels) { image_free(&im); return small; }
    }
    return im;
}

size_t utf8_encode(char *out, uint32_t cp) {
    if (cp <= 0x7f) { out[0] = cp; return 1; }
    if (cp <= 0x7ff) { out[0] = 0xc0 | (cp >> 6); out[1] = 0x80 | (cp & 63); return 2; }
    if (cp <= 0xffff) {
        out[0] = 0xe0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 63);
        out[2] = 0x80 | (cp & 63); return 3;
    }
    out[0] = 0xf0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 63);
    out[2] = 0x80 | ((cp >> 6) & 63); out[3] = 0x80 | (cp & 63); return 4;
}

uint32_t utf8_next(const char **s, const char *end) {
    const unsigned char *p = (const unsigned char *)*s;
    if (*s >= end) return 0;
    uint32_t lead = *p++;
    ++*s;
    if (lead < 0x80) return lead;
    unsigned count = lead >= 0xf0 && lead <= 0xf4 ? 3 : lead >= 0xe0 && lead <= 0xef ? 2 :
                     lead >= 0xc2 && lead <= 0xdf ? 1 : 0;
    if (!count || (size_t)(end - *s) < count) return 0xfffd;
    uint32_t cp = lead & (0x7f >> (count + 1));
    for (unsigned i = 0; i < count; ++i) {
        if ((p[i] & 0xc0) != 0x80) return 0xfffd;
        cp = (cp << 6) | (p[i] & 63);
    }
    if ((count == 2 && cp < 0x800) || (count == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return 0xfffd;
    *s += count;
    return cp;
}

char *utf16_to_utf8(const unsigned short *in, size_t n) {
    if (n > (SIZE_MAX - 1) / 3) return NULL;
    char *out = malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < n && in[i]; ++i) {
        uint32_t cp = in[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < n && in[i+1] >= 0xdc00 && in[i+1] <= 0xdfff)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (in[++i] - 0xdc00);
        else if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
        pos += utf8_encode(out + pos, cp);
    }
    out[pos] = 0;
    return out;
}

size_t utf8_to_utf16(const char *p, unsigned short *out, size_t cap) {
    const char *end = p + strlen(p);
    size_t n = 0;
    while (p < end && n + 2 < cap) {
        uint32_t cp = utf8_next(&p, end);
        if (cp > 0xffff) {
            cp -= 0x10000;
            out[n++] = 0xd800 | (cp >> 10);
            out[n++] = 0xdc00 | (cp & 1023);
        } else out[n++] = cp;
    }
    out[n] = 0;
    return n;
}
