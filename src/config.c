#include "view.h"
#include <X11/Xresource.h>
#include <X11/Xatom.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *const action_names[ACTIONS] = {
    "open", "prev", "next", "first", "last", "zoom-in", "zoom-out", "fit", "actual",
    "fullscreen", "gallery", "sidebar", "find", "find-next", "find-prev", "copy",
    "up", "down", "page-up", "page-down", "reload", "help", "quit"
};

static const char *const default_keys[ACTIONS] = {
    "Ctrl+o", "Left,h", "Right,l", "Home", "End", "equal,plus,KP_Add", "minus,KP_Subtract",
    "0", "1", "f,F11", "g", "t", "Ctrl+f,slash", "n,F3", "Shift+n,Shift+F3", "Ctrl+c",
    "Up,k", "Down,j", "Page_Up", "Page_Down,space", "Ctrl+r", "F1,Shift+slash", "q,Ctrl+w"
};

static const char *const color_names[COLOR_COUNT] = {
    "background", "canvas", "surface", "raised", "border", "foreground", "muted", "accent"
};

static cui_color mix(cui_color a, cui_color b, float amount) {
    return (cui_color) {
        a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount,
        a.b + (b.b - a.b) * amount, 255
    };
}

static cui_color color_value(Display *dpy, const char *value, cui_color fallback) {
    XColor x;
    if (!XParseColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), value, &x)) return fallback;
    return (cui_color){x.red >> 8, x.green >> 8, x.blue >> 8, 255};
}

static const char *resource(XrmDatabase db, const char *name, const char *class) {
    XrmValue value;
    char *type;
    return db && XrmGetResource(db, name, class, &type, &value) ? value.addr : "";
}

static void palette(Config *c, Display *dpy) {
    XrmInitialize();
    unsigned char *source = NULL;
    XrmDatabase db = NULL;
    if (c->xresources) {
        Atom type;
        int format;
        unsigned long size, tail;
        if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), XA_RESOURCE_MANAGER, 0, 262144,
                               False, XA_STRING, &type, &format, &size, &tail, &source) == Success &&
            format == 8 && source && !tail)
            db = XrmGetStringDatabase((const char *)source);
        if (source) XFree(source);
    }
    cui_color *p = c->colors;
    p[BACKGROUND] = color_value(dpy, resource(db, "view.background", "View.Background"),
                                (cui_color){23, 15, 22, 255});
    p[FOREGROUND] = color_value(dpy, resource(db, "view.foreground", "View.Foreground"),
                                (cui_color){250, 233, 239, 255});
    p[ACCENT] = color_value(dpy, resource(db, "view.color4", "View.Color4"),
                            (cui_color){229, 104, 208, 255});
    p[CANVAS] = mix(p[BACKGROUND], (cui_color){0, 0, 0, 255}, .22);
    p[SURFACE] = mix(p[BACKGROUND], p[FOREGROUND], .045);
    p[RAISED] = mix(p[BACKGROUND], p[FOREGROUND], .085);
    p[BORDER] = mix(p[BACKGROUND], p[FOREGROUND], .16);
    p[MUTED] = mix(p[BACKGROUND], p[FOREGROUND], .64);
    for (int i = 0; i < COLOR_COUNT; ++i) {
        char name[64], class[64];
        snprintf(name, sizeof name, "view.%s", color_names[i]);
        snprintf(class, sizeof class, "View.%c%s", toupper(color_names[i][0]), color_names[i] + 1);
        p[i] = color_value(dpy, resource(db, name, class), p[i]);
    }
    if (db) XrmDestroyDatabase(db);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

static void key_list(Config *c, Action action, const char *value) {
    char copy[256];
    snprintf(copy, sizeof copy, "%s", value);
    memset(c->keys[action], 0, sizeof c->keys[action]);
    char *next = NULL;
    int i = 0;
    for (char *key = strtok_r(copy, ",", &next); key && i < 4; key = strtok_r(NULL, ",", &next)) {
        unsigned mods = 0;
        key = trim(key);
        while (true) {
            if (!strncasecmp(key, "Ctrl+", 5)) { mods |= ControlMask; key += 5; }
            else if (!strncasecmp(key, "Shift+", 6)) { mods |= ShiftMask; key += 6; }
            else if (!strncasecmp(key, "Alt+", 4)) { mods |= Mod1Mask; key += 4; }
            else if (!strncasecmp(key, "Super+", 6)) { mods |= Mod4Mask; key += 6; }
            else break;
        }
        KeySym symbol = XStringToKeysym(key);
        if (symbol != NoSymbol) {
            c->keys[action][i].symbol = symbol;
            c->keys[action][i++].mods = mods;
        }
    }
}

static int integer(const char *s, int fallback, long min, long max) {
    char *end;
    long v = strtol(s, &end, 10);
    return end != s && !*end && v >= min && v <= max ? (int)v : fallback;
}

static bool boolean(const char *s, bool fallback) {
    if (!strcasecmp(s, "true")) return true;
    if (!strcasecmp(s, "false")) return false;
    return fallback;
}

void config_load(Config *c, Display *dpy) {
    *c = (Config){
        .gallery = true, .sidebar = true, .wrap = true, .xresources = true, .check_updates = true,
        .width = 1120, .height = 780, .font_size = 14, .thumb_size = 78,
        .scroll_step = 108, .sidebar_width = 168, .padding = 28, .zoom_step = 1.25,
    };
    for (int i = 0; i < ACTIONS; ++i) key_list(c, i, default_keys[i]);

    const char *root = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[4096];
    if (root && *root == '/') snprintf(path, sizeof path, "%s/view/config", root);
    else snprintf(path, sizeof path, "%s/.config/view/config", home ? home : "");
    FILE *file = fopen(path, "r");
    char overrides[COLOR_COUNT][64] = {{0}};
    char line[8192];
    while (file && fgets(line, sizeof line, file)) {
        char *name = trim(line);
        if (*name == '#' || !*name) continue;
        char *value = strchr(name, '=');
        if (!value) continue;
        *value++ = 0;
        name = trim(name);
        value = trim(value);

        struct { const char *name; int *field; int min, max; } sizes[] = {
            {"width", &c->width, 360, 7680}, {"height", &c->height, 260, 4320},
            {"font_size", &c->font_size, 10, 28}, {"thumb_size", &c->thumb_size, 40, 160},
            {"scroll_step", &c->scroll_step, 16, 1200},
            {"sidebar_width", &c->sidebar_width, 100, 320}, {"padding", &c->padding, 4, 96},
        };
        struct { const char *name; bool *field; } flags[] = {
            {"gallery", &c->gallery}, {"sidebar", &c->sidebar}, {"wrap", &c->wrap},
            {"upscale", &c->upscale}, {"check_updates", &c->check_updates},
            {"xresources", &c->xresources},
        };
        for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i)
            if (!strcmp(name, sizes[i].name))
                *sizes[i].field = integer(value, *sizes[i].field, sizes[i].min, sizes[i].max);
        for (size_t i = 0; i < sizeof flags / sizeof *flags; ++i)
            if (!strcmp(name, flags[i].name)) *flags[i].field = boolean(value, *flags[i].field);
        if (!strcmp(name, "zoom_step")) {
            char *end;
            double step = strtod(value, &end);
            if (end != value && !*end && step >= 1.05 && step <= 3) c->zoom_step = step;
        }
        const char *const font_names[] = {"font", "font_bold", "font_mono"};
        for (int i = 0; i < 3; ++i) {
            if (strcmp(name, font_names[i])) continue;
            if (value[0] == '~' && value[1] == '/' && home)
                snprintf(c->fonts[i], sizeof c->fonts[i], "%s%s", home, value + 1);
            else
                snprintf(c->fonts[i], sizeof c->fonts[i], "%s", value);
        }
        if (!strncmp(name, "key.", 4))
            for (int i = 0; i < ACTIONS; ++i)
                if (!strcmp(name + 4, action_names[i])) key_list(c, i, value);
        if (!strncmp(name, "color.", 6))
            for (int i = 0; i < COLOR_COUNT; ++i)
                if (!strcmp(name + 6, color_names[i]))
                    snprintf(overrides[i], sizeof overrides[i], "%s", value);
    }
    if (file) fclose(file);
    palette(c, dpy);
    for (int i = 0; i < COLOR_COUNT; ++i)
        if (*overrides[i]) c->colors[i] = color_value(dpy, overrides[i], c->colors[i]);
}
