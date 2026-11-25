#include "app.h"
#include <X11/Xatom.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_signal(int signal_number) { (void)signal_number; stop = 1; }

static void help(FILE *out) {
    fputs("View " VIEW_VERSION "\n"
          "Usage: view [--] [FILE|DIRECTORY]\n"
          "       view open FILE|DIRECTORY\n"
          "       view status | raise | quit | reload\n"
          "       view prev | next | first | last | goto NUMBER\n"
          "       view zoom PERCENT | zoom-in | zoom-out | fit | width | actual\n"
          "       view fullscreen|gallery|sidebar [on|off|toggle]\n"
          "       view find TEXT | find-next | find-prev | clear-search | copy\n"
          "       view up | down | page-up | page-down | scroll PIXELS\n"
          "       view --socket | --version | --help\n", out);
}

static bool parse_number(const char *s, double *out) {
    char *end;
    errno = 0;
    double value = strtod(s, &end);
    if (end == s || *end || errno || !isfinite(value)) return false;
    *out = value;
    return true;
}

static bool parse_toggle(const char *s, bool before, bool *out) {
    if (!strcmp(s, "toggle")) *out = !before;
    else if (!strcmp(s, "on")) *out = true;
    else if (!strcmp(s, "off")) *out = false;
    else return false;
    return true;
}

static void json_string(const char *s, char *out, size_t capacity) {
    if (!s) { snprintf(out, capacity, "null"); return; }
    size_t pos = 0;
    out[pos++] = '"';
    for (const char *end = s + strlen(s); s < end && pos + 8 < capacity; ) {
        uint32_t cp = utf8_next(&s, end);
        if (cp < 32) pos += snprintf(out + pos, capacity - pos, "\\u%04x", cp);
        else if (cp == '"' || cp == '\\') { out[pos++] = '\\'; out[pos++] = cp; }
        else pos += utf8_encode(out + pos, cp);
    }
    out[pos++] = '"';
    out[pos] = 0;
}

static void status(App *a, char reply[IPC_MAX]) {
    Document *d = &a->doc;
    char path[IPC_MAX - 600], query[3072];
    json_string(d->path, path, sizeof path);
    json_string(d->query, query, sizeof query);
    int n = snprintf(reply, IPC_MAX,
        "{\"version\":\"%s\",\"path\":%s,\"kind\":\"%s\",\"index\":%d,\"count\":%d,"
        "\"zoom\":%.2f,\"fullscreen\":%s,\"gallery\":%s,\"sidebar\":%s,"
        "\"query\":", VIEW_VERSION, path, d->pdf ? "pdf" : d->picture.pixels ? "image" : "empty",
        d->count ? d->current + 1 : 0, d->count, a->layout.scale * 100,
        a->fullscreen ? "true" : "false", a->gallery ? "true" : "false", a->sidebar ? "true" : "false");
    size_t remaining = IPC_MAX - n;
    size_t len = strlen(query);
    if (len + 140 >= remaining) strcpy(query, "\"\"");
    snprintf(reply + n, remaining,
        "%s,\"matches\":%zu,\"active_match\":%d,\"searching\":%s,\"update\":\"%s\"}",
        query, d->num_matches, d->active_match + 1, d->searching ? "true" : "false", a->update);
}

static Action action_for(const char *cmd) {
    for (int i = 0; i < ACTIONS; ++i)
        if (!strcmp(action_names[i], cmd)) return i;
    return ACTIONS;
}

static bool command(App *a, int argc, const char *const *argv, char reply[IPC_MAX]) {
    Document *d = &a->doc;
    if (!argc) { strcpy(reply, "Missing command"); return false; }
    const char *cmd = argv[0], *arg = argc > 1 ? argv[1] : "";
    Action action = action_for(cmd);
    double value;
    if (!strcmp(cmd, "status") && argc == 1) {
        app_layout(a);
        status(a, reply);
    } else if (!strcmp(cmd, "raise") && argc == 1) {
        x11_raise(a);
    } else if (action == OPEN && argc == 2) {
        x11_raise(a);
        return app_open(a, arg, NULL, reply);
    } else if (action == FIND && argc == 2) {
        if (!d->pdf) { strcpy(reply, "Open a PDF before searching"); return false; }
        if (strlen(arg) >= sizeof d->query) { strcpy(reply, "Search exceeds 511 bytes"); return false; }
        document_search(d, arg);
        if (a->prompt == PROMPT_FIND) app_prompt(a, PROMPT_NONE);
    } else if (!strcmp(cmd, "goto") && argc == 2 && parse_number(arg, &value) &&
               value >= 1 && value <= d->count && value == floor(value)) {
        int i = (int)value - 1;
        if (d->pdf) app_jump(a, i);
        else if (i != d->current) {
            app_item(a, i);
            if (d->current != i) { snprintf(reply, IPC_MAX, "%s", a->toast); return false; }
        }
    } else if (!strcmp(cmd, "zoom") && argc == 2 && parse_number(arg, &value) &&
               d->path && value >= 1 && value <= 3200) {
        app_zoom(a, SCALE_MANUAL, value / 100);
    } else if (!strcmp(cmd, "scroll") && argc == 2 && parse_number(arg, &value) &&
               fabs(value) <= 1000000000) {
        app_scroll(a, value);
    } else if (!strcmp(cmd, "width") && argc == 1) {
        app_zoom(a, SCALE_WIDTH, 1);
    } else if (!strcmp(cmd, "clear-search") && argc == 1) {
        if (d->pdf) document_search(d, "");
    } else if ((action == FULLSCREEN || action == GALLERY || action == SIDEBAR) && argc <= 2) {
        bool before = action == FULLSCREEN ? a->fullscreen_requested :
                      action == GALLERY ? a->gallery : a->sidebar;
        bool after;
        if (!parse_toggle(argc == 2 ? arg : "toggle", before, &after)) {
            strcpy(reply, "Expected on, off, or toggle");
            return false;
        }
        if (before != after) {
            if (action == SIDEBAR && !d->pdf) a->sidebar = after;
            else app_action(a, action);
        }
    } else if (action != ACTIONS && argc == 1) {
        const char *before = d->path;
        int index = d->current;
        double time = a->toast_until;
        app_action(a, action);
        if ((action == NEXT || action == PREV || action == FIRST || action == LAST) &&
            before && !d->pdf && d->current == index && a->toast_until > time) {
            snprintf(reply, IPC_MAX, "%s", a->toast);
            return false;
        }
    } else {
        snprintf(reply, IPC_MAX, "Invalid command or argument: %.180s", cmd);
        return false;
    }
    if (strcmp(cmd, "status")) a->dirty = true;
    return true;
}

static void receive(App *a) {
    char request[IPC_MAX], response[IPC_MAX] = "";
    size_t length;
    int fd = ipc_accept(&a->ipc, request, &length);
    if (fd < 0) return;
    const char *args[8];
    int n = 0;
    for (size_t i = 0; i < length; ) {
        if (n == (int)(sizeof args / sizeof *args)) {
            ipc_reply(fd, false, "Too many arguments");
            return;
        }
        args[n++] = request + i;
        i += strlen(request + i) + 1;
    }
    bool ok = command(a, n, args, response);
    ipc_reply(fd, ok, response);
}

static void interval(int *ms, double deadline, double time) {
    int wait = (int)fmin(2147000000, fmax(0, ceil((deadline - time) * 1000)));
    if (*ms < 0 || wait < *ms) *ms = wait;
}

static int run(Ipc ipc, const char *initial, int notify) {
    App a = {.ipc = ipc, .running = true, .update_fd = -1, .drag_page_index = -1,
        .doc = {.anchor.page = -1, .end.page = -1, .active_match = -1},
        .zoom = 1, .scale_mode = SCALE_FIT};
    struct sigaction handler = {.sa_handler = on_signal};
    sigemptyset(&handler.sa_mask);
    sigaction(SIGINT, &handler, NULL);
    sigaction(SIGTERM, &handler, NULL);
    if (!x11_init(&a)) {
        fprintf(stderr, "view: could not initialize the X11 window\n");
        if (notify >= 0) close(notify);
        x11_close(&a);
        ipc_close(&a.ipc);
        return 1;
    }
    if (notify >= 0) {
        if (write(notify, "1", 1) != 1) stop = 1;
        close(notify);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO) close(null);
        }
    }
    if (initial) {
        char error[256];
        if (!app_open(&a, initial, NULL, error) && a.prompt != PROMPT_PASSWORD)
            app_toast(&a, error);
    }
    a.update_at = now() + .8;
    while (a.running && !stop) {
        for (int i = 0; i < 192 && a.running && XPending(a.display); ++i) {
            XEvent event;
            XNextEvent(a.display, &event);
            x11_event(&a, &event);
        }
        if (!a.running || stop) break;
        double time = now();
        if (a.toast_until && a.toast_until <= time) { a.toast_until = 0; a.dirty = true; }
        if (a.fullscreen && a.show_cursor && a.prompt == PROMPT_NONE &&
            a.drag == DRAG_NONE && a.cursor_deadline <= time) x11_show_cursor(&a, false);
        if (!a.fullscreen || a.prompt != PROMPT_NONE) x11_show_cursor(&a, true);
        if (a.thumb_pending && a.next_thumbnail <= time) a.dirty = true;
        if (*a.hover_label && a.hover_since > 0 && time >= a.hover_since + .55 &&
            a.hover_since + .55 >= time - .15) a.dirty = true;
        if (a.config.check_updates && a.update_at <= time) update_begin(&a);
        if (a.doc.searching) {
            document_search_step(&a.doc);
            if (a.doc.active_match < 0 && a.doc.num_matches) app_find_match(&a, 1);
            a.dirty = true;
        }
        if (a.dirty) app_draw(&a);
        if (!a.running || stop) break;
        int timeout = a.dirty || a.doc.searching || XPending(a.display) ? 0 : -1;
        time = now();
        if (a.toast_until) interval(&timeout, a.toast_until, time);
        if (a.fullscreen && a.show_cursor && a.drag == DRAG_NONE && a.prompt == PROMPT_NONE)
            interval(&timeout, a.cursor_deadline, time);
        if (a.thumb_pending) interval(&timeout, a.next_thumbnail, time);
        if (*a.hover_label && a.hover_since + .55 > time)
            interval(&timeout, a.hover_since + .55, time);
        if (a.config.check_updates) interval(&timeout, a.update_at, time);
        struct pollfd descriptors[] = {
            {ConnectionNumber(a.display), POLLIN, 0}, {a.ipc.server, POLLIN, 0},
            {a.update_fd, POLLIN, 0}
        };
        int ready = poll(descriptors, sizeof descriptors / sizeof *descriptors, timeout);
        if (ready > 0) {
            if (descriptors[0].revents & (POLLERR | POLLHUP)) break;
            if (descriptors[1].revents & POLLIN) receive(&a);
            if (descriptors[2].revents & (POLLIN | POLLERR | POLLHUP)) update_read(&a);
        } else if (ready < 0 && errno != EINTR) break;
        if (!a.update_pid) while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
    update_close(&a);
    app_close_drag(&a);
    document_close(&a.doc);
    free(a.clipboard);
    x11_close(&a);
    ipc_close(&a.ipc);
    return 0;
}

extern const char view_icon[];

static int desktop_entry(const char *binary) {
    if (*binary != '/' || strchr(binary, '\n') || strchr(binary, '\r')) return 2;
    fputs("[Desktop Entry]\nType=Application\nName=View\nComment=View images and PDFs\nExec=\"", stdout);
    for (const char *s = binary; *s; ++s) {
        if (strchr("\\`$\"", *s)) fputs("\\\\", stdout);
        if (*s == '%') putchar('%');
        putchar(*s);
    }
    fputs("\" -- %f\nIcon=view\nTerminal=false\nCategories=Graphics;Viewer;\nStartupWMClass=View\nMimeType=", stdout);
    for (int i = 0; mime_types[i]; ++i) printf("%s;", mime_types[i]);
    putchar('\n');
    return 0;
}

int main(int argc, char **argv) {
    const char *first = argc > 1 ? argv[1] : "";
    if (argc == 2 && (!strcmp(first, "--help") || !strcmp(first, "-h"))) { help(stdout); return 0; }
    if (argc == 2 && (!strcmp(first, "--version") || !strcmp(first, "-v"))) { puts(VIEW_VERSION); return 0; }
    if (argc == 2 && !strcmp(first, "--mime-types")) {
        for (int i = 0; mime_types[i]; ++i) puts(mime_types[i]);
        return 0;
    }
    if (argc == 2 && !strcmp(first, "--icon")) { fputs(view_icon, stdout); return 0; }
    if (argc == 3 && !strcmp(first, "--desktop-entry")) return desktop_entry(argv[2]);

    bool explicit_open = !strcmp(first, "open") && argc > 2;
    bool inferred = argc == 2 && *first != '-' && action_for(first) == ACTIONS &&
        strcmp(first, "status") && strcmp(first, "raise") && strcmp(first, "width") &&
        strcmp(first, "clear-search");
    bool path_arg = inferred || (argc == 3 && !strcmp(first, "--")) || (explicit_open && argc == 3);
    bool graphical = argc == 1 || (path_arg && !explicit_open);
    if (!path_arg && !graphical && !strcmp(first, "--")) { help(stderr); return 2; }
    if (*first == '-' && strcmp(first, "--") && strcmp(first, "--socket")) { help(stderr); return 2; }

    Ipc ipc;
    if (!ipc_init(&ipc)) {
        fprintf(stderr, "view: DISPLAY and a private runtime directory are required\n");
        return 1;
    }
    if (argc == 2 && !strcmp(first, "--socket")) { puts(ipc.path); return 0; }

    char *path = path_arg ? absolute_path(argv[argc - 1]) : NULL;
    if (path_arg && !path) { fprintf(stderr, "view: invalid path\n"); return 2; }
    const char *internal[] = {path ? "open" : "raise", path};
    const char *const *args = path || graphical ? internal : (const char *const *)(argv + 1);
    int count = path ? 2 : graphical ? 1 : argc - 1;
    int result = ipc_send(&ipc, count, args);
    if (result >= 0) { free(path); return result; }
    if (!graphical && !path) {
        fprintf(stderr, "view: no running viewer on %s\n", getenv("DISPLAY"));
        return 1;
    }

    if (!ipc_listen(&ipc)) {
        for (int i = 0; i < 60 && result < 0; ++i) {
            poll(NULL, 0, 25);
            result = ipc_send(&ipc, count, args);
        }
        if (result < 0) fprintf(stderr, "view: could not start the local socket\n");
        free(path);
        return result < 0 ? 1 : result;
    }
    if (graphical) {
        result = run(ipc, path, -1);
        free(path);
        return result;
    }

    int handshake[2];
    if (pipe2(handshake, O_CLOEXEC)) { ipc_close(&ipc); free(path); return 1; }
    pid_t pid = fork();
    if (!pid) {
        close(handshake[0]);
        setsid();
        free(path);
        _exit(run(ipc, NULL, handshake[1]));
    }
    close(handshake[1]);
    if (pid < 0) { close(handshake[0]); ipc_close(&ipc); free(path); return 1; }
    close(ipc.server);
    close(ipc.lock);
    char ready = 0;
    ssize_t n;
    do { n = read(handshake[0], &ready, 1); } while (n < 0 && errno == EINTR);
    close(handshake[0]);
    if (n == 1 && ready == '1') result = ipc_send(&ipc, count, args);
    free(path);
    if (result < 0) fprintf(stderr, "view: could not start the viewer\n");
    return result < 0 ? 1 : result;
}
