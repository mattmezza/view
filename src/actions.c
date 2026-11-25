#include "app.h"
#include <X11/Xatom.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void app_toast(App *a, const char *message) {
    snprintf(a->toast, sizeof a->toast, "%s", message);
    a->toast_until = now() + 2.6;
    a->dirty = true;
}

void app_close_drag(App *a) {
    if (a->drag_text) FPDFText_ClosePage(a->drag_text);
    if (a->drag_page) FPDF_ClosePage(a->drag_page);
    a->drag_text = NULL;
    a->drag_page = NULL;
    a->drag_page_index = -1;
    a->drag = DRAG_NONE;
    a->drag_button = 0;
}

static void window_title(App *a) {
    char title[4096];
    snprintf(title, sizeof title, "%s%s", a->doc.path ? base_name(a->doc.path) : "View",
             a->doc.path ? " — View" : "");
    Atom name = XInternAtom(a->display, "_NET_WM_NAME", False);
    XChangeProperty(a->display, a->window, name, a->utf8_atom, 8, PropModeReplace,
                    (const unsigned char *)title, strlen(title));
    XStoreName(a->display, a->window, title);
}

bool app_open(App *a, const char *path, const char *password, char error[256]) {
    char *full = absolute_path(path);
    if (!full) { snprintf(error, 256, "Invalid path"); return false; }
    app_close_drag(a);
    OpenResult result = document_open(&a->doc, full, password, error);
    if (result == DOC_PASSWORD) {
        bool retry = a->prompt == PROMPT_PASSWORD && !strcmp(a->password_path, full);
        snprintf(a->password_path, sizeof a->password_path, "%s", full);
        app_prompt(a, PROMPT_PASSWORD);
        if (retry) snprintf(a->prompt_error, sizeof a->prompt_error, "Incorrect password");
    }
    free(full);
    if (result != DOC_OK) return false;
    metadata_close(&a->metadata);
    if (a->presenting) app_present(a, false);
    a->scale_mode = a->doc.pdf ? SCALE_WIDTH : SCALE_FIT;
    a->zoom = 1;
    a->scroll = a->pan = 0;
    a->thumbnail_shift = a->sidebar_shift = a->info_scroll = 0;
    a->prompt = PROMPT_NONE;
    a->toast_until = 0;
    a->dirty = true;
    window_title(a);
    app_layout(a);
    return true;
}

void app_item(App *a, int index) {
    Document *d = &a->doc;
    if (index < 0 || index >= d->count) return;
    if (d->pdf) { app_jump(a, index); return; }
    if (index == d->current) return;
    char error[256] = "";
    if (document_image(d, index, error)) {
        metadata_close(&a->metadata);
        a->pan = a->scroll = 0;
        a->thumbnail_shift = 0;
        a->info_scroll = 0;
        window_title(a);
        a->dirty = true;
    } else if (*error) app_toast(a, error);
}

static void navigate(App *a, int dir) {
    if (!a->doc.count) return;
    int index = a->doc.current + dir;
    if (a->presenting && !a->doc.pdf) {
        int current = a->doc.current;
        while (index >= 0 && index < a->doc.count) {
            app_item(a, index);
            if (a->doc.current != current) break;
            index += dir;
        }
        return;
    }
    if (!a->doc.pdf && a->config.wrap) index = (index + a->doc.count) % a->doc.count;
    app_item(a, index);
}

void app_set_clipboard(App *a, char *text) {
    if (!text || !*text) { free(text); return; }
    free(a->clipboard);
    a->clipboard = text;
    XSetSelectionOwner(a->display, a->clipboard_atom, a->window, CurrentTime);
    XSetSelectionOwner(a->display, XA_PRIMARY, a->window, CurrentTime);
}

void app_set_fullscreen(App *a, bool on) {
    if (!on && a->presenting) app_present(a, false);
    if (a->fullscreen_requested == on) return;
    a->fullscreen_requested = on;
    a->fullscreen = on;
    Window root = DefaultRootWindow(a->display);
    if (a->fallback_fullscreen) {
        if (on) {
            Window child;
            XTranslateCoordinates(a->display, a->window, root, 0, 0, &a->restore_x, &a->restore_y, &child);
            a->restore_w = a->w; a->restore_h = a->h;
            XWindowAttributes attr;
            XGetWindowAttributes(a->display, root, &attr);
            XRaiseWindow(a->display, a->window);
            XMoveResizeWindow(a->display, a->window, 0, 0, attr.width, attr.height);
        } else {
            XMoveResizeWindow(a->display, a->window, a->restore_x, a->restore_y,
                              a->restore_w, a->restore_h);
        }
    } else {
        XEvent e = {.xclient = {.type = ClientMessage, .window = a->window,
            .message_type = a->state_atom, .format = 32,
            .data.l = {on ? 1 : 0, a->fullscreen_atom, 0, 1, 0}}};
        XSendEvent(a->display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    }
    a->cursor_deadline = now() + 2;
    a->dirty = true;
}

void app_present(App *a, bool on) {
    if (a->presenting == on || (on && !a->doc.path)) return;
    app_close_drag(a);
    a->last_click = 0;
    if (on) {
        a->before_presentation.mode = a->scale_mode;
        a->before_presentation.zoom = a->zoom;
        a->before_presentation.pan = a->pan;
        a->before_presentation.scroll = a->scroll;
        a->before_presentation.page = a->doc.current;
        a->presentation_was_fullscreen = a->fullscreen_requested;
        a->presenting = true;
        a->pan = a->scroll = 0;
        app_prompt(a, PROMPT_NONE);
        app_set_fullscreen(a, true);
    } else {
        int page = a->doc.current;
        a->presenting = false;
        a->scale_mode = a->before_presentation.mode;
        a->zoom = a->before_presentation.zoom;
        a->pan = a->before_presentation.pan;
        a->scroll = a->before_presentation.scroll;
        app_set_fullscreen(a, a->presentation_was_fullscreen);
        if (a->doc.pdf && page != a->before_presentation.page) {
            app_layout(a);
            app_jump(a, page);
        }
    }
    a->dirty = true;
}

void app_zoom(App *a, ScaleMode mode, double value) {
    if (!a->doc.path || a->presenting) return;
    app_layout(a);
    double old = a->layout.scale;
    int page = a->doc.current;
    double sample_y = a->layout.canvas.y + a->layout.canvas.h * .4;
    if (a->doc.pdf) page = app_page_at(a, sample_y);
    double spot = a->doc.pdf ? (sample_y - app_page_rect(a, page).y) / old : 0;
    double x = a->pan, y = a->scroll;
    if (mode == SCALE_MANUAL) {
        a->scale_mode = a->doc.pdf ? SCALE_WIDTH : SCALE_FIT;
        app_layout(a);
        a->zoom = fmax(.05, fmin(32, value / a->layout.scale));
    }
    a->scale_mode = mode;
    app_layout(a);
    double scale = a->layout.scale;
    a->pan = x * scale / old;
    if (a->doc.pdf) {
        double doc_y = app_page_rect(a, page).y + a->scroll;
        a->scroll = doc_y + spot * scale - sample_y;
    } else {
        a->scroll = y * scale / old;
    }
    app_layout(a);
    a->dirty = true;
}

void app_find_match(App *a, int dir) {
    Document *d = &a->doc;
    if (!d->num_matches) return;
    int n = (int)d->num_matches;
    if (d->active_match < 0) d->active_match = dir < 0 ? n - 1 : 0;
    else d->active_match = (d->active_match + dir + n) % n;
    Match m = d->matches[d->active_match];
    if (a->presenting) { app_jump(a, m.page); return; }
    FPDF_PAGE page;
    FPDF_TEXTPAGE text = document_text(d, m.page, &page);
    bool positioned = false;
    if (text) {
        if (FPDFText_CountRects(text, m.start, m.count) > 0) {
            double l, t, r, b;
            int x, y;
            cui_rect bounds = app_page_rect(a, m.page);
            if (FPDFText_GetRect(text, 0, &l, &t, &r, &b) &&
                FPDF_PageToDevice(page, 0, 0, round(bounds.w), round(bounds.h), 0, l, t, &x, &y)) {
                double offset = bounds.y + y - a->layout.canvas.y;
                if (offset < 45 || offset > a->layout.canvas.h - 45) {
                    a->scroll += offset - a->layout.canvas.h * .33;
                    a->sidebar_shift = 0;
                    app_layout(a);
                }
                positioned = true;
            }
        }
        FPDFText_ClosePage(text);
    }
    if (page) FPDF_ClosePage(page);
    if (!positioned) app_jump(a, m.page);
    a->dirty = true;
}

void app_scroll(App *a, double px) {
    if (a->presenting) return;
    a->scroll += px;
    a->dirty = true;
}

void app_prompt(App *a, Prompt prompt) {
    app_close_drag(a);
    if (a->prompt == PROMPT_PRINT && prompt != PROMPT_PRINT) job_close(&a->print.printers, true);
    a->prompt = prompt;
    a->prompt_error[0] = 0;
    a->input[0] = 0;
    a->select_input = false;
    if (prompt == PROMPT_FIND) {
        snprintf(a->input, sizeof a->input, "%s", a->doc.query);
        a->select_input = *a->input != 0;
    } else if (prompt == PROMPT_OPEN && a->doc.path) {
        const char *slash = strrchr(a->doc.path, '/');
        if (slash && (size_t)(slash - a->doc.path) < sizeof a->input - 1)
            snprintf(a->input, sizeof a->input, "%.*s", (int)(slash - a->doc.path + 1), a->doc.path);
        a->select_input = *a->input != 0;
    }
    a->caret = strlen(a->input);
    a->dirty = true;
}

void app_submit(App *a) {
    if (a->prompt == PROMPT_PRINT) {
        print_submit(a);
    } else if (a->prompt == PROMPT_FIND) {
        document_search(&a->doc, a->input);
        app_prompt(a, PROMPT_NONE);
    } else if (a->prompt == PROMPT_OPEN || a->prompt == PROMPT_PASSWORD) {
        bool unlock = a->prompt == PROMPT_PASSWORD;
        char error[256];
        if (!app_open(a, unlock ? a->password_path : a->input, unlock ? a->input : NULL, error) &&
            a->prompt != PROMPT_PASSWORD)
            snprintf(a->prompt_error, sizeof a->prompt_error, "%s", error);
        if (unlock) {
            explicit_bzero(a->input, sizeof a->input);
            a->caret = 0;
        }
        a->dirty = true;
    }
}

void app_action(App *a, Action action) {
    Document *d = &a->doc;
    switch (action) {
    case OPEN: app_prompt(a, PROMPT_OPEN); break;
    case PREV: navigate(a, -1); break;
    case NEXT: navigate(a, 1); break;
    case FIRST: app_item(a, 0); break;
    case LAST: app_item(a, d->count - 1); break;
    case ZOOM_IN: app_zoom(a, SCALE_MANUAL, a->layout.scale * a->config.zoom_step); break;
    case ZOOM_OUT: app_zoom(a, SCALE_MANUAL, a->layout.scale / a->config.zoom_step); break;
    case FIT: app_zoom(a, SCALE_FIT, 1); break;
    case ACTUAL: app_zoom(a, SCALE_ACTUAL, 1); break;
    case FULLSCREEN:
        if (a->presenting) app_present(a, false);
        else app_set_fullscreen(a, !a->fullscreen_requested);
        break;
    case GALLERY: a->gallery = !a->gallery; break;
    case SIDEBAR:
        if (d->pdf) { int page = d->current; a->sidebar = !a->sidebar; app_layout(a); app_jump(a, page); }
        break;
    case FIND: if (d->pdf) app_prompt(a, PROMPT_FIND); break;
    case FIND_NEXT: app_find_match(a, 1); break;
    case FIND_PREV: app_find_match(a, -1); break;
    case COPY:
        if (d->pdf) {
            char *text = document_selection(d);
            if (text && *text) { app_set_clipboard(a, text); app_toast(a, "Copied to clipboard"); }
            else free(text);
        }
        break;
    case UP:
        if (a->presenting) navigate(a, -1); else app_scroll(a, -a->config.scroll_step);
        break;
    case DOWN:
        if (a->presenting) navigate(a, 1); else app_scroll(a, a->config.scroll_step);
        break;
    case PAGE_UP: if (d->pdf && !a->presenting) app_scroll(a, -a->layout.canvas.h * .87); else navigate(a, -1); break;
    case PAGE_DOWN: if (d->pdf && !a->presenting) app_scroll(a, a->layout.canvas.h * .87); else navigate(a, 1); break;
    case RELOAD: {
        bool old_updates = a->config.check_updates;
        config_load(&a->config, a->display);
        renderer_fonts(&a->renderer, &a->config);
        cui_text_metrics_changed(a->ui);
        a->gallery = a->config.gallery;
        a->sidebar = a->config.sidebar;
        a->info = a->config.info;
        a->thumb_size = a->config.thumb_size;
        a->sidebar_width = a->config.sidebar_width;
        if (!a->config.check_updates) {
            *a->update = 0;
            update_close(a);
        } else if (!old_updates) {
            a->update_at = now();
        }
        app_toast(a, "Settings reloaded");
        break;
    }
    case HELP: app_prompt(a, PROMPT_HELP); break;
    case QUIT: a->running = false; break;
    case INFO: {
        int page = d->current;
        a->info = !a->info;
        app_layout(a);
        if (d->pdf) app_jump(a, page);
        break;
    }
    case PRESENT: app_present(a, !a->presenting); break;
    case PRINT: if (d->path) print_open(a); break;
    case ACTIONS: break;
    }
    a->dirty = true;
}

void app_command(App *a, int command) {
    if (command >= UI_PRINT_STEP && command < UI_PRINT_STEP + PRINT_FIELDS * 2) {
        int step = command - UI_PRINT_STEP;
        print_step(a, step / 2, step % 2 ? 1 : -1);
        return;
    }
    switch (command) {
    case UI_SUBMIT: app_submit(a); break;
    case UI_DISMISS:
        if (a->prompt != PROMPT_NONE) {
            bool password = a->prompt == PROMPT_PASSWORD;
            app_prompt(a, PROMPT_NONE);
            if (password) explicit_bzero(a->input, sizeof a->input);
        } else if (a->doc.pdf) document_search(&a->doc, "");
        a->dirty = true;
        break;
    case UI_UPDATE:
        if (*a->update) {
            char url[160];
            snprintf(url, sizeof url, "https://github.com/mattmezza/view/releases/tag/%s", a->update);
            if (fork() == 0) { execlp("xdg-open", "xdg-open", url, (char *)NULL); _exit(127); }
        }
        break;
    case UI_FIT_TOGGLE:
        app_zoom(a, a->scale_mode == SCALE_FIT && a->doc.pdf ? SCALE_WIDTH : SCALE_FIT, 1);
        break;
    case UI_PRINT_RANGE: a->print.editable = a->dirty = true; break;
    default: break;
    }
}
