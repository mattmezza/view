#ifndef VIEW_H
#define VIEW_H

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "cui.h"

#ifndef VIEW_VERSION
#define VIEW_VERSION "dev"
#endif

typedef struct {
    unsigned char *pixels;
    int w, h;
} Image;

typedef enum {
    OPEN, PREV, NEXT, FIRST, LAST, ZOOM_IN, ZOOM_OUT, FIT, ACTUAL,
    FULLSCREEN, GALLERY, SIDEBAR, FIND, FIND_NEXT, FIND_PREV, COPY,
    UP, DOWN, PAGE_UP, PAGE_DOWN, RELOAD, HELP, QUIT, INFO, PRINT, PRESENT, ACTIONS
} Action;

typedef enum {
    BACKGROUND, CANVAS, SURFACE, RAISED, BORDER, FOREGROUND, MUTED, ACCENT, COLOR_COUNT
} Color;

typedef struct {
    cui_color colors[COLOR_COUNT];
    bool gallery, sidebar, info, wrap, upscale, check_updates, xresources;
    int width, height, font_size, thumb_size, scroll_step, sidebar_width, info_width, padding;
    double zoom_step;
    char fonts[3][4096];
    struct { KeySym symbol; unsigned mods; } keys[ACTIONS][4];
} Config;

extern const char *const action_names[ACTIONS];
extern const char *const mime_types[];
void config_load(Config *, Display *);
double now(void);
const char *base_name(const char *);
bool read_file(const char *, size_t limit, unsigned char **, size_t *);
bool is_image(const char *);
Image image_load(const char *, int max_edge);
Image image_resize(Image, int w, int h);
void image_free(Image *);
size_t utf8_encode(char *, uint32_t);
uint32_t utf8_next(const char **, const char *);
char *utf16_to_utf8(const unsigned short *, size_t);
size_t utf8_to_utf16(const char *, unsigned short *, size_t);
#endif
