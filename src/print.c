#include "app.h"
#include "fpdf_annot.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_save.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const struct { const char *name, *media; double w, h; } papers[] = {
    {"A4", "iso_a4_210x297mm", 595.276, 841.890},
    {"Letter", "na_letter_8.5x11in", 612, 792},
    {"A3", "iso_a3_297x420mm", 841.890, 1190.551},
    {"A5", "iso_a5_148x210mm", 419.528, 595.276},
    {"Legal", "na_legal_8.5x14in", 612, 1008},
    {"4 × 6 in", "na_index-4x6_4x6in", 288, 432},
    {"5 × 7 in", "na_5x7_5x7in", 360, 504},
};
static const struct { int cols, rows; const char *name; } grids[] = {
    {1, 1, "1 per sheet"}, {1, 2, "2 per sheet"}, {2, 2, "4 per sheet"},
    {2, 3, "6 per sheet"}, {3, 3, "9 per sheet"},
};
static const char *const orders[] = {"Across →", "Down ↓", "Across ←", "Down, right first"};
static const char *const colors[] = {"Color", "Black & white"};
static const char *const sides[] = {"One-sided", "Long edge", "Short edge"};
static const char *const sides_ipp[] = {"one-sided", "two-sided-long-edge", "two-sided-short-edge"};
static const char *const orientation[] = {"Portrait", "Landscape"};
static const char *const item_scopes[] = {"Current", "All", "Range"};

void print_open(App *a) {
    if (a->print.job.fd >= 0) { app_toast(a, "A print job is being prepared"); return; }
    job_close(&a->print.printers, true);
    app_prompt(a, PROMPT_PRINT);
    a->print.settings = (PrintSettings){.option = {[PRINT_ITEMS] = a->doc.pdf && a->doc.count > 1,
        [PRINT_SCALE] = 100, [PRINT_COPIES] = 1}};
    a->print.num_queues = a->print.scroll = 0;
    a->print.editable = false;
    if (!job_start(&a->print.printers, 8192)) {
        setenv("LC_ALL", "C", 1);
        execlp("lpstat", "lpstat", "-e", (char *)NULL);
        _exit(127);
    }
}

void print_discover(App *a) {
    PrintDialog *p = &a->print;
    char *rest = p->printers.text, *line;
    while ((line = strsep(&rest, "\r\n")) && p->num_queues < PRINTER_LIMIT) {
        size_t n = strlen(line);
        if (n && n < sizeof p->queues[0] && strspn(line,
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.@") == n)
            memcpy(p->queues[p->num_queues++], line, n + 1);
    }
    job_close(&p->printers, false);
    a->dirty = true;
}

void print_close(App *a) {
    job_close(&a->print.printers, true);
    job_close(&a->print.job, false);
}

bool print_range(const App *a, int *first, int *last) {
    int scope = a->print.settings.option[PRINT_ITEMS];
    *first = scope == 1 ? 0 : a->doc.current;
    *last = scope == 1 ? a->doc.count - 1 : *first;
    if (scope < 2) return a->doc.count > 0;
    const char *s = a->input;
    while (isspace((unsigned char)*s)) ++s;
    if (!isdigit((unsigned char)*s)) return false;
    char *end;
    long from = strtol(s, &end, 10), to = from;
    while (isspace((unsigned char)*end)) ++end;
    if (*end == '-') {
        s = end + 1;
        while (isspace((unsigned char)*s)) ++s;
        if (!isdigit((unsigned char)*s)) return false;
        to = strtol(s, &end, 10);
        while (isspace((unsigned char)*end)) ++end;
    }
    if (*end || from < 1 || to < from || to > a->doc.count) return false;
    *first = from - 1;
    *last = to - 1;
    return true;
}

const char *print_value(const App *a, PrintField field, char buf[32]) {
    int value = a->print.settings.option[field];
    switch (field) {
    case PRINT_PRINTER: return value ? a->print.queues[value - 1] : "System default";
    case PRINT_ITEMS: return item_scopes[value];
    case PRINT_PAPER: return papers[value].name;
    case PRINT_ORIENTATION: return orientation[value];
    case PRINT_GRID: return grids[value].name;
    case PRINT_ORDER: return orders[value];
    case PRINT_SCALE: snprintf(buf, 32, "%d%% of fit", value); return buf;
    case PRINT_COLOR: return colors[value];
    case PRINT_DUPLEX: return sides[value];
    case PRINT_COPIES: snprintf(buf, 32, "%d", value); return buf;
    default: return "";
    }
}

void print_step(App *a, PrintField field, int dir) {
    if (a->print.job.fd >= 0) return;
    PrintSettings *p = &a->print.settings;
    int *value = &p->option[field];
    int count = (int[]){
        [PRINT_PRINTER] = a->print.num_queues + 1, [PRINT_ITEMS] = 3,
        [PRINT_PAPER] = sizeof papers / sizeof *papers, [PRINT_ORIENTATION] = 2,
        [PRINT_GRID] = sizeof grids / sizeof *grids, [PRINT_ORDER] = 4, [PRINT_COLOR] = 2, [PRINT_DUPLEX] = 3
    }[field];
    if (field == PRINT_SCALE) *value = fmax(5, fmin(100, *value + dir * 5));
    else if (field == PRINT_COPIES) *value = fmax(1, fmin(99, *value + dir));
    else if (count) *value = (*value + count + dir) % count;
    a->print.editable = field == PRINT_ITEMS && *value == 2;
    if (a->print.editable && !*a->input) {
        snprintf(a->input, sizeof a->input, "1-%d", a->doc.count);
        a->caret = strlen(a->input);
        a->select_input = true;
    }
    a->prompt_error[0] = 0;
    a->dirty = true;
}

PrintSheet print_sheet(const PrintSettings *p) {
    int paper = p->option[PRINT_PAPER], grid = p->option[PRINT_GRID];
    bool wide = p->option[PRINT_ORIENTATION];
    return (PrintSheet){
        .w = wide ? papers[paper].h : papers[paper].w,
        .h = wide ? papers[paper].w : papers[paper].h,
        .cols = wide ? grids[grid].rows : grids[grid].cols,
        .rows = wide ? grids[grid].cols : grids[grid].rows,
        .margin = 18, .gap = 9,
    };
}

cui_rect print_cell(const PrintSettings *p, PrintSheet sheet, int index) {
    int order = p->option[PRINT_ORDER];
    int x = order % 2 ? index / sheet.rows : index % sheet.cols;
    int y = order % 2 ? index % sheet.rows : index / sheet.cols;
    if (order >= 2) x = sheet.cols - x - 1;
    double w = (sheet.w - 2 * sheet.margin - (sheet.cols - 1) * sheet.gap) / sheet.cols;
    double h = (sheet.h - 2 * sheet.margin - (sheet.rows - 1) * sheet.gap) / sheet.rows;
    return (cui_rect){sheet.margin + x * (w + sheet.gap), sheet.margin + y * (h + sheet.gap), w, h};
}

cui_rect print_fit(cui_rect cell, int percentage, double w, double h) {
    double scale = fmin(cell.w / w, cell.h / h) * percentage / 100;
    return (cui_rect){cell.x + (cell.w - w * scale) / 2, cell.y + (cell.h - h * scale) / 2,
                      w * scale, h * scale};
}

typedef struct {
    struct jpeg_error_mgr error;
    jmp_buf jump;
} JpegError;

static void jpeg_error(j_common_ptr c) {
    JpegError *error = (JpegError *)c->err;
    longjmp(error->jump, 1);
}

typedef struct {
    struct jpeg_compress_struct encoder;
    JpegError error;
    unsigned char *bytes, *row;
    unsigned long size;
} Jpeg;

static int jpeg_block(void *context, unsigned long offset, unsigned char *out, unsigned long n) {
    Jpeg *jpeg = context;
    if (offset > jpeg->size || n > jpeg->size - offset) return 0;
    memcpy(out, jpeg->bytes + offset, n);
    return 1;
}

static bool encode_jpeg(Jpeg *j, Image im, bool gray) {
    j->encoder.err = jpeg_std_error(&j->error.error);
    j->error.error.error_exit = jpeg_error;
    if (setjmp(j->error.jump)) return false;
    jpeg_create_compress(&j->encoder);
    jpeg_mem_dest(&j->encoder, &j->bytes, &j->size);
    j->encoder.image_width = im.w;
    j->encoder.image_height = im.h;
    j->encoder.input_components = gray ? 1 : 3;
    j->encoder.in_color_space = gray ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&j->encoder);
    if (!gray) j->encoder.comp_info[0].h_samp_factor = j->encoder.comp_info[0].v_samp_factor = 1;
    jpeg_set_quality(&j->encoder, 95, TRUE);
    jpeg_start_compress(&j->encoder, TRUE);
    while (j->encoder.next_scanline < j->encoder.image_height) {
        const unsigned char *p = im.pixels + (size_t)j->encoder.next_scanline * im.w * 4;
        unsigned char *out = j->row;
        for (int x = 0; x < im.w; ++x, p += 4) {
            int r = 255 - ((255 - p[0]) * p[3] + 127) / 255;
            int g = 255 - ((255 - p[1]) * p[3] + 127) / 255;
            int b = 255 - ((255 - p[2]) * p[3] + 127) / 255;
            if (gray) *out++ = (77 * r + 150 * g + 29 * b + 128) >> 8;
            else { *out++ = r; *out++ = g; *out++ = b; }
        }
        jpeg_write_scanlines(&j->encoder, &j->row, 1);
    }
    jpeg_finish_compress(&j->encoder);
    return true;
}

static FPDF_PAGEOBJECT photo(FPDF_DOCUMENT dest, Image im, bool gray) {
    Jpeg *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->row = malloc((size_t)im.w * (gray ? 1 : 3));
    FPDF_PAGEOBJECT object = NULL;
    if (j->row && encode_jpeg(j, im, gray)) {
        FPDF_FILEACCESS source = {.m_FileLen = j->size, .m_GetBlock = jpeg_block, .m_Param = j};
        object = FPDFPageObj_NewImageObj(dest);
        if (object && !FPDFImageObj_LoadJpegFileInline(NULL, 0, object, &source)) {
            FPDFPageObj_Destroy(object);
            object = NULL;
        }
    }
    jpeg_destroy_compress(&j->encoder);
    free(j->bytes);
    free(j->row);
    free(j);
    return object;
}

static FPDF_PAGEOBJECT vector_page(FPDF_DOCUMENT dest, const Document *d, int index) {
    FPDF_PAGE page = FPDF_LoadPage(d->pdf, index);
    if (!page) return NULL;
    bool has_annotations = FPDFPage_GetAnnotCount(page) > 0;
    FPDF_ClosePage(page);
    if (has_annotations) return NULL;
    FPDF_XOBJECT source = FPDF_NewXObjectFromPage(dest, d->pdf, index);
    if (!source) return NULL;
    FPDF_PAGEOBJECT object = FPDF_NewFormObjectFromXObject(source);
    FPDF_CloseXObject(source);
    return object;
}

static bool place(FPDF_DOCUMENT dest, FPDF_PAGE sheet, const Document *d, const PrintSettings *p,
                  cui_rect cell, int index, bool low_resolution) {
    bool gray = p->option[PRINT_COLOR];
    int scale = p->option[PRINT_SCALE];
    FPDF_PAGEOBJECT object = NULL;
    cui_rect box = cell;
    double w = 1, h = 1;
    if (d->pdf) {
        PageSize size = d->pages[index];
        box = print_fit(cell, scale, size.w, size.h);
        if (!gray && !low_resolution) {
            object = vector_page(dest, d, index);
            if (object) { w = size.w; h = size.h; }
        }
        if (!object) {
            Image raster = document_render_page(d, index, ceil(box.w * (low_resolution ? 150 : 300) / 72),
                                                FPDF_ANNOT | FPDF_PRINTING);
            if (raster.pixels) object = photo(dest, raster, gray);
            image_free(&raster);
        }
    } else {
        double dpi = 300.0 / 72;
        int edge = ceil(fmax(cell.w, cell.h) * scale / 100 * dpi);
        Image im = image_load(d->files[index], edge);
        if (im.pixels) {
            box = print_fit(cell, scale, im.w, im.h);
            double shrink = fmin(box.w * dpi / im.w, box.h * dpi / im.h);
            if (shrink < 1) {
                Image small = image_resize(im, fmax(1, ceil(im.w * shrink)), fmax(1, ceil(im.h * shrink)));
                if (small.pixels) { image_free(&im); im = small; }
            }
            object = photo(dest, im, gray);
        }
        image_free(&im);
    }
    if (!object) return false;
    FPDFPageObj_Transform(object, box.w / w, 0, 0, box.h / h, box.x,
                          FPDF_GetPageHeightF(sheet) - box.y - box.h);
    FPDFPage_InsertObject(sheet, object);
    return true;
}

typedef struct { FPDF_FILEWRITE base; FILE *file; } PdfOutput;

static int write_block(FPDF_FILEWRITE *writer, const void *data, unsigned long size) {
    PdfOutput *out = (PdfOutput *)writer;
    return fwrite(data, 1, size, out->file) == size;
}

static bool build_job(const Document *d, const PrintSettings *p, FILE *file) {
    document_init();
    FPDF_DOCUMENT dest = FPDF_CreateNewDocument();
    if (!dest) return false;
    PrintSheet layout = print_sheet(p);
    int cells = layout.rows * layout.cols, sheet = 0;
    bool low_resolution = d->pdf && !(FPDF_GetDocPermissions(d->pdf) & 0x800);
    bool ok = true;
    for (int i = p->first; ok && i <= p->last; ) {
        FPDF_PAGE page = FPDFPage_New(dest, sheet++, layout.w, layout.h);
        if (!page) { ok = false; break; }
        for (int j = 0; ok && j < cells && i <= p->last; ++i, ++j)
            ok = place(dest, page, d, p, print_cell(p, layout, j), i, low_resolution);
        if (ok) ok = FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
    }
    PdfOutput writer = {.base = {1, write_block}, .file = file};
    if (ok) ok = FPDF_SaveAsCopy(dest, &writer.base, 0) && !fflush(file) && !fseeko(file, 0, SEEK_SET);
    FPDF_CloseDocument(dest);
    return ok;
}

static void submit_job(App *a, PrintSettings p) {
    /* The worker may finish after the window closes. */
    signal(SIGPIPE, SIG_IGN);
    close(ConnectionNumber(a->display));
    close(a->ipc.server);
    close(a->ipc.lock);
    if (a->update_fd >= 0) close(a->update_fd);
    if (a->metadata.extra.fd >= 0) close(a->metadata.extra.fd);
    if (a->print.printers.fd >= 0) close(a->print.printers.fd);
    FILE *file = tmpfile();
    if (!file || !build_job(&a->doc, &p, file)) {
        puts("0Could not prepare the print job. Check that every selected item can be opened.");
        fflush(stdout);
        _exit(1);
    }
    char media[80], copies[12], duplex[80], color[64];
    snprintf(media, sizeof media, "media=%s", papers[p.option[PRINT_PAPER]].media);
    snprintf(copies, sizeof copies, "%d", p.option[PRINT_COPIES]);
    snprintf(duplex, sizeof duplex, "sides=%s", sides_ipp[p.option[PRINT_DUPLEX]]);
    snprintf(color, sizeof color, "print-color-mode=%s", p.option[PRINT_COLOR] ? "monochrome" : "color");
    const char *args[23] = {"lp", "-s", "-t", base_name(a->doc.path), "-n", copies, "-o", media,
        "-o", duplex, "-o", color, "-o", "print-scaling=none", "-o", "number-up=1",
        "-o", "Collate=True"};
    if (p.option[PRINT_PRINTER]) {
        args[18] = "-d";
        args[19] = a->print.queues[p.option[PRINT_PRINTER] - 1];
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (dup2(fileno(file), STDIN_FILENO) < 0) _exit(126);
        close(fileno(file));
        if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) _exit(126);
        execvp("lp", (char *const *)args);
        _exit(errno == ENOENT ? 127 : 126);
    }
    fclose(file);
    int status = 0;
    if (pid > 0) {
        pid_t waited;
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0) pid = -1;
    }
    if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) puts("\n1");
    else if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 127)
        puts("\n0Printing needs the CUPS lp command.");
    else puts("\n0");
    fflush(stdout);
    _exit(0);
}

void print_submit(App *a) {
    if (a->print.job.fd >= 0) return;
    PrintSettings p = a->print.settings;
    if (!print_range(a, &p.first, &p.last)) {
        snprintf(a->prompt_error, sizeof a->prompt_error, "Enter a page or range from 1 to %d.", a->doc.count);
    } else if (a->doc.pdf && !(FPDF_GetDocPermissions(a->doc.pdf) & 4)) {
        snprintf(a->prompt_error, sizeof a->prompt_error, "This PDF does not allow printing.");
    } else {
        pid_t worker = job_start(&a->print.job, 4096);
        if (!worker) submit_job(a, p);
        if (worker > 0) {
            job_close(&a->print.printers, true);
            a->prompt_error[0] = 0;
            a->dirty = true;
            return;
        }
        snprintf(a->prompt_error, sizeof a->prompt_error, "Could not start the print job.");
    }
    a->dirty = true;
}

void print_done(App *a) {
    char *message = a->print.job.text;
    size_t length = a->print.job.length;
    while (length && isspace((unsigned char)message[length - 1])) message[--length] = 0;
    char *last = strrchr(message, '\n');
    if (last) *last++ = 0;
    else last = message;
    if (!strcmp(last, "1")) {
        if (a->prompt == PROMPT_PRINT) app_prompt(a, PROMPT_NONE);
        app_toast(a, "Sent to printer");
    } else {
        const char *error = *last == '0' && last[1] ? last + 1 :
                            last != message && *message ? message : "Could not send the print job";
        if (a->prompt == PROMPT_PRINT) {
            snprintf(a->prompt_error, sizeof a->prompt_error, "%s", error);
            a->dirty = true;
        } else app_toast(a, error);
    }
    job_close(&a->print.job, false);
}
