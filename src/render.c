#include "render.h"
#include <X11/Xutil.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char font_regular[], font_bold[], font_mono[];

static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }

typedef struct { int x0, y0, x1, y1; } Rect;

static Rect rect(cui_rect r) {
    int x = (int)roundf(r.x), y = (int)roundf(r.y);
    return (Rect){x, y, (int)roundf(r.x + r.w), (int)roundf(r.y + r.h)};
}

static Rect intersect(Rect a, Rect b) {
    return (Rect){maximum(a.x0, b.x0), maximum(a.y0, b.y0),
                  minimum(a.x1, b.x1), minimum(a.y1, b.y1)};
}

static uint32_t rgb(cui_color c) {
    return (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b;
}

static uint32_t blend(uint32_t bg, uint32_t fg, unsigned alpha) {
    unsigned back = 255 - alpha;
    unsigned rb = (fg & 0xff00ff) * alpha + (bg & 0xff00ff) * back + 0x800080;
    unsigned g = ((fg >> 8) & 255) * alpha + ((bg >> 8) & 255) * back + 128;
    /* Each channel is divided by 255 with correct rounding. */
    rb += (rb >> 8) & 0xff00ff;
    g += g >> 8;
    return ((rb >> 8) & 0xff00ff) | (g & 0xff00);
}

static void span(Renderer *r, int y, int x0, int x1, uint32_t color, unsigned a) {
    uint32_t *row = r->pixels + (size_t)y * r->w;
    if (a == 255) {
        for (int x = x0; x < x1; ++x) row[x] = color;
    } else if (a) {
        for (int x = x0; x < x1; ++x) row[x] = blend(row[x], color, a);
    }
}

static int corner_inset(int y, Rect b, int radius) {
    if (radius <= 0) return 0;
    float d = fmaxf(b.y0 + radius - (y + .5f), (y + .5f) - (b.y1 - radius));
    return d > 0 ? (int)ceilf(radius - sqrtf(fmaxf(0, radius * radius - d * d)) - .5f) : 0;
}

static void shape(Renderer *r, Rect clip, cui_draw_command cmd) {
    if (cmd.color.a == 0) return;
    Rect b = rect(cmd.bounds), draw = intersect(b, clip);
    int radius = minimum(maximum(0, (int)roundf(cmd.radius)),
                         minimum(b.x1 - b.x0, b.y1 - b.y0) / 2);
    int stroke = cmd.type == CUI_DRAW_BORDER ? maximum(1, (int)roundf(cmd.stroke)) : 0;
    Rect inner = {b.x0 + stroke, b.y0 + stroke, b.x1 - stroke, b.y1 - stroke};
    uint32_t color = rgb(cmd.color);
    for (int y = draw.y0; y < draw.y1; ++y) {
        int d = corner_inset(y, b, radius);
        int x0 = maximum(draw.x0, b.x0 + d), x1 = minimum(draw.x1, b.x1 - d);
        if (stroke && y >= inner.y0 && y < inner.y1 && inner.x0 < inner.x1) {
            int inside = corner_inset(y, inner, radius - stroke);
            span(r, y, x0, minimum(x1, inner.x0 + inside), color, cmd.color.a);
            span(r, y, maximum(x0, inner.x1 - inside), x1, color, cmd.color.a);
        } else {
            span(r, y, x0, x1, color, cmd.color.a);
        }
    }
}

static bool resize(Renderer *r, int w, int h) {
    if (r->image && r->w == w && r->h == h) return true;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (size_t)w * h > 36000000) return false;
    int screen = DefaultScreen(r->display);
    XImage *im = XCreateImage(r->display, DefaultVisual(r->display, screen),
                              DefaultDepth(r->display, screen), ZPixmap, 0, NULL, w, h, 32, 0);
    if (!im) return false;
    bool native = im->bits_per_pixel == 32 && im->byte_order == LSBFirst &&
        im->bytes_per_line == w * 4 && im->red_mask == 0xff0000 &&
        im->green_mask == 0xff00 && im->blue_mask == 0xff;
    im->data = calloc(h, im->bytes_per_line);
    uint32_t *pixels = native ? (uint32_t *)im->data : malloc((size_t)w * h * sizeof *pixels);
    if (!im->data || !pixels) {
        XDestroyImage(im);
        if (!native) free(pixels);
        return false;
    }
    if (r->image) XDestroyImage(r->image);
    if (!r->native) free(r->pixels);
    r->w = w;
    r->h = h;
    r->image = im;
    r->pixels = pixels;
    r->native = native;
    return true;
}

static int font_index(cui_text_style style) {
    return style.monospace ? 2 : style.weight >= CUI_TEXT_WEIGHT_SEMIBOLD ? 1 : 0;
}

static float scale_for(const stbtt_fontinfo *font, float size) {
    return stbtt_ScaleForMappingEmToPixels(font, size);
}

bool renderer_metrics(void *user, float size, cui_text_style style, cui_font_metrics *out) {
    Renderer *r = user;
    const stbtt_fontinfo *font = &r->fonts[font_index(style)];
    int a, d, gap;
    stbtt_GetFontVMetrics(font, &a, &d, &gap);
    float scale = scale_for(font, size);
    *out = (cui_font_metrics){ceilf(a * scale), ceilf(-d * scale), ceilf((a - d + gap) * scale)};
    return true;
}

static int glyph_index(const stbtt_fontinfo *font, unsigned cp) {
    int id = stbtt_FindGlyphIndex(font, cp);
    return id ? id : stbtt_FindGlyphIndex(font, 0xfffd);
}

cui_size renderer_measure(void *user, cui_str text, float size, cui_text_style style) {
    Renderer *r = user;
    const stbtt_fontinfo *font = &r->fonts[font_index(style)];
    int previous = 0, width = 0;
    for (const char *s = text.data, *end = s + text.length; s < end; ) {
        uint32_t code = utf8_next(&s, end);
        int glyph = glyph_index(font, code), advance;
        stbtt_GetGlyphHMetrics(font, glyph, &advance, NULL);
        width += advance + stbtt_GetGlyphKernAdvance(font, previous, glyph);
        previous = glyph;
    }
    cui_font_metrics m;
    renderer_metrics(user, size, style, &m);
    return (cui_size){ceilf(width * scale_for(font, size)), m.line_height};
}

static Glyph *glyph_mask(Renderer *r, int font, int size64, int id) {
    unsigned index = ((unsigned)id * 2654435761u ^ (unsigned)size64 * 97u ^
                      (unsigned)font * 563u) % (sizeof r->glyphs / sizeof *r->glyphs);
    Glyph *g = &r->glyphs[index];
    if (g->valid && g->font == font && g->size == size64 && g->code == (unsigned)id) return g;
    stbtt_FreeBitmap(g->mask, NULL);
    *g = (Glyph){.font = font, .size = size64, .code = id, .valid = true};
    float scale = scale_for(&r->fonts[font], size64 / 64.f);
    g->mask = stbtt_GetGlyphBitmap(&r->fonts[font], scale, scale, id, &g->w, &g->h, &g->x, &g->y);
    return g;
}

static void text(Renderer *r, Rect clip, cui_draw_command cmd) {
    int f = font_index(cmd.text_style);
    const stbtt_fontinfo *font = &r->fonts[f];
    cui_font_metrics m;
    renderer_metrics(r, cmd.font_size, cmd.text_style, &m);
    int baseline = (int)roundf(cmd.bounds.y + (cmd.bounds.h - m.line_height) / 2 + m.ascent);
    float scale = scale_for(font, cmd.font_size);
    float x = cmd.bounds.x;
    int previous = 0;
    uint32_t color = rgb(cmd.color);
    for (const char *s = cmd.text.data, *end = s + cmd.text.length; s < end; ) {
        uint32_t code = utf8_next(&s, end);
        int id = glyph_index(font, code), advance;
        x += stbtt_GetGlyphKernAdvance(font, previous, id) * scale;
        Glyph *g = glyph_mask(r, f, (int)roundf(cmd.font_size * 64), id);
        if (g->mask) {
            int gx = (int)roundf(x) + g->x, gy = baseline + g->y;
            Rect draw = intersect(clip, (Rect){gx, gy, gx + g->w, gy + g->h});
            for (int y = draw.y0; y < draw.y1; ++y) {
                uint32_t *row = r->pixels + (size_t)y * r->w;
                const unsigned char *mask = g->mask + (size_t)(y - gy) * g->w;
                for (int i = draw.x0; i < draw.x1; ++i) {
                    unsigned a = mask[i - gx] * cmd.color.a / 255;
                    if (a) row[i] = a == 255 ? color : blend(row[i], color, a);
                }
            }
        }
        stbtt_GetGlyphHMetrics(font, id, &advance, NULL);
        x += advance * scale;
        if (x >= clip.x1) break;
        previous = id;
    }
}

static unsigned channel(const unsigned char *p, const unsigned char *q, size_t dx,
                        unsigned u, unsigned v) {
    unsigned top = p[0] * (256 - u) + p[dx] * u;
    unsigned bottom = q[0] * (256 - u) + q[dx] * u;
    return (top * (256 - v) + bottom * v + 32768) >> 16;
}

static void bitmap(Renderer *r, Rect clip, cui_draw_command cmd) {
    cui_bitmap_paint p = cmd.bitmap;
    Rect dst = rect(cmd.bounds), draw = intersect(clip, dst);
    int w = dst.x1 - dst.x0, h = dst.y1 - dst.y0;
    if (w <= 0 || h <= 0 || !p.bitmap.pixels || p.opacity <= 0) return;
    double sx = p.source.w / w, sy = p.source.h / h;
    int limit_x = p.bitmap.width - 1, limit_y = p.bitmap.height - 1;
    uint32_t tint = rgb(p.tint);
    for (int y = draw.y0; y < draw.y1; ++y) {
        double fy = fmax(0, fmin(limit_y, p.source.y + (y - dst.y0 + .5) * sy - .5));
        int iy = (int)fy;
        unsigned v = (unsigned)((fy - iy) * 256);
        const unsigned char *row = p.bitmap.pixels + (size_t)iy * p.bitmap.stride;
        const unsigned char *row2 = iy < limit_y ? row + p.bitmap.stride : row;
        uint32_t *out = r->pixels + (size_t)y * r->w;
        double start = p.source.x + (draw.x0 - dst.x0 + .5) * sx - .5;
        for (int x = draw.x0; x < draw.x1; ++x, start += sx) {
            double fx = start < 0 ? 0 : start > limit_x ? limit_x : start;
            int ix = (int)fx;
            unsigned u = (unsigned)((fx - ix) * 256);
            size_t dx = ix < limit_x ? 4 : 0;
            const unsigned char *a = row + (size_t)ix * 4, *b = row2 + (size_t)ix * 4;
            unsigned alpha = u || v ? channel(a + 3, b + 3, dx, u, v) : a[3];
            alpha = (unsigned)(alpha * p.opacity);
            if (!alpha) continue;
            uint32_t color = tint;
            if (!p.custom_tint) {
                color = u || v ? channel(a, b, dx, u, v) << 16 |
                                  channel(a+1, b+1, dx, u, v) << 8 |
                                  channel(a+2, b+2, dx, u, v) :
                                  (uint32_t)a[0] << 16 | (uint32_t)a[1] << 8 | a[2];
            }
            out[x] = alpha >= 255 ? color : blend(out[x], color, alpha);
        }
    }
}

void renderer_fonts(Renderer *r, const Config *c) {
    const unsigned char *const embedded[] = {font_regular, font_bold, font_mono};
    for (int i = 0; i < 3; ++i) {
        free(r->font_bytes[i]);
        r->font_bytes[i] = NULL;
        size_t length;
        if (*c->fonts[i] && read_file(c->fonts[i], 16u * 1024 * 1024, &r->font_bytes[i], &length) &&
            length >= 12 && stbtt_InitFont(&r->fonts[i], r->font_bytes[i], 0)) continue;
        stbtt_InitFont(&r->fonts[i], embedded[i], 0);
    }
    for (size_t i = 0; i < sizeof r->glyphs / sizeof *r->glyphs; ++i) {
        stbtt_FreeBitmap(r->glyphs[i].mask, NULL);
        r->glyphs[i] = (Glyph){0};
    }
}

void renderer_init(Renderer *r, Display *display, Window window, const Config *c) {
    *r = (Renderer){.display = display, .window = window, .gc = XCreateGC(display, window, 0, NULL)};
    renderer_fonts(r, c);
}

void renderer_destroy(Renderer *r) {
    if (r->image) XDestroyImage(r->image);
    if (!r->native) free(r->pixels);
    if (r->gc) XFreeGC(r->display, r->gc);
    for (int i = 0; i < 3; ++i) free(r->font_bytes[i]);
    for (size_t i = 0; i < sizeof r->glyphs / sizeof *r->glyphs; ++i)
        stbtt_FreeBitmap(r->glyphs[i].mask, NULL);
}

static unsigned long masked(unsigned channel, unsigned long mask) {
    if (!mask) return 0;
    unsigned shift = (unsigned)__builtin_ctzl(mask);
    return ((channel * (mask >> shift) + 127) / 255) << shift;
}

bool renderer_paint(Renderer *r, cui_draw_list list, cui_color background) {
    if (!r->gc || !resize(r, (int)list.width, (int)list.height)) return false;
    uint32_t color = rgb(background);
    for (size_t i = 0, n = (size_t)r->w * r->h; i < n; ++i) r->pixels[i] = color;
    Rect clips[64] = {{0, 0, r->w, r->h}};
    int level = 0;
    for (size_t i = 0; i < list.count; ++i) {
        cui_draw_command cmd = list.commands[i];
        Rect clip = clips[level];
        switch (cmd.type) {
        case CUI_DRAW_CLIP_PUSH:
            if (level >= (int)(sizeof clips / sizeof *clips) - 1) return false;
            clips[++level] = intersect(clip, rect(cmd.bounds));
            break;
        case CUI_DRAW_CLIP_POP:
            if (level == 0) return false;
            --level;
            break;
        case CUI_DRAW_RECT:
        case CUI_DRAW_BORDER: shape(r, clip, cmd); break;
        case CUI_DRAW_TEXT: text(r, clip, cmd); break;
        case CUI_DRAW_IMAGE: bitmap(r, clip, cmd); break;
        default: break;
        }
    }
    if (!r->native) {
        for (int y = 0; y < r->h; ++y)
            for (int x = 0; x < r->w; ++x) {
                color = r->pixels[(size_t)y * r->w + x];
                XPutPixel(r->image, x, y,
                          masked(color >> 16, r->image->red_mask) |
                          masked((color >> 8) & 255, r->image->green_mask) |
                          masked(color & 255, r->image->blue_mask));
            }
    }
    XPutImage(r->display, r->window, r->gc, r->image, 0, 0, 0, 0, r->w, r->h);
    XFlush(r->display);
    return true;
}
