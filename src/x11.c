#include "app.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool point(cui_rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static bool in_findbar(App *, int, int);

static void atom_init(App *a) {
    static char *names[] = {
        "WM_DELETE_WINDOW", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE",
        "CLIPBOARD", "TARGETS", "UTF8_STRING", "VIEW_PASTE", "text/uri-list",
        "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndDrop",
        "XdndFinished", "XdndSelection", "XdndActionCopy", "XdndLeave"
    };
    Atom *const targets[] = {
        &a->wm_delete, &a->fullscreen_atom, &a->state_atom,
        &a->clipboard_atom, &a->targets_atom, &a->utf8_atom, &a->paste_atom, &a->uri_atom,
        &a->xdnd_aware, &a->xdnd_enter, &a->xdnd_position, &a->xdnd_status, &a->xdnd_drop,
        &a->xdnd_finished, &a->xdnd_selection, &a->xdnd_action_copy, &a->xdnd_leave
    };
    enum { count = sizeof names / sizeof *names };
    Atom atoms[count];
    XInternAtoms(a->display, names, count, False, atoms);
    for (int i = 0; i < count; ++i) *targets[i] = atoms[i];
}

static bool has_atom(App *a, Window window, Atom property, Atom wanted) {
    unsigned long n = 0, rem = 0;
    unsigned char *data = NULL;
    Atom type;
    int format;
    bool found = false;
    if (XGetWindowProperty(a->display, window, property, 0, 4096, False, XA_ATOM,
                           &type, &format, &n, &rem, &data) == Success && format == 32) {
        Atom *values = (Atom *)data;
        for (unsigned long i = 0; i < n; ++i) found |= values[i] == wanted;
    }
    if (data) XFree(data);
    return found;
}

static void window_icon(App *a) {
    const unsigned side = 48;
    unsigned long pixels[2 + side * side];
    pixels[0] = side; pixels[1] = side;
    Image image = a->icons[ICON_PHOTO];
    cui_color accent = a->config.colors[ACCENT], bg = a->config.colors[RAISED];
    for (unsigned y = 0; y < side; ++y) {
        for (unsigned x = 0; x < side; ++x) {
            int gx = (int)x - 7, gy = (int)y - 7;
            unsigned alpha = image.pixels && gx >= 0 && gx < 34 && gy >= 0 && gy < 34 ?
                image.pixels[((size_t)(gy * image.h / 34) * image.w + gx * image.w / 34) * 4 + 3] : 0;
            unsigned r = (bg.r * (255 - alpha) + accent.r * alpha) / 255;
            unsigned g = (bg.g * (255 - alpha) + accent.g * alpha) / 255;
            unsigned b = (bg.b * (255 - alpha) + accent.b * alpha) / 255;
            int cx = x < 8 ? 8 - (int)x : x >= side - 8 ? (int)x - (int)side + 9 : 0;
            int cy = y < 8 ? 8 - (int)y : y >= side - 8 ? (int)y - (int)side + 9 : 0;
            pixels[2 + y * side + x] = cx * cx + cy * cy <= 74 ? 0xff000000 | r << 16 | g << 8 | b : 0;
        }
    }
    XChangeProperty(a->display, a->window, XInternAtom(a->display, "_NET_WM_ICON", False),
                    XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)pixels, 2 + side * side);
}

bool x11_init(App *a) {
    setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    a->display = XOpenDisplay(NULL);
    if (!a->display) return false;
    fcntl(ConnectionNumber(a->display), F_SETFD, FD_CLOEXEC);
    int screen = DefaultScreen(a->display);
    Window root = RootWindow(a->display, screen);
    config_load(&a->config, a->display);
    XSelectInput(a->display, root, PropertyChangeMask);
    a->w = fmin(a->config.width, DisplayWidth(a->display, screen));
    a->h = fmin(a->config.height, DisplayHeight(a->display, screen));
    cui_color bg = a->config.colors[BACKGROUND];
    a->window = XCreateSimpleWindow(a->display, root, 0, 0, a->w, a->h, 0, 0,
                                    (unsigned)bg.r << 16 | (unsigned)bg.g << 8 | bg.b);
    if (!a->window) return false;
    XSelectInput(a->display, a->window, ExposureMask | StructureNotifyMask | KeyPressMask |
        KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
        FocusChangeMask | PropertyChangeMask | EnterWindowMask | LeaveWindowMask);
    XSizeHints hints = {.flags = PMinSize, .min_width = 360, .min_height = 260};
    XSetWMNormalHints(a->display, a->window, &hints);
    XClassHint class = {.res_name = "view", .res_class = "View"};
    XSetClassHint(a->display, a->window, &class);
    XStoreName(a->display, a->window, "View");
    atom_init(a);
    XSetWMProtocols(a->display, a->window, &a->wm_delete, 1);
    a->fallback_fullscreen = !has_atom(a, root, XInternAtom(a->display, "_NET_SUPPORTED", False),
                                       a->fullscreen_atom);
    unsigned long aware = 5;
    XChangeProperty(a->display, a->window, a->xdnd_aware, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&aware, 1);
    a->im = XOpenIM(a->display, NULL, NULL, NULL);
    if (a->im) {
        a->ic = XCreateIC(a->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, a->window, XNFocusWindow, a->window, NULL);
    }
    Bool detectable = False;
    XkbSetDetectableAutoRepeat(a->display, True, &detectable);
    renderer_init(&a->renderer, a->display, a->window, &a->config);
    a->ui = cui_create((cui_config){.width = a->w, .height = a->h, .max_elements = 3072,
        .max_commands = 4096, .frame_bytes = 192 * 1024, .measure_styled = renderer_measure,
        .font_metrics = renderer_metrics, .measure_user = &a->renderer});
    if (!a->ui) return false;
    app_icons(a);
    window_icon(a);
    XMapWindow(a->display, a->window);
    XFlush(a->display);
    a->gallery = a->config.gallery;
    a->sidebar = a->config.sidebar;
    a->info = a->config.info;
    a->thumb_size = a->config.thumb_size;
    a->sidebar_width = a->config.sidebar_width;
    a->show_cursor = true;
    a->dirty = true;
    return true;
}

void x11_close(App *a) {
    if (a->ui) cui_destroy(a->ui);
    for (int i = 0; i < ICONS; ++i) image_free(&a->icons[i]);
    if (!a->display) return;
    if (a->ic) XDestroyIC(a->ic);
    if (a->im) XCloseIM(a->im);
    for (int i = 0; i < POINTER_COUNT; ++i)
        if (a->cursors[i]) XFreeCursor(a->display, a->cursors[i]);
    renderer_destroy(&a->renderer);
    if (a->window) XDestroyWindow(a->display, a->window);
    XCloseDisplay(a->display);
}

static Cursor bitmap_cursor(App *a, const char *const rows[16], int hot_x, int hot_y, bool transpose) {
    xcb_point_t light[256], outline[256];
    unsigned light_count = 0, outline_count = 0;
    for (int y = 0; rows && y < 16 && rows[y]; ++y) {
        for (int x = 0; rows[y][x]; ++x) {
            xcb_point_t point = transpose ? (xcb_point_t){y, x} : (xcb_point_t){x, y};
            if (rows[y][x] != ' ') outline[outline_count++] = point;
            if (rows[y][x] == '.') light[light_count++] = point;
        }
    }
    /* Xlib's cursor helpers dlopen libXcursor, which can load a second libc into a static build. */
    XFlush(a->display);
    xcb_connection_t *conn = XGetXCBConnection(a->display);
    xcb_pixmap_t source = xcb_generate_id(conn), mask = xcb_generate_id(conn);
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_cursor_t cursor = xcb_generate_id(conn);
    uint32_t foreground = 0;
    xcb_rectangle_t clear = {0, 0, 16, 16};
    xcb_create_pixmap(conn, 1, source, a->window, 16, 16);
    xcb_create_pixmap(conn, 1, mask, a->window, 16, 16);
    xcb_create_gc(conn, gc, source, XCB_GC_FOREGROUND, &foreground);
    xcb_poly_fill_rectangle(conn, source, gc, 1, &clear);
    xcb_poly_fill_rectangle(conn, mask, gc, 1, &clear);
    foreground = 1;
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &foreground);
    if (light_count) xcb_poly_point(conn, XCB_COORD_MODE_ORIGIN, source, gc, light_count, light);
    if (outline_count) xcb_poly_point(conn, XCB_COORD_MODE_ORIGIN, mask, gc, outline_count, outline);
    xcb_create_cursor(conn, cursor, source, mask, 65535, 65535, 65535, 0, 0, 0, hot_x, hot_y);
    xcb_free_gc(conn, gc);
    xcb_free_pixmap(conn, source);
    xcb_free_pixmap(conn, mask);
    xcb_flush(conn);
    return cursor;
}

void x11_cursor(App *a) {
    static const char *const zoom[16] = {
        "   #####", " ##.....##", " #.#####.#", "#.#.....#.#", "#.#..#..#.#", "#.#.###.#.#",
        "#.#..#..#.#", "#.#.....#.#", " #.#####.##", " ##.....#..#", "   #######..#",
        "          #..#", "           #..#", "            ###"
    };
    static const char *const hand[16] = {
        "     ###", "   ###.###", "   #.#.#.###", "   #.#.#.#.#", "   #.......#",
        " ###.......#", " #.........#", "  #........#", "   #.......#",
        "   #......#", "    #.....#", "    #.....#", "    #######"
    };
    static const char *const resize[16] = {
        "       ##", "      #..#", "     #....#", "    #......#", "    ###..###", "      #..#",
        "      #..#", "      #..#", "      #..#", "      #..#", "      #..#",
        "    ###..###", "    #......#", "     #....#", "      #..#", "       ##"
    };
    Pointer next = POINTER_ARROW;
    if (!a->show_cursor) next = POINTER_HIDDEN;
    else if (a->prompt == PROMPT_NONE && !a->presenting) {
        if (a->drag == DRAG_PAN) next = POINTER_HAND;
        else if (a->drag == DRAG_STRIP || point(a->layout.strip_grip, a->mx, a->my)) next = POINTER_ROW;
        else if (a->drag == DRAG_SIDEBAR || point(a->layout.sidebar_grip, a->mx, a->my)) next = POINTER_COLUMN;
        else if (a->ctrl && a->doc.path && point(a->layout.canvas, a->mx, a->my) &&
                 !in_findbar(a, a->mx, a->my)) next = POINTER_ZOOM;
    }
    if (a->pointer == next) return;
    if (a->pointer == POINTER_ROW || a->pointer == POINTER_COLUMN ||
        next == POINTER_ROW || next == POINTER_COLUMN) a->dirty = true;
    a->pointer = next;
    if (next == POINTER_ARROW) XUndefineCursor(a->display, a->window);
    else {
        if (!a->cursors[next]) {
            const char *const *pattern = next == POINTER_ZOOM ? zoom : next == POINTER_HAND ? hand :
                                         next == POINTER_HIDDEN ? NULL : resize;
            int x = next == POINTER_ZOOM ? 5 : next == POINTER_HIDDEN ? 0 : 7;
            int y = next == POINTER_ZOOM ? 5 : next == POINTER_HAND ? 6 : x;
            a->cursors[next] = bitmap_cursor(a, pattern, x, y, next == POINTER_COLUMN);
        }
        XDefineCursor(a->display, a->window, a->cursors[next]);
    }
    XFlush(a->display);
}

void x11_show_cursor(App *a, bool show) {
    a->show_cursor = show;
    x11_cursor(a);
}

void x11_raise(App *a) {
    XMapRaised(a->display, a->window);
    XEvent event = {.xclient = {.type = ClientMessage, .window = a->window,
        .message_type = XInternAtom(a->display, "_NET_ACTIVE_WINDOW", False),
        .format = 32, .data.l = {1, CurrentTime, 0, 0, 0}}};
    XSendEvent(a->display, DefaultRootWindow(a->display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(a->display);
}

static size_t previous_char(const char *s, size_t i) {
    if (i) --i;
    while (i && ((unsigned char)s[i] & 0xc0) == 0x80) --i;
    return i;
}

static size_t next_char(const char *s, size_t i) {
    if (s[i]) ++i;
    while (((unsigned char)s[i] & 0xc0) == 0x80) ++i;
    return i;
}

static void insert(App *a, const char *bytes, size_t n) {
    if (!n) return;
    size_t len = strlen(a->input);
    if (a->select_input) {
        len = 0;
        a->input[0] = 0;
        a->caret = 0;
        a->select_input = false;
    }
    if (n >= sizeof a->input - len) n = sizeof a->input - len - 1;
    const char *s = bytes, *end = bytes + n;
    char filtered[4096];
    size_t valid = 0;
    while (s < end) {
        uint32_t ch = utf8_next(&s, end);
        if (ch >= 32 && ch != 127) {
            char tmp[4];
            size_t count = utf8_encode(tmp, ch);
            if (count > n - valid) break;
            memcpy(filtered + valid, tmp, count);
            valid += count;
        }
    }
    memmove(a->input + a->caret + valid, a->input + a->caret, len - a->caret + 1);
    memcpy(a->input + a->caret, filtered, valid);
    a->caret += valid;
    a->prompt_error[0] = 0;
    a->dirty = true;
}

static void selection_request(App *a, const XSelectionRequestEvent *e) {
    Atom property = e->property == None ? e->target : e->property;
    Atom result = None;
    if (e->target == a->targets_atom) {
        Atom targets[] = {a->targets_atom, a->utf8_atom, XA_STRING};
        XChangeProperty(a->display, e->requestor, property, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)targets, sizeof targets / sizeof *targets);
        result = property;
    } else if (a->clipboard && (e->target == a->utf8_atom || e->target == XA_STRING)) {
        size_t n = strlen(a->clipboard);
        long request = XExtendedMaxRequestSize(a->display);
        if (!request) request = XMaxRequestSize(a->display);
        size_t chunk = request > 32 ? (request - 32) * 4 : 0;
        for (size_t offset = 0; chunk && offset < n; offset += chunk) {
            XChangeProperty(a->display, e->requestor, property, e->target, 8,
                offset ? PropModeAppend : PropModeReplace, (const unsigned char *)a->clipboard + offset,
                (int)fmin(chunk, n - offset));
        }
        if (chunk) result = property;
    }
    XEvent reply = {.xselection = {.type = SelectionNotify, .requestor = e->requestor,
        .selection = e->selection, .target = e->target, .property = result, .time = e->time}};
    XSendEvent(a->display, e->requestor, False, NoEventMask, &reply);
    XFlush(a->display);
}

static void send_drag(App *a, Atom type, long a1, long a2, long a3, long a4) {
    if (!a->drag_source) return;
    XEvent e = {.xclient = {.type = ClientMessage, .window = a->drag_source,
        .message_type = type, .format = 32, .data.l = {a->window, a1, a2, a3, a4}}};
    XSendEvent(a->display, a->drag_source, False, NoEventMask, &e);
    XFlush(a->display);
}

static char *uri_path(char *line) {
    if (strncmp(line, "file://", 7)) return NULL;
    line += 7;
    if (!strncmp(line, "localhost/", 10)) line += 9;
    if (*line != '/') return NULL;
    for (char *out = line, *in = line; ; ++in, ++out) {
        if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[] = {in[1], in[2], 0};
            int decoded = strtol(hex, NULL, 16);
            if (!decoded) return NULL;
            *out = decoded;
            in += 2;
        } else {
            *out = *in;
            if (!*in) break;
        }
    }
    return line;
}

static void selection_received(App *a, XSelectionEvent *e) {
    bool drop = e->selection == a->xdnd_selection;
    bool ok = false;
    if (e->property != None) {
        Atom type;
        int format;
        unsigned long n, tail;
        unsigned char *data = NULL;
        if (XGetWindowProperty(a->display, a->window, e->property, 0, 16384, True, AnyPropertyType,
                               &type, &format, &n, &tail, &data) == Success && data && format == 8) {
            if (drop) {
                char *ctx = NULL;
                for (char *line = strtok_r((char *)data, "\r\n", &ctx); line; line = strtok_r(NULL, "\r\n", &ctx)) {
                    char *path = uri_path(line);
                    if (!path) continue;
                    char error[256];
                    ok = app_open(a, path, NULL, error);
                    if (!ok && a->prompt != PROMPT_PASSWORD) app_toast(a, error);
                    break;
                }
            } else if (a->prompt != PROMPT_NONE && a->prompt != PROMPT_HELP) {
                if (type == XA_STRING) {
                    char utf8[4096];
                    size_t length = 0;
                    for (size_t i = 0; i < n && length < sizeof utf8 - 2; ++i)
                        length += utf8_encode(utf8 + length, data[i]);
                    insert(a, utf8, length);
                } else insert(a, (const char *)data, n);
            }
        }
        if (data) XFree(data);
    } else if (!drop && e->target == a->utf8_atom) {
        XConvertSelection(a->display, e->selection, XA_STRING, a->paste_atom, a->window, e->time);
    }
    if (drop) {
        send_drag(a, a->xdnd_finished, ok, ok ? a->xdnd_action_copy : None, 0, 0);
        a->drag_source = None;
    }
}

static void client_message(App *a, XClientMessageEvent *e) {
    if ((Atom)e->data.l[0] == a->wm_delete &&
        e->message_type == XInternAtom(a->display, "WM_PROTOCOLS", False)) {
        a->running = false;
    } else if (e->message_type == a->xdnd_enter) {
        a->drag_source = e->data.l[0];
        a->drag_uris = false;
        if (e->data.l[1] & 1) {
            a->drag_uris = has_atom(a, a->drag_source, XInternAtom(a->display, "XdndTypeList", False),
                                    a->uri_atom);
        } else {
            for (int i = 2; i <= 4; ++i) a->drag_uris |= (Atom)e->data.l[i] == a->uri_atom;
        }
    } else if ((Window)e->data.l[0] == a->drag_source) {
        if (e->message_type == a->xdnd_position) {
            send_drag(a, a->xdnd_status, a->drag_uris ? 3 : 2, 0, 0,
                       a->drag_uris ? a->xdnd_action_copy : None);
        } else if (e->message_type == a->xdnd_drop) {
            if (a->drag_uris) {
                XConvertSelection(a->display, a->xdnd_selection, a->uri_atom, a->paste_atom, a->window,
                                  (Time)e->data.l[2]);
            } else {
                send_drag(a, a->xdnd_finished, 0, None, 0, 0);
                a->drag_source = None;
            }
        } else if (e->message_type == a->xdnd_leave) a->drag_source = None;
    }
}

static void queue_pointer(App *a, cui_event_type type, int x, int y, unsigned button) {
    cui_queue_event(a->ui, (cui_event){.type = type, .x = x, .y = y,
        .pointer_button = button == Button3 ? CUI_POINTER_SECONDARY :
                          button == Button2 ? CUI_POINTER_MIDDLE : CUI_POINTER_PRIMARY});
    a->dirty = true;
}

static void queue_key(App *a, cui_event_type type, cui_key key, unsigned mask) {
    unsigned mods = (mask & ShiftMask ? CUI_MOD_SHIFT : 0) |
                    (mask & ControlMask ? CUI_MOD_CTRL : 0) |
                    (mask & Mod1Mask ? CUI_MOD_ALT : 0) |
                    (mask & Mod4Mask ? CUI_MOD_META : 0);
    cui_queue_event(a->ui, (cui_event){.type = type, .key = key, .modifiers = mods});
    a->dirty = true;
}

static void handle_key(App *a, XKeyEvent *event, bool press) {
    KeySym actual = NoSymbol;
    char input[128];
    int n = 0;
    if (press && a->ic && a->prompt != PROMPT_NONE) {
        Status status;
        n = Xutf8LookupString(a->ic, event, input, sizeof input, &actual, &status);
        if (status == XBufferOverflow) n = 0;
    } else n = XLookupString(event, input, sizeof input, &actual, NULL);
    KeySym plain = XkbKeycodeToKeysym(a->display, event->keycode, 0, 0);
    if (actual == NoSymbol) actual = plain;
    unsigned mods = event->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
    if (plain == XK_Control_L || plain == XK_Control_R) {
        a->ctrl = press;
        if (!press) {
            char keys[32];
            XQueryKeymap(a->display, keys);
            KeyCode other = XKeysymToKeycode(a->display, plain == XK_Control_L ? XK_Control_R : XK_Control_L);
            a->ctrl = other && (keys[other / 8] & (1 << (other % 8)));
        }
        a->show_cursor = true;
        a->cursor_deadline = now() + 1.6;
    } else a->ctrl = mods & ControlMask;
    bool ctrl = mods & ControlMask;
    bool alt = mods & (Mod1Mask | Mod4Mask);
    if (a->prompt != PROMPT_NONE && press) {
        if (actual == XK_Escape) { app_command(a, UI_DISMISS); return; }
        if (a->prompt == PROMPT_PRINT) {
            if ((actual == XK_Return || actual == XK_KP_Enter) &&
                (a->print.editable || !cui_focused(a->ui))) {
                app_submit(a);
                return;
            }
            if (actual == XK_Tab || actual == XK_ISO_Left_Tab) a->print.editable = false;
        }
        if (a->prompt == PROMPT_HELP) {
            if (actual == XK_F1) { app_command(a, UI_DISMISS); return; }
        } else if (a->prompt != PROMPT_PRINT || (a->print.editable && a->print.job.fd < 0)) {
            size_t length = strlen(a->input);
            if (actual == XK_Return || actual == XK_KP_Enter) app_submit(a);
            else if (ctrl && (plain == XK_v || plain == XK_V))
                XConvertSelection(a->display, a->clipboard_atom, a->utf8_atom, a->paste_atom,
                                  a->window, event->time);
            else if (ctrl && (plain == XK_a || plain == XK_A)) a->select_input = true;
            else if (ctrl && (plain == XK_u || plain == XK_U)) {
                a->input[0] = 0; a->caret = 0; a->select_input = false;
            } else if (actual == XK_Home || actual == XK_End || actual == XK_Left || actual == XK_Right) {
                a->caret = actual == XK_Home ? 0 : actual == XK_End ? length :
                    actual == XK_Left ? previous_char(a->input, a->caret) : next_char(a->input, a->caret);
                a->select_input = false;
            } else if (actual == XK_BackSpace || actual == XK_Delete) {
                size_t from = actual == XK_BackSpace ? previous_char(a->input, a->caret) : a->caret;
                size_t to = actual == XK_BackSpace ? a->caret : next_char(a->input, a->caret);
                if (a->select_input) { from = 0; to = length; a->select_input = false; }
                memmove(a->input + from, a->input + to, length - to + 1);
                a->caret = from;
            } else if (!ctrl && !alt && n > 0) insert(a, input, n);
            a->dirty = true;
            return;
        }
    }
    if (press && a->prompt == PROMPT_NONE) {
        if (actual == XK_Escape) {
            if (a->presenting) app_present(a, false);
            else if (a->fullscreen) app_set_fullscreen(a, false);
            else if (*a->doc.query) app_command(a, UI_DISMISS);
            else if (a->doc.anchor.page >= 0) {
                a->doc.anchor.page = a->doc.end.page = -1;
                a->dirty = true;
            }
            return;
        }
        if (a->presenting || actual != XK_space || !cui_focused(a->ui)) {
            if (a->presenting && (actual == XK_Return || actual == XK_KP_Enter)) {
                app_action(a, NEXT);
                return;
            }
            for (int i = 0; i < ACTIONS; ++i) {
                for (int k = 0; k < 4 && a->config.keys[i][k].symbol; ++k) {
                    KeySym wanted = a->config.keys[i][k].symbol;
                    unsigned expected = a->config.keys[i][k].mods;
                    if ((expected == mods && (wanted == plain || wanted == actual)) ||
                        (!(expected & ShiftMask) && wanted == actual && wanted != plain &&
                         (mods & ~ShiftMask) == expected)) {
                        app_action(a, i);
                        return;
                    }
                }
            }
        }
    }
    cui_key key = actual == XK_Tab || actual == XK_ISO_Left_Tab ? CUI_KEY_TAB :
                  actual == XK_Return || actual == XK_KP_Enter ? CUI_KEY_ENTER :
                  actual == XK_space ? CUI_KEY_SPACE : CUI_KEY_NONE;
    if (key != CUI_KEY_NONE)
        queue_key(a, press ? CUI_EVENT_KEY_DOWN : CUI_EVENT_KEY_UP, key, mods);
}

static TextPoint pick_text(App *a, int x, int y) {
    TextPoint result = {-1, 0};
    Document *d = &a->doc;
    int index = app_page_at(a, y);
    cui_rect p = app_page_rect(a, index);
    if (!point(p, x, y)) return result;
    if (a->drag_page_index != index) {
        Drag mode = a->drag;
        app_close_drag(a);
        a->drag = mode;
        a->drag_text = document_text(d, index, &a->drag_page);
        a->drag_page_index = index;
    }
    if (!a->drag_text) return result;
    double px, py;
    if (!FPDF_DeviceToPage(a->drag_page, 0, 0, roundf(p.w), roundf(p.h), 0,
                           roundf(x - p.x), roundf(y - p.y), &px, &py)) return result;
    double tolerance = fmax(5, 7 / a->layout.scale);
    int ch = FPDFText_GetCharIndexAtPos(a->drag_text, px, py, tolerance, tolerance);
    if (ch >= 0) result = (TextPoint){index, ch};
    return result;
}

static bool in_findbar(App *a, int x, int y) {
    cui_rect c = a->layout.canvas;
    return a->doc.pdf && *a->doc.query && !a->fullscreen && c.w >= 234 &&
           y >= c.y + 10 && y < c.y + 53 && x >= c.x + c.w - fmin(c.w - 24, 428) - 12 &&
           x < c.x + c.w - 12;
}

static void press_button(App *a, XButtonEvent *e) {
    int x = e->x, y = e->y;
    bool middle = e->button == Button2;
    if (a->presenting && a->prompt == PROMPT_NONE) {
        if (e->button == Button1 || e->button == Button5) app_action(a, NEXT);
        if (e->button == Button3 || e->button == Button4) app_action(a, PREV);
        return;
    }
    if (e->button >= Button4 && e->button <= 7) {
        if (a->prompt == PROMPT_PRINT) {
            int delta = e->button == Button4 || e->button == 6 ? -70 : 70;
            a->print.scroll = fmax(0, fmin(a->print.scroll_end, a->print.scroll + delta));
            a->dirty = true;
        }
        if (a->prompt != PROMPT_NONE) return;
        int dir = e->button == Button4 || e->button == 6 ? -1 : 1;
        if ((e->state & ControlMask) && point(a->layout.canvas, x, y) && !in_findbar(a, x, y))
            app_action(a, dir < 0 ? ZOOM_IN : ZOOM_OUT);
        else if (point(a->layout.info, x, y)) {
            a->info_scroll = (int)fmax(0, fmin(a->info_end, a->info_scroll + dir * a->config.scroll_step));
            a->dirty = true;
        }
        else if (point(a->layout.strip, x, y)) {
            a->thumbnail_shift = (int)fmax(-a->doc.count, fmin(a->doc.count, a->thumbnail_shift + dir));
            a->dirty = true;
        } else if (point(a->layout.sidebar, x, y)) {
            a->sidebar_shift = (int)fmax(-a->doc.count, fmin(a->doc.count, a->sidebar_shift + dir));
            a->dirty = true;
        } else if ((e->state & ShiftMask) || e->button >= 6) {
            a->pan -= dir * a->config.scroll_step;
            a->dirty = true;
        } else if (a->doc.pdf || (a->doc.picture.pixels &&
                   a->doc.picture.h * a->layout.scale > a->layout.canvas.h)) {
            app_scroll(a, dir * a->config.scroll_step);
        } else if (a->doc.picture.pixels) {
            app_action(a, dir < 0 ? PREV : NEXT);
        }
        return;
    }
    if (a->prompt == PROMPT_NONE && e->button == Button1 &&
        (point(a->layout.strip_grip, x, y) || point(a->layout.sidebar_grip, x, y))) {
        bool strip = point(a->layout.strip_grip, x, y);
        app_close_drag(a);
        a->drag = strip ? DRAG_STRIP : DRAG_SIDEBAR;
        a->drag_button = Button1;
        a->resize_origin = strip ? y : x;
        a->resize_start = strip ? a->layout.strip.h - 42 : a->layout.sidebar.w;
        a->dirty = true;
        return;
    }
    if (middle && a->prompt != PROMPT_NONE && a->prompt != PROMPT_HELP) {
        XConvertSelection(a->display, XA_PRIMARY, a->utf8_atom, a->paste_atom, a->window, e->time);
        return;
    }
    if (a->prompt != PROMPT_NONE || !point(a->layout.canvas, x, y) || in_findbar(a, x, y) ||
        !a->doc.path || (e->button != Button1 && !middle)) {
        queue_pointer(a, CUI_EVENT_POINTER_DOWN, x, y, e->button);
        return;
    }
    app_close_drag(a);
    bool double_click = !middle && a->last_click && e->time - a->last_click < 290 &&
                        abs(x - a->last_click_x) < 7 && abs(y - a->last_click_y) < 7;
    a->last_click = double_click ? 0 : e->time;
    a->last_click_x = x; a->last_click_y = y;
    if (double_click && !a->doc.pdf) { app_action(a, FULLSCREEN); return; }
    if (a->doc.pdf && !middle && !(e->state & Mod1Mask)) {
        TextPoint hit = pick_text(a, x, y);
        a->doc.anchor.page = a->doc.end.page = -1;
        if (hit.page >= 0) {
            a->doc.anchor = a->doc.end = hit;
            a->drag = DRAG_TEXT;
        } else {
            app_close_drag(a);
            a->drag = DRAG_PAN;
        }
    } else a->drag = DRAG_PAN;
    a->drag_button = e->button;
    a->dirty = true;
}

static void motion(App *a, int x, int y) {
    if (a->drag == DRAG_PAN) {
        a->pan += x - a->mx;
        a->scroll -= y - a->my;
        app_layout(a);
        a->dirty = true;
    } else if (a->drag == DRAG_TEXT) {
        unsigned button = a->drag_button;
        TextPoint hit = pick_text(a, x, y);
        a->drag_button = button;
        if (hit.page >= 0) a->doc.end = hit;
        a->dirty = true;
    } else if (a->drag == DRAG_STRIP) {
        a->thumb_size = fmax(40, fmin(360, a->resize_start + a->resize_origin - y));
        app_layout(a);
    } else if (a->drag == DRAG_SIDEBAR) {
        int page = a->doc.current;
        a->sidebar_width = fmax(100, fmin(460, a->resize_start + x - a->resize_origin));
        app_layout(a);
        app_jump(a, page);
    }
    a->mx = x; a->my = y;
    a->cursor_deadline = now() + 1.6;
    x11_show_cursor(a, true);
    queue_pointer(a, CUI_EVENT_POINTER_MOVE, x, y, 0);
}

void x11_event(App *a, XEvent *e) {
    if (XFilterEvent(e, None)) return;
    switch (e->type) {
    case Expose: a->dirty = true; break;
    case ConfigureNotify:
        if (a->w != e->xconfigure.width || a->h != e->xconfigure.height) {
            int page = a->doc.current;
            int width = a->w;
            a->w = e->xconfigure.width;
            a->h = e->xconfigure.height;
            app_layout(a);
            if (a->doc.pdf && width != a->w) app_jump(a, page);
            a->dirty = true;
        }
        break;
    case PropertyNotify:
        if (e->xproperty.window == DefaultRootWindow(a->display) &&
            e->xproperty.atom == XA_RESOURCE_MANAGER && a->config.xresources) {
            Config updated;
            config_load(&updated, a->display);
            memcpy(a->config.colors, updated.colors, sizeof updated.colors);
            window_icon(a);
            a->dirty = true;
        }
        if (e->xproperty.window == a->window && !a->fallback_fullscreen &&
            e->xproperty.atom == a->state_atom) {
            bool full = has_atom(a, a->window, a->state_atom, a->fullscreen_atom);
            if (a->presenting && a->fullscreen && !full) {
                a->fullscreen_requested = a->fullscreen = false;
                a->presentation_was_fullscreen = false;
                app_present(a, false);
            } else if (!a->presenting || full) a->fullscreen_requested = a->fullscreen = full;
            a->cursor_deadline = now() + 1.6;
            if (!full) x11_show_cursor(a, true);
            a->dirty = true;
        }
        break;
    case KeyPress: handle_key(a, &e->xkey, true); x11_cursor(a); break;
    case KeyRelease: handle_key(a, &e->xkey, false); x11_cursor(a); break;
    case ButtonPress:
        a->mx = e->xbutton.x; a->my = e->xbutton.y;
        a->ctrl = e->xbutton.state & ControlMask;
        a->cursor_deadline = now() + 1.6;
        a->show_cursor = true;
        press_button(a, &e->xbutton);
        x11_cursor(a);
        break;
    case ButtonRelease:
        if (a->drag != DRAG_NONE && e->xbutton.button == a->drag_button)
            app_close_drag(a);
        else if (!a->presenting && e->xbutton.button && e->xbutton.button < Button4)
            queue_pointer(a, CUI_EVENT_POINTER_UP, e->xbutton.x, e->xbutton.y, e->xbutton.button);
        x11_cursor(a);
        break;
    case MotionNotify:
        a->ctrl = e->xmotion.state & ControlMask;
        motion(a, e->xmotion.x, e->xmotion.y);
        break;
    case EnterNotify:
        a->ctrl = e->xcrossing.state & ControlMask;
        a->mx = e->xcrossing.x; a->my = e->xcrossing.y;
        x11_cursor(a);
        queue_pointer(a, CUI_EVENT_POINTER_ENTER, a->mx, a->my, 0);
        break;
    case LeaveNotify:
        if (a->drag == DRAG_NONE) a->mx = a->my = -1;
        x11_cursor(a);
        queue_pointer(a, CUI_EVENT_POINTER_LEAVE, 0, 0, 0);
        break;
    case FocusIn: {
        if (a->ic) XSetICFocus(a->ic);
        Window root, child;
        int root_x, root_y;
        unsigned mask = 0;
        if (XQueryPointer(a->display, a->window, &root, &child, &root_x, &root_y, &a->mx, &a->my, &mask))
            a->ctrl = mask & ControlMask;
        x11_cursor(a);
        break;
    }
    case FocusOut:
        if (a->ic) XUnsetICFocus(a->ic);
        a->ctrl = false;
        app_close_drag(a);
        x11_cursor(a);
        queue_pointer(a, CUI_EVENT_CANCEL, 0, 0, 0);
        break;
    case DestroyNotify: a->window = None; a->running = false; break;
    case ClientMessage: client_message(a, &e->xclient); break;
    case SelectionRequest: selection_request(a, &e->xselectionrequest); break;
    case SelectionNotify: selection_received(a, &e->xselection); break;
    case MappingNotify: XRefreshKeyboardMapping(&e->xmapping); break;
    }
}
