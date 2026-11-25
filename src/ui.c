#include "app.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_GAP 24
#define PDF_TOP 24

extern const char icon_folder_open[], icon_photo[], icon_document_text[], icon_squares_2x2[],
    icon_document[], icon_chevron_left[], icon_chevron_right[], icon_magnifying_glass[],
    icon_magnifying_glass_plus[], icon_magnifying_glass_minus[], icon_arrows_pointing_out[],
    icon_x_mark[], icon_arrow_up[], icon_arrow_down[], icon_arrow_down_tray[], icon_information_circle[],
    icon_printer[], icon_presentation_chart_bar[];

void app_icons(App *a) {
    const char *const sources[] = {
        icon_folder_open, icon_photo, icon_document_text, icon_squares_2x2, icon_document,
        icon_chevron_left, icon_chevron_right, icon_magnifying_glass, icon_magnifying_glass_plus,
        icon_magnifying_glass_minus, icon_arrows_pointing_out,
        icon_x_mark, icon_arrow_up, icon_arrow_down, icon_arrow_down_tray,
        icon_information_circle, icon_printer, icon_presentation_chart_bar,
    };
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    for (int i = 0; rast && i < ICONS; ++i) {
        char *source = strdup(sources[i]);
        if (!source) continue;
        NSVGimage *svg = nsvgParse(source, "px", 96);
        free(source);
        if (!svg) continue;
        int edge = 48;
        unsigned char *pixels = malloc(edge * edge * 4);
        if (pixels) {
            nsvgRasterize(rast, svg, 0, 0, edge / svg->width, pixels, edge, edge, edge * 4);
            a->icons[i] = (Image){pixels, edge, edge};
        }
        nsvgDelete(svg);
    }
    nsvgDeleteRasterizer(rast);
}

static double limit(double n, double lo, double hi) { return fmax(lo, fmin(n, hi)); }

static double page_top(const App *a, int page) {
    return PDF_TOP + a->doc.pages[page].offset * a->layout.scale + PAGE_GAP * page;
}

int app_page_at(const App *a, double y) {
    if (a->presenting) return a->doc.current;
    double target = y + a->scroll - a->layout.canvas.y;
    int lo = 0, hi = a->doc.count;
    while (lo + 1 < hi) {
        int index = lo + (hi - lo) / 2;
        if (page_top(a, index) <= target) lo = index;
        else hi = index;
    }
    return lo;
}

cui_rect app_page_rect(const App *a, int index) {
    cui_rect c = a->layout.canvas;
    PageSize page = a->doc.pages[index];
    double scale = a->layout.scale, width = page.w * scale;
    if (a->presenting)
        return (cui_rect){c.x + (c.w - width) / 2, c.y + (c.h - page.h * scale) / 2,
                          width, page.h * scale};
    return (cui_rect){
        c.x + (c.w - width) / 2 + a->pan,
        c.y + page_top(a, index) - a->scroll,
        width, page.h * scale
    };
}

void app_jump(App *a, int index) {
    if (index < 0 || index >= a->doc.count) return;
    if (a->presenting) {
        a->doc.current = index;
        a->dirty = true;
        return;
    }
    a->scroll = fmax(0, page_top(a, index) - 10);
    a->sidebar_shift = 0;
    app_layout(a);
    a->dirty = true;
}

void app_layout(App *a) {
    Document *d = &a->doc;
    bool chrome = !a->fullscreen && !a->presenting;
    bool gallery = chrome && a->gallery && d->picture.pixels && d->count > 1 && a->h >= 330;
    bool sidebar = chrome && a->sidebar && d->pdf && a->w >= 580;
    int top = chrome ? 62 : 0, foot = chrome ? 40 : 0;
    int info = chrome && a->info && d->path ? fmin(a->config.info_width, a->w * .48) : 0;
    int side = sidebar ? fmin(a->sidebar_width, fmin(a->w * .44, a->w - info - 200)) : 0;
    int width = a->w - info;
    int thumb = fmin(a->thumb_size, fmax(40, fmin(a->h - top - foot - 157, (width - 105) * .75)));
    int strip = gallery ? thumb + 42 : 0;
    a->layout.canvas = (cui_rect){side, top, fmax(1, width - side), fmax(1, a->h - top - foot - strip)};
    a->layout.strip = (cui_rect){0, a->h - foot - strip, width, strip};
    a->layout.sidebar = (cui_rect){0, top, side, a->h - top - foot};
    a->layout.info = (cui_rect){width, top, info, a->h - top - foot};
    a->layout.strip_grip = (cui_rect){0, a->layout.strip.y - 3, width, strip ? 11 : 0};
    a->layout.sidebar_grip = (cui_rect){side - 5, top, side ? 10 : 0, a->h - top - foot};
    cui_rect c = a->layout.canvas;

    a->layout.thumb_width = thumb * 4 / 3;
    int slots = fmin(23, fmax(1, (width - 105) / (a->layout.thumb_width + 10)));
    a->layout.strip_slots = slots;
    double pad = a->fullscreen ? 4 : a->config.padding;
    double cap_w = fmax(32, c.w - 2 * pad), cap_h = fmax(32, c.h - 2 * pad);
    if (a->presenting) {
        double w = d->pdf ? d->pages[d->current].w : d->picture.w;
        double h = d->pdf ? d->pages[d->current].h : d->picture.h;
        a->layout.scale = w > 0 && h > 0 ? fmin(c.w / w, c.h / h) : 1;
        return;
    }
    double w = d->pdf ? d->max_width : d->picture.w;
    double h = d->pdf ? d->pages[0].h : d->picture.h;
    double fit = w > 0 && h > 0 ? fmin(cap_w / w, cap_h / h) : 1;
    if (!a->config.upscale && !d->pdf) fit = fmin(1, fit);
    double scale = fit;
    if (a->scale_mode == SCALE_ACTUAL) scale = 1;
    if (a->scale_mode == SCALE_WIDTH && w > 0) scale = cap_w / w;
    if (a->scale_mode == SCALE_MANUAL)
        scale = (d->pdf ? cap_w / d->max_width : fit) * a->zoom;
    if (d->pdf) scale = fmin(scale, fmin(32768 / d->max_width, 8));
    a->layout.scale = fmax(.0001, scale);

    double max_x = fmax(0, (w * a->layout.scale - c.w) / 2);
    a->pan = limit(a->pan, -max_x, max_x);
    if (d->pdf) {
        double content = PDF_TOP * 2 + d->total_height * a->layout.scale + PAGE_GAP * (d->count - 1);
        double end = fmax(0, content - c.h);
        a->scroll = limit(a->scroll, 0, end);
        d->current = end > 0 && a->scroll >= end - 1 ? d->count - 1 :
                     app_page_at(a, c.y + c.h * .26);
    } else {
        double max_y = fmax(0, (h * a->layout.scale - c.h) / 2);
        a->scroll = limit(a->scroll, -max_y, max_y);
    }
}

static cui_color palette(const App *a, Color color) { return a->config.colors[color]; }

static void box(App *a, cui_id id, cui_rect r, cui_color background,
                cui_color border, float stroke, float radius, bool clip) {
    cui_begin_absolute_box(a->ui, id, (cui_box){.background = background, .border = border,
        .border_width = stroke, .radius = radius, .clip = clip}, r);
}

static void fill(App *a, cui_rect r, cui_color color, float radius) {
    if (r.w <= 0 || r.h <= 0) return;
    box(a, 0, r, color, (cui_color){0}, 0, radius, false);
    cui_end_box(a->ui);
}

static void label(App *a, cui_rect r, const char *value, float size, Color color,
                  bool bold, bool mono, cui_align align) {
    if (r.w <= 0 || r.h <= 0 || !*value) return;
    cui_text_style style = {.weight = bold ? CUI_TEXT_WEIGHT_SEMIBOLD : CUI_TEXT_WEIGHT_NORMAL,
                            .monospace = mono};
    char clipped[4096];
    cui_str text = cui_string(value);
    if (renderer_measure(&a->renderer, text, size, style).width > r.w) {
        size_t n = text.length < sizeof clipped - 4 ? text.length : sizeof clipped - 4;
        memcpy(clipped, value, n);
        while (n && ((unsigned char)value[n] & 0xc0) == 0x80) --n;
        for (int pass = 0; n && pass < 15; ++pass) {
            memcpy(clipped + n, "…", 4);
            float width = renderer_measure(&a->renderer, cui_string(clipped), size, style).width;
            if (width <= r.w) break;
            n = fmin(n - 1, n * r.w / width);
            while (n && ((unsigned char)clipped[n] & 0xc0) == 0x80) --n;
        }
        memcpy(clipped + n, "…", 4);
        text = cui_string(clipped);
    }
    cui_begin_absolute_box(a->ui, 0, (cui_box){.align_x = align, .align_y = CUI_ALIGN_CENTER,
                                              .clip = true}, r);
    cui_text_ex(a->ui, text, size, palette(a, color), 0, style);
    cui_end_box(a->ui);
}

static void image(App *a, Image im, cui_rect r, cui_rect source, bool tinted, cui_color tint) {
    if (!im.pixels || r.w < 1 || r.h < 1) return;
    cui_begin_absolute_box(a->ui, 0, (cui_box){0}, r);
    cui_paint_bitmap(a->ui, 0, (cui_bitmap_paint){
        .bitmap = {im.pixels, im.w, im.h, (size_t)im.w * 4, 0},
        .source = source, .opacity = tint.a / 255.f, .custom_tint = tinted, .tint = tint
    }, (cui_box){.width = r.w, .height = r.h});
    cui_end_box(a->ui);
}

static void icon(App *a, Icon i, cui_rect r, Color color) {
    image(a, a->icons[i], r, (cui_rect){0}, true, palette(a, color));
}

static void tooltip_target(App *a, cui_id id, const char *name) {
    if (a->hovered_id != id) {
        a->hovered_id = id;
        a->hover_since = now();
    }
    snprintf(a->hover_label, sizeof a->hover_label, "%s", name);
}

static bool button(App *a, cui_rect r, const char *name, int icon_id, const char *text,
                   bool selected, bool disabled) {
    cui_id id = cui_id_from(a->ui, name);
    if (a->prompt != PROMPT_NONE) disabled = true;
    cui_interaction before = cui_interaction_state(a->ui, id);
    Color bg = selected ? RAISED : SURFACE;
    bool hot = before.hovered || before.focus_visible;
    box(a, id, r, selected || hot ? palette(a, bg == SURFACE ? RAISED : bg) : (cui_color){0},
        selected || before.focus_visible ? palette(a, selected ? BORDER : ACCENT) : (cui_color){0},
        1, 7, true);
    cui_interaction hit = cui_interact(a->ui, id, CUI_ROLE_BUTTON, cui_string(name), disabled, selected, 0);
    if (hit.hovered && !disabled) tooltip_target(a, id, name);
    Color fg = disabled ? MUTED : selected ? ACCENT : FOREGROUND;
    float h = r.h < 30 ? 17 : 19;
    if (icon_id >= 0) icon(a, (Icon)icon_id, (cui_rect){(r.w-h)/2, (r.h-h)/2, h, h}, fg);
    if (text) label(a, (cui_rect){4, 0, r.w - 8, r.h}, text, a->config.font_size - 1,
                    fg, false, true, CUI_ALIGN_CENTER);
    cui_end_box(a->ui);
    return hit.activated && !disabled;
}

static void action_button(App *a, int x, int y, const char *name, Icon icon_id,
                          Action action, bool selected, bool disabled) {
    if (button(a, (cui_rect){x, y, 34, 34}, name, icon_id, NULL, selected, disabled))
        a->frame_action = action;
}

static const char *extension_label(const Document *d) {
    if (d->pdf) return "PDF";
    const char *ext = d->path ? strrchr(base_name(d->path), '.') : NULL;
    return ext && ext[1] ? ext + 1 : "Image";
}

static void topbar(App *a) {
    Document *d = &a->doc;
    cui_begin_portal(a->ui, cui_id_from(a->ui, "header"),
        (cui_box){.background = palette(a, SURFACE), .clip = true}, (cui_rect){0, 0, a->w, 62}, 2);
    fill(a, (cui_rect){0, 61, a->w, 1}, palette(a, BORDER), 0);
    fill(a, (cui_rect){17, 13, 36, 36}, palette(a, RAISED), 9);
    icon(a, d->pdf ? ICON_PDF : ICON_PHOTO, (cui_rect){25, 21, 20, 20}, ACCENT);

    int x = a->w - 15;
    action_button(a, x -= 34, 14, "Fullscreen", ICON_FULL, FULLSCREEN, false, false);
    if (a->w >= 480) {
        action_button(a, x -= 38, 14, "Zoom in", ICON_IN, ZOOM_IN, false, !d->path);
        action_button(a, x -= 36, 14, "Zoom out", ICON_OUT, ZOOM_OUT, false, !d->path);
        x -= 9;
        fill(a, (cui_rect){x + 2, 20, 1, 22}, palette(a, BORDER), 0);
        if (d->pdf) {
            action_button(a, x -= 38, 14, "Find in PDF", ICON_FIND, FIND, *d->query, false);
            action_button(a, x -= 36, 14, "Page thumbnails", ICON_SIDEBAR, SIDEBAR, a->sidebar, false);
        } else {
            action_button(a, x -= 38, 14, "Gallery", ICON_GALLERY, GALLERY, a->gallery && d->count > 1,
                          d->count < 2);
        }
    }
    if (d->path) {
        action_button(a, x -= 36, 14, "File info", ICON_INFO, INFO, a->info, false);
        action_button(a, x -= 36, 14, "Print", ICON_PRINT, PRINT, false, false);
        action_button(a, x -= 36, 14, "Present", ICON_PRESENT, PRESENT, false, false);
    }
    action_button(a, x -= 38, 14, "Open file or folder", ICON_OPEN, OPEN, false, false);
    if (*a->update && a->w >= 660) {
        char update[80];
        snprintf(update, sizeof update, "Update %s", a->update);
        x -= 122;
        if (button(a, (cui_rect){x, 15, 112, 32}, "Open release on GitHub", -1, update, true, false))
            a->frame_command = UI_UPDATE;
    }
    const char *name = d->path ? base_name(d->path) : "View";
    label(a, (cui_rect){65, 11, x - 76, 23}, name, a->config.font_size + 2,
          FOREGROUND, true, false, CUI_ALIGN_START);
    char dir[1024] = "Image and PDF viewer";
    if (d->path) {
        const char *end = strrchr(d->path, '/'), *start = end;
        while (start > d->path && start[-1] != '/') --start;
        if (start == end) snprintf(dir, sizeof dir, "/");
        else snprintf(dir, sizeof dir, "%.*s", (int)fmin(sizeof dir - 1, end - start), start);
    }
    label(a, (cui_rect){65, 34, x - 76, 16}, dir, a->config.font_size - 2,
          MUTED, false, false, CUI_ALIGN_START);
    cui_end_box(a->ui);
}

static void footer(App *a) {
    Document *d = &a->doc;
    cui_begin_portal(a->ui, cui_id_from(a->ui, "status bar"),
        (cui_box){.background = palette(a, SURFACE), .clip = true},
        (cui_rect){0, a->h - 40, a->w, 40}, 2);
    fill(a, (cui_rect){0, 0, a->w, 1}, palette(a, BORDER), 0);
    if (d->path) {
        char kind[24], index[80], dimensions[90];
        snprintf(kind, sizeof kind, "%.20s", extension_label(d));
        for (char *p = kind; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
        int tag = (int)fmin(74, 22 + strlen(kind) * (a->config.font_size - 2) * .63);
        fill(a, (cui_rect){16, 10, tag, 21}, palette(a, RAISED), 5);
        label(a, (cui_rect){18, 10, tag - 4, 21}, kind, a->config.font_size - 3,
              ACCENT, false, true, CUI_ALIGN_CENTER);
        snprintf(index, sizeof index, "%d / %d", d->current + 1, d->count);
        label(a, (cui_rect){tag + 28, 0, 112, 40}, index, a->config.font_size - 1,
              FOREGROUND, false, true, CUI_ALIGN_START);
        if (d->pdf)
            snprintf(dimensions, sizeof dimensions, "%.0f × %.0f pt", d->pages[d->current].w,
                     d->pages[d->current].h);
        else
            snprintf(dimensions, sizeof dimensions, "%d × %d px", d->picture.w, d->picture.h);
        if (a->w >= 500)
            label(a, (cui_rect){tag + 143, 0, 180, 40}, dimensions, a->config.font_size - 2,
                  MUTED, false, true, CUI_ALIGN_START);

        char zoom[40];
        snprintf(zoom, sizeof zoom, "%.0f%%", a->layout.scale * 100);
        label(a, (cui_rect){a->w - 174, 0, 68, 40}, zoom, a->config.font_size - 1,
              FOREGROUND, false, true, CUI_ALIGN_END);
        const char *fit = a->scale_mode == SCALE_ACTUAL ? "Actual" :
                          a->scale_mode == SCALE_WIDTH ? "Width" : "Fit";
        if (button(a, (cui_rect){a->w - 93, 7, 77, 26}, "Fit or fill width", -1, fit,
                   true, false)) a->frame_command = UI_FIT_TOGGLE;
    } else {
        label(a, (cui_rect){16, 0, a->w - 120, 40}, "Drop a file or folder here",
              a->config.font_size - 1, MUTED, false, false, CUI_ALIGN_START);
        if (button(a, (cui_rect){a->w - 93, 7, 77, 26}, "Keyboard shortcuts", -1, "Help",
                   true, false)) a->frame_action = HELP;
    }
    cui_end_box(a->ui);
}

static Image *thumbnail(App *a, int index, int width) {
    bool load = false;
    Image *im = document_cached(&a->doc, index, true, width, false, &load);
    if (load && a->thumb_budget > 0) {
        --a->thumb_budget;
        im = document_cached(&a->doc, index, true, width, true, &a->thumb_pending);
    } else if (load) a->thumb_pending = true;
    return im;
}

static void cover(App *a, const Image *im, cui_rect r) {
    if (!im || !im->pixels) return;
    double scale = fmax(r.w / im->w, r.h / im->h);
    int w = fmax(1, fmin(im->w, round(r.w / scale)));
    int h = fmax(1, fmin(im->h, round(r.h / scale)));
    image(a, *im, r, (cui_rect){(im->w - w) / 2, (im->h - h) / 2, w, h},
          false, (cui_color){.a = 255});
}

static void filmstrip(App *a) {
    cui_rect strip = a->layout.strip;
    if (strip.h <= 0) return;
    Document *d = &a->doc;
    cui_begin_portal(a->ui, cui_id_from(a->ui, "filmstrip"),
        (cui_box){.background = palette(a, SURFACE), .clip = true}, strip, 2);
    fill(a, (cui_rect){0, 0, strip.w, 1}, palette(a, BORDER), 0);
    int count = fmin(a->layout.strip_slots, d->count), width = a->layout.thumb_width;
    int start = limit(d->current - count / 2 + a->thumbnail_shift, 0, d->count - count);
    float left = (strip.w - (count * width + (count - 1) * 10)) / 2;
    float y = 17, height = strip.h - 42;
    bool grip = a->drag == DRAG_STRIP || a->pointer == POINTER_ROW;
    fill(a, (cui_rect){(strip.w - 42) / 2, 5, 42, 3}, palette(a, grip ? ACCENT : MUTED), 2);
    for (int i = 0; i < count; ++i) {
        int index = start + i;
        char key[48], number[24];
        snprintf(key, sizeof key, "Image %d", index + 1);
        snprintf(number, sizeof number, "%02d", index + 1);
        cui_id id = cui_id_from(a->ui, key);
        bool selected = index == d->current;
        bool hot = cui_interaction_state(a->ui, id).hovered;
        float x = left + i * (width + 10);
        box(a, id, (cui_rect){x, y, width, height}, palette(a, RAISED),
            palette(a, selected ? ACCENT : hot ? MUTED : BORDER), selected ? 2 : 1, 6, true);
        bool blocked = a->prompt != PROMPT_NONE;
        cui_interaction hit = cui_interact(a->ui, id, CUI_ROLE_BUTTON,
            cui_string(base_name(d->files[index])), blocked, selected, 0);
        Image *im = thumbnail(a, index, width * 2);
        cover(a, im, (cui_rect){3, 3, width - 6, height - 6});
        if (hit.activated && !blocked) a->frame_item = index;
        if (hit.hovered && !blocked) tooltip_target(a, id, base_name(d->files[index]));
        cui_end_box(a->ui);
        label(a, (cui_rect){x, y + height + 4, width, 19}, number, a->config.font_size - 3,
              selected ? ACCENT : MUTED, false, true, CUI_ALIGN_CENTER);
    }
    action_button(a, 15, (int)(y + (height - 34) / 2), "Previous image", ICON_LEFT, PREV,
                  false, !a->config.wrap && d->current == 0);
    action_button(a, strip.w - 49, (int)(y + (height - 34) / 2), "Next image", ICON_RIGHT, NEXT,
                  false, !a->config.wrap && d->current == d->count - 1);
    cui_end_box(a->ui);
}

static void page_sidebar(App *a) {
    cui_rect side = a->layout.sidebar;
    if (side.w <= 0) return;
    Document *d = &a->doc;
    cui_begin_portal(a->ui, cui_id_from(a->ui, "pages"),
        (cui_box){.background = palette(a, SURFACE), .clip = true}, side, 2);
    fill(a, (cui_rect){side.w - 1, 0, 1, side.h}, palette(a, BORDER), 0);
    label(a, (cui_rect){17, 8, side.w - 80, 27}, "Pages", a->config.font_size, FOREGROUND,
          true, false, CUI_ALIGN_START);
    char total[24];
    snprintf(total, sizeof total, "%d", d->count);
    label(a, (cui_rect){side.w - 73, 8, 54, 27}, total, a->config.font_size - 2, MUTED,
          false, true, CUI_ALIGN_END);
    fill(a, (cui_rect){15, 40, side.w - 30, 1}, palette(a, BORDER), 0);
    int tile_h = side.w - 9;
    int slots = fmax(1, (side.h - 50) / tile_h);
    int start = limit(d->current - slots / 2 + a->sidebar_shift, 0, fmax(0, d->count - slots));
    for (int i = 0; i <= slots && start + i < d->count; ++i) {
        int index = start + i;
        char key[48], number[24];
        snprintf(key, sizeof key, "Page %d", index + 1);
        snprintf(number, sizeof number, "%d", index + 1);
        bool selected = index == d->current;
        cui_id id = cui_id_from(a->ui, key);
        bool hot = cui_interaction_state(a->ui, id).hovered;
        float y = 49 + i * tile_h;
        box(a, id, (cui_rect){11, y, side.w - 22, tile_h - 8},
            palette(a, selected || hot ? RAISED : SURFACE),
            selected ? palette(a, ACCENT) : (cui_color){0}, selected ? 1 : 0, 7, true);
        cui_interaction hit = cui_interact(a->ui, id, CUI_ROLE_BUTTON, cui_string(key),
                                           a->prompt != PROMPT_NONE, selected, 0);
        Image *im = thumbnail(a, index, ((int)side.w + 31) / 32 * 64);
        if (im) {
            float scale = fminf((side.w - 49) / im->w, (tile_h - 38.f) / im->h);
            float w = im->w * scale, h = im->h * scale;
            image(a, *im, (cui_rect){(side.w - 22 - w) / 2, 7 + (tile_h - 38 - h) / 2, w, h},
                  (cui_rect){0}, false, (cui_color){.a = 255});
        } else {
            fill(a, (cui_rect){(side.w - 100) / 2, 9, 78, tile_h - 42}, palette(a, RAISED), 2);
        }
        label(a, (cui_rect){0, tile_h - 28, side.w - 22, 18}, number, a->config.font_size - 2,
              selected ? ACCENT : MUTED, false, true, CUI_ALIGN_CENTER);
        if (hit.activated && a->prompt == PROMPT_NONE) a->frame_item = index;
        cui_end_box(a->ui);
    }
    bool grip = a->drag == DRAG_SIDEBAR || a->pointer == POINTER_COLUMN;
    fill(a, (cui_rect){side.w - 5, (side.h - 42) / 2, 3, 42},
         palette(a, grip ? ACCENT : MUTED), 2);
    cui_end_box(a->ui);
}

static void info_sidebar(App *a) {
    cui_rect side = a->layout.info;
    if (side.w <= 0) return;
    Metadata *m = &a->metadata;
    metadata_load(m, &a->doc);
    cui_begin_portal(a->ui, cui_id_from(a->ui, "file info"),
        (cui_box){.background = palette(a, SURFACE), .clip = true}, side, 2);
    fill(a, (cui_rect){0, 0, 1, side.h}, palette(a, BORDER), 0);
    label(a, (cui_rect){16, 8, side.w - 64, 27}, "File info", a->config.font_size, FOREGROUND,
          true, false, CUI_ALIGN_START);
    if (button(a, (cui_rect){side.w - 40, 8, 27, 27}, "Close file info", ICON_CLOSE, NULL, false, false))
        a->frame_action = INFO;
    fill(a, (cui_rect){15, 40, side.w - 30, 1}, palette(a, BORDER), 0);

    float view_h = side.h - 49, width = side.w - 37;
    int size = a->config.font_size - 2, line_h = size + 5;
    float advance = renderer_measure(&a->renderer, cui_string("M"), size,
                                     (cui_text_style){.monospace = true}).width;
    int columns = fmax(10, floor(width / fmax(1, advance)));
    cui_begin_absolute_box(a->ui, 0, (cui_box){.clip = true}, (cui_rect){14, 43, side.w - 28, view_h});
    int y = 4 - a->info_scroll;
    for (int i = 0; i < m->count; ++i) {
        int top = y;
        const MetadataRow *row = &m->rows[i];
        const char *p = row->value, *end = p + strlen(p);
        y += line_h + 2;
        if (top + line_h > 0 && top < view_h)
            label(a, (cui_rect){2, top, width, line_h}, row->label, size, MUTED, true, false, CUI_ALIGN_START);
        while (p < end) {
            const char *stop = p, *space = NULL;
            for (int j = 0; j < columns && stop < end; ++j) {
                if (*stop == ' ') space = stop;
                utf8_next(&stop, end);
            }
            if (stop < end && space && space > p && stop - space < columns / 2) stop = space;
            if (y + line_h > 0 && y < view_h) {
                char line[4097];
                memcpy(line, p, stop - p);
                line[stop - p] = 0;
                label(a, (cui_rect){2, y, width, line_h}, line, size, FOREGROUND, false, true, CUI_ALIGN_START);
            }
            p = stop;
            while (p < end && *p == ' ') ++p;
            y += line_h;
        }
        y += 11;
        if (y > 0 && top < view_h) {
            char key[36];
            snprintf(key, sizeof key, "Metadata row %d", i);
            cui_id id = cui_id_from(a->ui, key);
            cui_begin_absolute_box(a->ui, id, (cui_box){0}, (cui_rect){0, top, width, y - top - 5});
            cui_interaction hit = cui_interact(a->ui, id, CUI_ROLE_BUTTON, cui_string(row->label),
                                               a->prompt != PROMPT_NONE, false, 0);
            if (hit.activated && a->prompt == PROMPT_NONE) a->frame_meta = i;
            if (hit.hovered && a->prompt == PROMPT_NONE) tooltip_target(a, id, row->label);
            cui_end_box(a->ui);
        }
    }
    cui_end_box(a->ui);
    int content_h = y + a->info_scroll;
    a->info_end = fmax(0, content_h - view_h);
    if (a->info_scroll > a->info_end) { a->info_scroll = a->info_end; a->dirty = true; }
    if (a->info_end > 0 && view_h > 0) {
        float h = fmax(15, view_h * view_h / content_h);
        float top = (view_h - h) * a->info_scroll / a->info_end;
        fill(a, (cui_rect){side.w - 9, 43 + top, 3, h}, palette(a, MUTED), 2);
    }
    if (m->extra.fd >= 0 && y < view_h)
        label(a, (cui_rect){16, 45 + y, side.w - 32, line_h}, "Reading metadata…", size,
              MUTED, false, false, CUI_ALIGN_START);
    cui_end_box(a->ui);
}

static void range_rects(App *a, FPDF_PAGE page, FPDF_TEXTPAGE text, cui_rect bounds,
                        int from, int n, cui_color color) {
    int count = FPDFText_CountRects(text, from, n);
    for (int i = 0; i < count && i < 160; ++i) {
        double l, t, r, b;
        int x0, y0, x1, y1;
        if (FPDFText_GetRect(text, i, &l, &t, &r, &b) &&
            FPDF_PageToDevice(page, 0, 0, roundf(bounds.w), roundf(bounds.h), 0, l, t, &x0, &y0) &&
            FPDF_PageToDevice(page, 0, 0, roundf(bounds.w), roundf(bounds.h), 0, r, b, &x1, &y1)) {
            fill(a, (cui_rect){bounds.x + fmin(x0, x1), bounds.y + fmin(y0, y1),
                 abs(x1 - x0), abs(y1 - y0)}, color, 2);
        }
    }
}

static void pdf_highlights(App *a, int index, cui_rect bounds) {
    Document *d = &a->doc;
    int from, n;
    bool selection = document_range(d, index, &from, &n);
    size_t lo = 0, hi = d->num_matches;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (d->matches[mid].page < index) lo = mid + 1;
        else hi = mid;
    }
    if (!selection && (lo == d->num_matches || d->matches[lo].page != index)) return;
    FPDF_PAGE page;
    FPDF_TEXTPAGE text = document_text(d, index, &page);
    if (text) {
        for (size_t i = lo; i < d->num_matches && d->matches[i].page == index && i - lo < 150; ++i)
            range_rects(a, page, text, bounds, d->matches[i].start, d->matches[i].count,
                        (cui_color){255, 168, 35, (i == (size_t)d->active_match) ? 132 : 76});
        if (selection) {
            cui_color accent = palette(a, ACCENT);
            accent.a = 94;
            range_rects(a, page, text, bounds, from, n, accent);
        }
        FPDFText_ClosePage(text);
    }
    if (page) FPDF_ClosePage(page);
}

static void canvas(App *a) {
    Document *d = &a->doc;
    cui_rect c = a->layout.canvas;
    cui_begin_portal(a->ui, cui_id_from(a->ui, "canvas"),
        (cui_box){.background = a->presenting ? (cui_color){0, 0, 0, 255} : palette(a, CANVAS),
                  .clip = true}, c, 1);
    if (d->pdf) {
        int start = app_page_at(a, c.y);
        for (int i = start; i < d->count && i - start < (a->presenting ? 1 : 32); ++i) {
            cui_rect p = app_page_rect(a, i);
            p.x -= c.x; p.y -= c.y;
            if (p.y >= c.h) break;
            if (p.y + p.h <= 0) continue;
            if (!a->presenting)
                fill(a, (cui_rect){p.x - 1, p.y - 1, p.w + 2, p.h + 2}, palette(a, BORDER), 0);
            fill(a, p, (cui_color){255, 255, 255, 255}, 0);
            int width = fmin(4096, fmax(24, roundf(p.w)));
            Image *im = document_cached(d, i, false, width, true, &a->thumb_pending);
            if (im) image(a, *im, p, (cui_rect){0}, false, (cui_color){.a = 255});
            else label(a, (cui_rect){p.x, p.y, p.w, p.h}, "Page unavailable", a->config.font_size,
                       BORDER, false, false, CUI_ALIGN_CENTER);
            if (!a->presenting) pdf_highlights(a, i, p);
        }
    } else if (d->picture.pixels) {
        Image im = d->picture;
        float w = fmax(1, round(im.w * a->layout.scale)), h = fmax(1, round(im.h * a->layout.scale));
        cui_rect p = {roundf((c.w - w) / 2 + a->pan), roundf((c.h - h) / 2 - a->scroll), w, h};
        if (a->layout.scale < .8 && w * h < 10000000) {
            if (d->scaled_picture.w != (int)w || d->scaled_picture.h != (int)h) {
                image_free(&d->scaled_picture);
                d->scaled_picture = image_resize(im, (int)w, (int)h);
            }
            if (d->scaled_picture.pixels) im = d->scaled_picture;
        }
        image(a, im, p, (cui_rect){0}, false, (cui_color){.a = 255});
    } else {
        float x = (c.w - 56) / 2, y = c.h / 2 - 88;
        fill(a, (cui_rect){x, y, 56, 56}, palette(a, RAISED), 13);
        icon(a, ICON_PHOTO, (cui_rect){x + 14, y + 14, 28, 28}, ACCENT);
        label(a, (cui_rect){0, y + 65, c.w, 32}, "Open an image or PDF", a->config.font_size + 8,
              FOREGROUND, true, false, CUI_ALIGN_CENTER);
        label(a, (cui_rect){18, y + 100, c.w - 36, 24}, "Open a folder to browse its images as a gallery.",
              a->config.font_size, MUTED, false, false, CUI_ALIGN_CENTER);
        if (button(a, (cui_rect){(c.w - 154) / 2, y + 140, 154, 36}, "Open…", -1, "Open…",
                   true, false)) a->frame_action = OPEN;
    }
    cui_end_box(a->ui);
}

static void findbar(App *a) {
    if (!a->doc.pdf || !*a->doc.query || a->fullscreen || a->presenting || a->prompt != PROMPT_NONE) return;
    cui_rect c = a->layout.canvas;
    int w = fmin(c.w - 24, 428);
    if (w < 210) return;
    cui_begin_portal(a->ui, cui_id_from(a->ui, "findbar"),
        (cui_box){.background = palette(a, SURFACE), .border = palette(a, BORDER),
                  .border_width = 1, .radius = 8, .clip = true},
        (cui_rect){c.x + c.w - w - 12, c.y + 10, w, 43}, 4);
    icon(a, ICON_FIND, (cui_rect){11, 13, 18, 18}, ACCENT);
    if (button(a, (cui_rect){35, 7, w - 196, 29}, "Edit search", -1, a->doc.query,
               false, false)) a->frame_action = FIND;
    char count[64];
    Document *d = &a->doc;
    if (d->searching) snprintf(count, sizeof count, "Searching…");
    else if (!d->num_matches) snprintf(count, sizeof count, "No matches");
    else snprintf(count, sizeof count, "%d / %zu%s", d->active_match + 1, d->num_matches,
                  d->search_limited ? "+" : "");
    label(a, (cui_rect){w - 166, 7, 82, 29}, count, a->config.font_size - 3,
          MUTED, false, true, CUI_ALIGN_END);
    if (button(a, (cui_rect){w - 80, 7, 24, 29}, "Previous match", ICON_UP, NULL,
               false, !d->num_matches)) a->frame_action = FIND_PREV;
    if (button(a, (cui_rect){w - 55, 7, 24, 29}, "Next match", ICON_DOWN, NULL,
               false, !d->num_matches)) a->frame_action = FIND_NEXT;
    if (button(a, (cui_rect){w - 29, 7, 24, 29}, "Clear search", ICON_CLOSE, NULL,
               false, false)) a->frame_command = UI_DISMISS;
    cui_end_box(a->ui);
}

static void input_content(App *a, const char *text, size_t index, cui_rect r) {
    box(a, 0, r, palette(a, CANVAS), palette(a, ACCENT), 1, 7, true);
    int size = a->config.font_size;
    int caret = renderer_measure(&a->renderer, (cui_str){text, index}, size,
                                 (cui_text_style){.monospace = true}).width;
    int width = renderer_measure(&a->renderer, cui_string(text), size,
                                 (cui_text_style){.monospace = true}).width;
    float x = fminf(12, r.w - 15 - caret);
    if (a->select_input)
        fill(a, (cui_rect){x, 9, width, r.h - 18}, palette(a, RAISED), 2);
    cui_begin_absolute_box(a->ui, 0, (cui_box){.align_y = CUI_ALIGN_CENTER},
                           (cui_rect){x, 0, fmax(width + 4, r.w - 20), r.h});
    cui_text_ex(a->ui, cui_string(text), size, palette(a, FOREGROUND), 0,
                (cui_text_style){.monospace = true});
    cui_end_box(a->ui);
    if (!a->select_input) fill(a, (cui_rect){x + caret, 11, 1, r.h - 22}, palette(a, ACCENT), 0);
    cui_end_box(a->ui);
}

static void prompt_button(App *a, cui_rect r, const char *name, int command, bool primary) {
    cui_id id = cui_id_from(a->ui, primary ? "dialog confirm" : "dialog dismiss");
    bool hot = cui_interaction_state(a->ui, id).hovered;
    box(a, id, r, palette(a, primary || hot ? RAISED : SURFACE), palette(a, primary ? ACCENT : BORDER),
        1, 7, true);
    cui_interaction hit = cui_interact(a->ui, id, CUI_ROLE_BUTTON, cui_string(name),
                                      false, false, 0);
    label(a, (cui_rect){2, 0, r.w - 4, r.h}, name, a->config.font_size, primary ? ACCENT : FOREGROUND,
          primary, false, CUI_ALIGN_CENTER);
    if (hit.activated) a->frame_command = command;
    cui_end_box(a->ui);
}

static void key_summary(const App *a, Action action, char out[96]) {
    out[0] = 0;
    for (int i = 0; i < 4 && a->config.keys[action][i].symbol; ++i) {
        unsigned mods = a->config.keys[action][i].mods;
        const char *name = XKeysymToString(a->config.keys[action][i].symbol);
        size_t n = strlen(out);
        snprintf(out + n, 96 - n, "%s%s%s%s%s%s", i ? " / " : "",
                 mods & ControlMask ? "Ctrl+" : "", mods & ShiftMask ? "Shift+" : "",
                 mods & Mod1Mask ? "Alt+" : "", mods & Mod4Mask ? "Super+" : "", name ? name : "");
        if (i == 1) break;
    }
}

static bool print_control(App *a, cui_rect r, const char *key, const char *value, int pictogram,
                          bool selected, bool disabled) {
    cui_id id = cui_id_from(a->ui, key);
    cui_interaction before = cui_interaction_state(a->ui, id);
    box(a, id, r, palette(a, selected || before.hovered ? RAISED : SURFACE),
        palette(a, selected || before.focus_visible ? ACCENT : BORDER), 1, 5, true);
    cui_interaction hit = cui_interact(a->ui, id, CUI_ROLE_BUTTON, cui_string(key), disabled, selected, 0);
    if (pictogram >= 0)
        icon(a, pictogram, (cui_rect){(r.w - 15) / 2, (r.h - 15) / 2, 15, 15}, disabled ? MUTED : ACCENT);
    else
        label(a, (cui_rect){6, 0, r.w - 12, r.h}, value, a->config.font_size - 2,
              disabled ? MUTED : FOREGROUND, false, false, CUI_ALIGN_CENTER);
    cui_end_box(a->ui);
    return hit.activated && !disabled;
}

static void sheet_preview(App *a, cui_rect r, Image converted[9]) {
    const PrintSettings *p = &a->print.settings;
    PrintSheet sheet = print_sheet(p);
    int first, last, cells = sheet.rows * sheet.cols;
    bool valid = print_range(a, &first, &last);
    int sheets = valid ? (last - first + cells) / cells : 0;
    box(a, 0, r, palette(a, CANVAS), palette(a, BORDER), 1, 8, true);
    float s = fmin((r.w - 40) / sheet.w, (r.h - 52) / sheet.h);
    float dx = (r.w - sheet.w * s) / 2, dy = (r.h - 29 - sheet.h * s) / 2;
    fill(a, (cui_rect){dx + 3, dy + 4, sheet.w * s, sheet.h * s}, palette(a, BORDER), 1);
    fill(a, (cui_rect){dx, dy, sheet.w * s, sheet.h * s}, (cui_color){255, 255, 255, 255}, 1);
    for (int i = 0; valid && i < cells && first + i <= last; ++i) {
        int index = first + i;
        cui_rect cell = print_cell(p, sheet, i);
        Image *im = thumbnail(a, index, 256);
        double w = a->doc.pdf ? a->doc.pages[index].w : im ? im->w : a->doc.picture.w;
        double h = a->doc.pdf ? a->doc.pages[index].h : im ? im->h : a->doc.picture.h;
        cui_rect fit = print_fit(cell, p->option[PRINT_SCALE], w, h);
        cui_rect bounds = {dx + fit.x * s, dy + fit.y * s, fit.w * s, fit.h * s};
        if (im && p->option[PRINT_COLOR]) {
            converted[i] = image_resize(*im, im->w, im->h);
            if (converted[i].pixels) {
                im = &converted[i];
                for (size_t px = 0; px < (size_t)im->w * im->h * 4; px += 4) {
                    unsigned char *pix = im->pixels + px;
                    pix[0] = pix[1] = pix[2] = (77 * pix[0] + 150 * pix[1] + 29 * pix[2] + 128) >> 8;
                }
            }
        }
        if (im) image(a, *im, bounds, (cui_rect){0}, false, (cui_color){.a = 255});
        else fill(a, bounds, (cui_color){225, 225, 225, 255}, 0);
    }
    char count[72];
    if (sheets > 1) snprintf(count, sizeof count, "Sheet 1 of %d", sheets);
    else snprintf(count, sizeof count, "%s", sheets ? "1 sheet" : "Enter a valid range");
    label(a, (cui_rect){8, r.h - 29, r.w - 16, 24}, count, a->config.font_size - 2,
          MUTED, false, true, CUI_ALIGN_CENTER);
    cui_end_box(a->ui);
}

static void print_dialog(App *a, int w, int h, Image converted[9]) {
    static const char *const fields[] = {"Printer", "Items", "Paper", "Orientation", "Layout",
        "Order", "Scale", "Color", "Sides", "Copies"};
    bool wide = w >= 615, busy = a->print.job.fd >= 0;
    int width = wide ? fmax(290, w * .46) : w - 40;
    int row = 33, view_h = h - 123, value_x = fmin(109, width * .36);
    bool range = a->print.settings.option[PRINT_ITEMS] == 2;
    a->print.scroll_end = fmax(0, (PRINT_FIELDS + range) * row + 2 - view_h);
    a->print.scroll = fmin(a->print.scroll, a->print.scroll_end);
    label(a, (cui_rect){20, 12, w - 40, 32}, "Print", a->config.font_size + 7,
          FOREGROUND, true, false, CUI_ALIGN_START);
    label(a, (cui_rect){20, 44, w - 40, 19}, *a->prompt_error ? a->prompt_error : base_name(a->doc.path),
          a->config.font_size - 2, *a->prompt_error ? ACCENT : MUTED, false, false, CUI_ALIGN_START);
    cui_begin_absolute_box(a->ui, 0, (cui_box){.clip = true}, (cui_rect){20, 69, width, view_h});
    int y = 1 - a->print.scroll;
    for (int i = 0; i < PRINT_FIELDS; ++i, y += row) {
        if (y + row > 0 && y < view_h) {
            char prev[42], next[42], buf[32];
            snprintf(prev, sizeof prev, "Previous %s", fields[i]);
            snprintf(next, sizeof next, "Next %s", fields[i]);
            label(a, (cui_rect){0, y, value_x - 7, 29}, fields[i], a->config.font_size,
                  FOREGROUND, false, false, CUI_ALIGN_START);
            bool disabled = busy || (i == PRINT_PRINTER && !a->print.num_queues) ||
                            (i == PRINT_ITEMS && a->doc.count < 2) ||
                            (i == PRINT_ORDER && !a->print.settings.option[PRINT_GRID]);
            int left = value_x, right = width - 34;
            const char *value = print_value(a, i, buf);
            if (print_control(a, (cui_rect){left, y, 27, 29}, prev, NULL, ICON_LEFT, false, disabled))
                a->frame_command = UI_PRINT_STEP + 2 * i;
            if (print_control(a, (cui_rect){right, y, 27, 29}, next, NULL, ICON_RIGHT, false, disabled))
                a->frame_command = UI_PRINT_STEP + 2 * i + 1;
            if (print_control(a, (cui_rect){left + 31, y, right - left - 35, 29}, fields[i],
                              value, -1, i == PRINT_COLOR && a->print.settings.option[i], disabled))
                a->frame_command = UI_PRINT_STEP + 2 * i + 1;
        }
        if (i == PRINT_ITEMS && range) {
            y += row;
            if (y + row <= 0 || y >= view_h) continue;
            label(a, (cui_rect){0, y, value_x - 7, 29}, "Range", a->config.font_size,
                  FOREGROUND, false, false, CUI_ALIGN_START);
            cui_rect field = {value_x, y, width - value_x - 7, 29};
            if (a->print.editable && !busy) input_content(a, a->input, a->caret, field);
            else if (print_control(a, field, "Edit range", a->input, -1, false, busy))
                a->frame_command = UI_PRINT_RANGE;
        }
    }
    cui_end_box(a->ui);
    if (wide) sheet_preview(a, (cui_rect){width + 36, 69, w - width - 56, view_h}, converted);
    if (a->print.scroll_end > 0) {
        float bar = fmax(15, view_h * (float)view_h / (view_h + a->print.scroll_end));
        float y = 69 + (view_h - bar) * a->print.scroll / a->print.scroll_end;
        fill(a, (cui_rect){width + 21, y, 3, bar}, palette(a, MUTED), 2);
    }
    prompt_button(a, (cui_rect){w - 254, h - 43, 100, 30}, busy ? "Close" : "Cancel", UI_DISMISS, false);
    if (busy) label(a, (cui_rect){w - 150, h - 43, 130, 30}, "Preparing…", a->config.font_size - 1,
                    MUTED, false, false, CUI_ALIGN_CENTER);
    else prompt_button(a, (cui_rect){w - 145, h - 43, 125, 30}, "Print", UI_SUBMIT, true);
}

static void help(App *a, int w, int h) {
    static const struct { const char *name; Action action; } rows[] = {
        {"Open", OPEN}, {"Fullscreen", FULLSCREEN}, {"Next", NEXT}, {"Previous", PREV},
        {"Gallery", GALLERY}, {"Page thumbnails", SIDEBAR}, {"Zoom in", ZOOM_IN},
        {"Zoom out", ZOOM_OUT}, {"Fit", FIT}, {"Actual size", ACTUAL},
        {"Find in PDF", FIND}, {"Next match", FIND_NEXT}, {"Copy PDF text", COPY},
        {"File info", INFO}, {"Print", PRINT}, {"Present", PRESENT}, {"Reload settings", RELOAD}, {"Quit", QUIT}
    };
    label(a, (cui_rect){22, 13, w - 44, 32}, "Keyboard shortcuts", a->config.font_size + 6,
          FOREGROUND, true, false, CUI_ALIGN_START);
    int row = fmin(24, fmax(14, (h - 100) / 9));
    for (size_t i = 0; i < sizeof rows / sizeof *rows; ++i) {
        int col = i % 2, x = 22 + col * ((w - 36) / 2), y = 51 + (i / 2) * row;
        char key[96];
        key_summary(a, rows[i].action, key);
        int half = (w - 54) / 2;
        label(a, (cui_rect){x, y, half / 2, row}, rows[i].name, a->config.font_size - 2,
              FOREGROUND, false, false, CUI_ALIGN_START);
        label(a, (cui_rect){x + half / 2, y, half / 2, row}, key, a->config.font_size - 3,
              ACCENT, false, true, CUI_ALIGN_END);
    }
    prompt_button(a, (cui_rect){w - 112, h - 46, 90, 30}, "Close", UI_DISMISS, false);
}

static void prompt(App *a, Image converted[9]) {
    if (a->prompt == PROMPT_NONE) return;
    cui_begin_portal(a->ui, cui_id_from(a->ui, "dialog overlay"), (cui_box){
        .background = {0, 0, 0, 129}, .clip = true
    }, (cui_rect){0, 0, a->w, a->h}, 10);
    cui_interact(a->ui, cui_id_from(a->ui, "dialog overlay"), CUI_ROLE_NONE, cui_string(""), false, false, 0);
    int w = fmin(a->w - 30, a->prompt == PROMPT_PRINT ? 780 : a->prompt == PROMPT_HELP ? 640 : 548);
    int h = fmin(a->h - 24, a->prompt == PROMPT_PRINT ? 504 : a->prompt == PROMPT_HELP ? 332 : 184);
    box(a, cui_id_from(a->ui, "dialog"), (cui_rect){(a->w - w) / 2, (a->h - h) / 2, w, h},
        palette(a, SURFACE), palette(a, BORDER), 1, 12, true);
    if (a->prompt == PROMPT_HELP) {
        help(a, w, h);
    } else if (a->prompt == PROMPT_PRINT) {
        print_dialog(a, w, h, converted);
    } else {
        const char *title = a->prompt == PROMPT_OPEN ? "Open file or folder" :
                            a->prompt == PROMPT_FIND ? "Find in PDF" : "Unlock PDF";
        const char *description = a->prompt == PROMPT_OPEN ? "Enter a path to an image, PDF, or folder." :
            a->prompt == PROMPT_FIND ? "Search the document’s text." : base_name(a->password_path);
        label(a, (cui_rect){20, 15, w - 40, 28}, title, a->config.font_size + 5, FOREGROUND,
              true, false, CUI_ALIGN_START);
        label(a, (cui_rect){20, 44, w - 40, 20}, *a->prompt_error ? a->prompt_error : description,
              a->config.font_size - 1, *a->prompt_error ? ACCENT : MUTED, false, false, CUI_ALIGN_START);
        char dots[4096];
        const char *text = a->input;
        if (a->prompt == PROMPT_PASSWORD) {
            size_t n = strlen(a->input);
            memset(dots, '*', n);
            dots[n] = 0;
            text = dots;
        }
        input_content(a, text, a->caret, (cui_rect){20, 75, w - 40, 41});
        prompt_button(a, (cui_rect){w - 205, h - 49, 88, 32}, "Cancel", UI_DISMISS, false);
        prompt_button(a, (cui_rect){w - 110, h - 49, 90, 32},
                      a->prompt == PROMPT_FIND ? "Find" : a->prompt == PROMPT_PASSWORD ? "Unlock" : "Open",
                      UI_SUBMIT, true);
    }
    cui_end_box(a->ui);
    cui_end_box(a->ui);
}

static void floating_status(App *a) {
    double time = now();
    const char *text = NULL;
    bool toast = a->toast_until > time;
    if (toast) text = a->toast;
    else if (*a->hover_label && time - a->hover_since > .55) text = a->hover_label;
    if (!text || !*text || a->prompt != PROMPT_NONE) return;
    int size = a->config.font_size - 2;
    float w = fmin(a->w - 22, renderer_measure(&a->renderer, cui_string(text), size,
                                               (cui_text_style){0}).width + 24);
    float x = toast ? (a->w - w) / 2 : limit(a->mx + 9, 10, a->w - w - 10);
    float y = toast ? a->layout.canvas.y + a->layout.canvas.h - 48 :
        limit(a->my < 70 ? a->my + 17 : a->my - 38, 9, a->h - 39);
    cui_begin_portal(a->ui, cui_id_from(a->ui, "notice"), (cui_box){.background = palette(a, RAISED),
         .border = palette(a, BORDER), .border_width = 1, .radius = 7, .inert = true, .clip = true},
         (cui_rect){x, y, w, 29}, 20);
    label(a, (cui_rect){6, 0, w - 12, 29}, text, size, FOREGROUND, false, false, CUI_ALIGN_CENTER);
    cui_end_box(a->ui);
}

void app_draw(App *a) {
    Image converted[9] = {0};
    app_layout(a);
    a->dirty = false;
    a->frame_action = ACTIONS;
    a->frame_item = -1;
    a->frame_meta = -1;
    a->frame_command = UI_NONE;
    a->thumb_pending = false;
    double time = now();
    a->thumb_budget = time >= a->next_thumbnail ? 1 : 0;
    ++a->doc.stamp;
    if (!cui_begin_frame(a->ui, a->w, a->h, time)) return;
    a->hover_label[0] = 0;
    cui_begin_box(a->ui, cui_id_from(a->ui, "view"), (cui_box){.width = -1, .height = -1});
    canvas(a);
    if (!a->fullscreen && !a->presenting) {
        page_sidebar(a);
        filmstrip(a);
        info_sidebar(a);
        topbar(a);
        footer(a);
    }
    findbar(a);
    if (a->prompt != PROMPT_NONE) prompt(a, converted);
    floating_status(a);
    cui_end_box(a->ui);
    cui_draw_list list = cui_end_frame(a->ui);
    if (cui_last_error(a->ui) != CUI_OK) {
        cui_diagnostic info;
        if (cui_first_diagnostic(a->ui, &info))
            fprintf(stderr, "view: CUI %s: %s\n", info.operation, info.message);
        cui_clear_error(a->ui);
    }
    renderer_paint(&a->renderer, list, palette(a, BACKGROUND));
    for (size_t i = 0; i < sizeof converted / sizeof *converted; ++i) image_free(&converted[i]);
    if (!a->thumb_budget && time >= a->next_thumbnail) a->next_thumbnail = now() + .012;
    if (!*a->hover_label) a->hovered_id = 0;
    if (a->frame_action != ACTIONS) app_action(a, a->frame_action);
    if (a->frame_item >= 0) app_item(a, a->frame_item);
    if (a->frame_meta >= 0 && a->frame_meta < a->metadata.count) {
        app_set_clipboard(a, strdup(a->metadata.rows[a->frame_meta].value));
        app_toast(a, "Copied to clipboard");
    }
    if (a->frame_command != UI_NONE) app_command(a, a->frame_command);
}
