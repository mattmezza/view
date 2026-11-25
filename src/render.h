#ifndef RENDER_H
#define RENDER_H
#include "view.h"
#include "stb_truetype.h"

typedef struct {
    uint32_t code;
    int font, size, w, h, x, y;
    unsigned char *mask;
    bool valid;
} Glyph;

typedef struct {
    Display *display;
    Window window;
    GC gc;
    XImage *image;
    uint32_t *pixels;
    int w, h;
    bool native;
    stbtt_fontinfo fonts[3];
    unsigned char *font_bytes[3];
    Glyph glyphs[512];
} Renderer;

void renderer_init(Renderer *, Display *, Window, const Config *);
void renderer_fonts(Renderer *, const Config *);
void renderer_destroy(Renderer *);
bool renderer_paint(Renderer *, cui_draw_list, cui_color background);
cui_size renderer_measure(void *, cui_str, float, cui_text_style);
bool renderer_metrics(void *, float, cui_text_style, cui_font_metrics *);
#endif
