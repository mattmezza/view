#ifndef PRINT_H
#define PRINT_H

#include "view.h"
#include "job.h"

typedef enum {
    PRINT_PRINTER, PRINT_ITEMS, PRINT_PAPER, PRINT_ORIENTATION, PRINT_GRID,
    PRINT_ORDER, PRINT_SCALE, PRINT_COLOR, PRINT_DUPLEX, PRINT_COPIES, PRINT_FIELDS
} PrintField;

typedef struct {
    int option[PRINT_FIELDS];
    int first, last;
} PrintSettings;

#define PRINTER_LIMIT 64

typedef struct {
    PrintSettings settings;
    Job printers, job;
    char queues[PRINTER_LIMIT][128];
    int num_queues, scroll, scroll_end;
    bool editable;
} PrintDialog;

typedef struct {
    double w, h;
    int rows, cols;
    double margin, gap;
} PrintSheet;

struct App;
void print_open(struct App *);
void print_discover(struct App *);
void print_close(struct App *);
void print_submit(struct App *);
void print_done(struct App *);
void print_step(struct App *, PrintField, int direction);
bool print_range(const struct App *, int *first, int *last);
const char *print_value(const struct App *, PrintField, char buffer[32]);
PrintSheet print_sheet(const PrintSettings *);
cui_rect print_cell(const PrintSettings *, PrintSheet, int);
cui_rect print_fit(cui_rect, int percentage, double width, double height);
#endif
