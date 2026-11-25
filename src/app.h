#ifndef APP_H
#define APP_H
#include "document.h"
#include "ipc.h"
#include "render.h"

typedef enum { PROMPT_NONE, PROMPT_OPEN, PROMPT_PASSWORD, PROMPT_FIND, PROMPT_HELP } Prompt;
typedef enum { SCALE_FIT, SCALE_WIDTH, SCALE_ACTUAL, SCALE_MANUAL } ScaleMode;
typedef enum { DRAG_NONE, DRAG_PAN, DRAG_TEXT } Drag;
typedef enum {
    ICON_OPEN, ICON_PHOTO, ICON_PDF, ICON_GALLERY, ICON_SIDEBAR,
    ICON_LEFT, ICON_RIGHT, ICON_FIND, ICON_IN, ICON_OUT, ICON_FULL,
    ICON_CLOSE, ICON_UP, ICON_DOWN, ICON_UPDATE, ICONS
} Icon;

typedef struct {
    cui_rect canvas, strip, sidebar;
    double scale;
    int thumb_width, strip_slots;
} Layout;

typedef struct {
    Display *display;
    Window window;
    Cursor invisible_cursor;
    XIM im;
    XIC ic;
    Atom wm_delete, fullscreen_atom, state_atom, clipboard_atom, targets_atom, utf8_atom,
         paste_atom, uri_atom, xdnd_aware, xdnd_enter, xdnd_position, xdnd_status, xdnd_drop,
         xdnd_finished, xdnd_selection, xdnd_action_copy, xdnd_leave;
    Window drag_source;
    bool drag_uris;
    Config config;
    Renderer renderer;
    cui_context *ui;
    Document doc;
    Ipc ipc;
    int w, h, restore_w, restore_h, restore_x, restore_y;
    bool fullscreen, fullscreen_requested, fallback_fullscreen, gallery, sidebar, dirty, running;
    bool show_cursor, thumb_pending;
    double cursor_deadline, toast_until, next_thumbnail;
    double zoom, scroll, pan;
    int thumbnail_shift, sidebar_shift, thumb_budget;
    ScaleMode scale_mode;
    Layout layout;
    Image icons[ICONS];
    Prompt prompt;
    char input[4096], prompt_error[256], password_path[4096], toast[256];
    size_t caret;
    bool select_input;
    char *clipboard;
    Drag drag;
    FPDF_PAGE drag_page;
    FPDF_TEXTPAGE drag_text;
    int drag_page_index, mx, my;
    Time last_click;
    int last_click_x, last_click_y;
    int update_fd;
    pid_t update_pid;
    char update[64], update_buffer[512];
    size_t update_length;
    double update_at, hover_since;
    cui_id hovered_id;
    char hover_label[100];
    Action frame_action;
    int frame_item, frame_command;
} App;

bool x11_init(App *);
void x11_close(App *);
void x11_raise(App *);
void x11_event(App *, XEvent *);
void x11_show_cursor(App *, bool);
void app_icons(App *);
void app_layout(App *);
void app_draw(App *);
void app_action(App *, Action);
void app_item(App *, int);
void app_command(App *, int);
void app_toast(App *, const char *);
cui_rect app_page_rect(const App *, int);
int app_page_at(const App *, double y);
void app_jump(App *, int);
void app_prompt(App *, Prompt);
void app_submit(App *);
void app_close_drag(App *);
void app_set_fullscreen(App *, bool);
void app_set_clipboard(App *, char *);
void app_scroll(App *, double);
void app_zoom(App *, ScaleMode, double);
void app_find_match(App *, int);
bool app_open(App *, const char *, const char *, char[256]);
bool update_newer(const char *, const char *);
void update_begin(App *);
void update_read(App *);
void update_close(App *);
enum { UI_NONE, UI_SUBMIT, UI_DISMISS, UI_UPDATE, UI_FIT_TOGGLE };
#endif
