#include "document.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_IMAGES 100000
#define MAX_MATCHES 10000
#define CACHE_BYTES (96u * 1024 * 1024)

static size_t image_bytes(Image i) { return (size_t)i.w * i.h * 4; }

void document_init(void) {
    static bool initialized;
    if (!initialized) { FPDF_InitLibrary(); initialized = true; }
}

void document_close(Document *d) {
    if (d->pdf) FPDF_CloseDocument(d->pdf);
    image_free(&d->picture);
    image_free(&d->scaled_picture);
    for (int i = 0; d->files && i < d->count; ++i) free(d->files[i]);
    for (size_t i = 0; i < sizeof d->cache / sizeof *d->cache; ++i)
        image_free(&d->cache[i].image);
    free(d->files);
    free(d->path);
    free(d->pages);
    free(d->matches);
    *d = (Document){.active_match = -1, .anchor.page = -1, .end.page = -1};
}

static int filename_compare(const void *a, const void *b) {
    const char *p = *(const char *const *)a, *q = *(const char *const *)b;
    int order = strverscmp(base_name(p), base_name(q));
    return order ? order : strcmp(p, q);
}

static bool scan(Document *d, const char *dir, const char *file) {
    DIR *stream = opendir(dir);
    size_t capacity = 0, dir_len = strlen(dir);
    bool found = false;
    struct dirent *entry;
    while (stream && d->count < MAX_IMAGES - 1 && (entry = readdir(stream))) {
        if (entry->d_name[0] == '.' || !is_image(entry->d_name) || entry->d_type == DT_DIR) continue;
        size_t n = dir_len + strlen(entry->d_name) + 2;
        char *path = malloc(n);
        if (!path) break;
        snprintf(path, n, "%s%s%s", dir, dir[dir_len - 1] == '/' ? "" : "/", entry->d_name);
        if (entry->d_type != DT_REG) {
            struct stat st;
            if (stat(path, &st) || !S_ISREG(st.st_mode)) { free(path); continue; }
        }
        if ((size_t)d->count == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            void *files = realloc(d->files, next * sizeof *d->files);
            if (!files) { free(path); break; }
            capacity = next;
            d->files = files;
        }
        found |= file && !strcmp(file, path);
        d->files[d->count++] = path;
    }
    if (stream) closedir(stream);
    if (file && !found) {
        void *files = realloc(d->files, (d->count + 1) * sizeof *d->files);
        if (!files) return false;
        d->files = files;
        char *copy = strdup(file);
        if (!copy) return false;
        d->files[d->count++] = copy;
    }
    if (!d->count) return false;
    qsort(d->files, d->count, sizeof *d->files, filename_compare);
    for (int i = 0; file && i < d->count; ++i)
        if (!strcmp(file, d->files[i])) { d->current = i; break; }
    return true;
}

bool document_image(Document *d, int i, char error[256]) {
    if (i < 0 || i >= d->count) return false;
    Image im = image_load(d->files[i], 0);
    if (!im.pixels) {
        snprintf(error, 256, "Could not decode %.180s", base_name(d->files[i]));
        return false;
    }
    char *path = strdup(d->files[i]);
    if (!path) {
        image_free(&im);
        snprintf(error, 256, "Out of memory");
        return false;
    }
    image_free(&d->picture);
    image_free(&d->scaled_picture);
    free(d->path);
    d->path = path;
    d->picture = im;
    d->current = i;
    return true;
}

static bool pdf_file(const char *path) {
    const char *ext = strrchr(base_name(path), '.');
    if (ext && !strcasecmp(ext, ".pdf")) return true;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char header[1024];
    size_t n = fread(header, 1, sizeof header, file);
    fclose(file);
    return memmem(header, n, "%PDF-", 5) != NULL;
}

OpenResult document_open(Document *d, const char *path, const char *password, char error[256]) {
    OpenResult result = DOC_FAIL;
    Document next = {.anchor.page = -1, .end.page = -1, .active_match = -1};
    char *full = realpath(path, NULL);
    struct stat st;
    if (!full || stat(full, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
        snprintf(error, 256, "Could not open %.190s", base_name(path));
        goto done;
    }

    if (S_ISREG(st.st_mode) && pdf_file(full)) {
        document_init();
        next.pdf = FPDF_LoadDocument(full, password);
        if (!next.pdf) {
            unsigned long cause = FPDF_GetLastError();
            if (cause == FPDF_ERR_PASSWORD) result = DOC_PASSWORD;
            const char *message = cause == FPDF_ERR_PASSWORD ? "Password required" :
                                  cause == FPDF_ERR_SECURITY ? "Unsupported PDF encryption" : "Could not read PDF";
            snprintf(error, 256, "%s", message);
            goto done;
        }
        next.count = FPDF_GetPageCount(next.pdf);
        if (next.count <= 0 || next.count > 1000000 ||
            !(next.pages = calloc(next.count, sizeof *next.pages))) {
            snprintf(error, 256, "PDF has no readable pages or exceeds page limit");
            goto done;
        }
        for (int i = 0; i < next.count; ++i) {
            FS_SIZEF size;
            if (!FPDF_GetPageSizeByIndexF(next.pdf, i, &size) ||
                !(size.width >= 1 && size.width <= 144000 && size.height >= 1 && size.height <= 144000))
                size = (FS_SIZEF){612, 792};
            next.pages[i] = (PageSize){size.width, size.height, next.total_height};
            next.max_width = fmax(next.max_width, size.width);
            next.total_height += size.height;
        }
        next.path = full;
        full = NULL;
    } else {
        bool directory = S_ISDIR(st.st_mode);
        char *dir = strdup(full);
        if (!dir) { snprintf(error, 256, "Out of memory"); goto done; }
        if (!directory) {
            char *slash = strrchr(dir, '/');
            slash[slash == dir ? 1 : 0] = 0;
        }
        bool found = scan(&next, dir, directory ? NULL : full);
        free(dir);
        if (!found) { snprintf(error, 256, "No supported images found"); goto done; }
        if (directory) {
            bool loaded = false;
            for (int i = 0; i < next.count && !loaded; ++i)
                loaded = document_image(&next, i, error);
            if (!loaded) goto done;
        } else if (!document_image(&next, next.current, error)) {
            goto done;
        }
    }
    document_close(d);
    *d = next;
    free(full);
    return DOC_OK;

done:
    free(full);
    document_close(&next);
    return result;
}

Image document_render_page(const Document *d, int index, int wanted, int flags) {
    PageSize p = d->pages[index];
    double scale = fmin(wanted / p.w, 4096.0 / fmax(p.w, p.h));
    int w = fmax(1, round(p.w * scale)), h = fmax(1, round(p.h * scale));
    Image im = {0};
    FPDF_PAGE page = FPDF_LoadPage(d->pdf, index);
    if (!page) return im;
    unsigned char *pixels = malloc((size_t)w * h * 4);
    FPDF_BITMAP bitmap = pixels ? FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, pixels, w * 4) : NULL;
    if (bitmap) {
        FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xffffffff);
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, flags);
        FPDFBitmap_Destroy(bitmap);
        for (size_t i = 0; i < (size_t)w * h * 4; i += 4) {
            unsigned char blue = pixels[i];
            pixels[i] = pixels[i + 2];
            pixels[i + 2] = blue;
            pixels[i + 3] = 255;
        }
        im = (Image){pixels, w, h};
    } else {
        free(pixels);
    }
    FPDF_ClosePage(page);
    return im;
}

static CacheEntry *oldest(Document *d) {
    CacheEntry *victim = NULL;
    for (size_t i = 0; i < sizeof d->cache / sizeof *d->cache; ++i) {
        CacheEntry *entry = &d->cache[i];
        if (!entry->attempted) return entry;
        if (entry->stamp != d->stamp && (!victim || entry->age < victim->age)) victim = entry;
    }
    return victim;
}

static void evict(Document *d, CacheEntry *entry) {
    d->cache_bytes -= image_bytes(entry->image);
    image_free(&entry->image);
    entry->attempted = false;
}

Image *document_cached(Document *d, int index, bool thumb, int width, bool load, bool *pending) {
    for (size_t i = 0; i < sizeof d->cache / sizeof *d->cache; ++i) {
        CacheEntry *e = &d->cache[i];
        if (e->attempted && e->item == index && e->thumb == thumb && e->width == width) {
            e->stamp = d->stamp;
            e->age = ++d->cache_age;
            return e->image.pixels ? &e->image : NULL;
        }
    }
    if (!load) { *pending = true; return NULL; }
    CacheEntry *e = oldest(d);
    if (!e) return NULL;
    evict(d, e);
    Image im = d->pdf ? document_render_page(d, index, width, FPDF_ANNOT | FPDF_LCD_TEXT) :
                         image_load(d->files[index], width);
    while (d->cache_bytes + image_bytes(im) > CACHE_BYTES) {
        CacheEntry *victim = NULL;
        for (size_t i = 0; i < sizeof d->cache / sizeof *d->cache; ++i) {
            CacheEntry *candidate = &d->cache[i];
            if (candidate->image.pixels && candidate->stamp != d->stamp &&
                (!victim || candidate->age < victim->age)) victim = candidate;
        }
        if (!victim) break;
        evict(d, victim);
    }
    *e = (CacheEntry){index, width, thumb, true, im, d->stamp, ++d->cache_age};
    d->cache_bytes += image_bytes(im);
    return e->image.pixels ? &e->image : NULL;
}

FPDF_TEXTPAGE document_text(Document *d, int index, FPDF_PAGE *page) {
    *page = index >= 0 && index < d->count ? FPDF_LoadPage(d->pdf, index) : NULL;
    return *page ? FPDFText_LoadPage(*page) : NULL;
}

void document_search(Document *d, const char *query) {
    snprintf(d->query, sizeof d->query, "%s", query);
    utf8_to_utf16(d->query, d->query16, sizeof d->query16 / sizeof *d->query16);
    d->num_matches = 0;
    d->active_match = -1;
    d->search_page = 0;
    d->searching = query[0] != 0;
    d->search_limited = false;
}

void document_search_step(Document *d) {
    double until = now() + .011;
    do {
        int index = d->search_page++;
        FPDF_PAGE page;
        FPDF_TEXTPAGE text = document_text(d, index, &page);
        FPDF_SCHHANDLE find = text ? FPDFText_FindStart(text, d->query16, 0, 0) : NULL;
        while (find && FPDFText_FindNext(find)) {
            if (d->num_matches >= MAX_MATCHES) { d->search_limited = true; break; }
            if (d->num_matches == d->match_capacity) {
                size_t cap = d->match_capacity ? d->match_capacity * 2 : 128;
                void *matches = realloc(d->matches, cap * sizeof *d->matches);
                if (!matches) { d->search_limited = true; break; }
                d->matches = matches;
                d->match_capacity = cap;
            }
            d->matches[d->num_matches++] = (Match){index, FPDFText_GetSchResultIndex(find),
                                                   FPDFText_GetSchCount(find)};
        }
        if (find) FPDFText_FindClose(find);
        if (text) FPDFText_ClosePage(text);
        if (page) FPDF_ClosePage(page);
        d->searching = d->search_page < d->count && !d->search_limited;
    } while (d->searching && now() < until);
}

bool document_range(const Document *d, int page, int *from, int *count) {
    TextPoint a = d->anchor, b = d->end;
    if (a.page < 0 || b.page < 0) return false;
    if (a.page > b.page || (a.page == b.page && a.character > b.character)) {
        TextPoint swap = a; a = b; b = swap;
    }
    if (page < a.page || page > b.page) return false;
    *from = page == a.page ? a.character : 0;
    *count = page == b.page ? b.character - *from + 1 : -1;
    return true;
}

char *document_selection(Document *d) {
    char *result = NULL;
    size_t size = 0, capacity = 0;
    int first = d->anchor.page < d->end.page ? d->anchor.page : d->end.page;
    int last = d->anchor.page > d->end.page ? d->anchor.page : d->end.page;
    for (int i = first; i <= last && size < 1000000; ++i) {
        int start, n;
        if (!document_range(d, i, &start, &n)) break;
        FPDF_PAGE page;
        FPDF_TEXTPAGE text = document_text(d, i, &page);
        if (text) {
            int end = FPDFText_CountChars(text);
            if (n < 0 || n > end - start) n = end - start;
            if (n > 300000) n = 300000;
            unsigned short *buffer = n > 0 ? malloc((n + 1) * sizeof *buffer) : NULL;
            if (buffer) {
                int length = FPDFText_GetText(text, start, n, buffer);
                char *part = length > 0 ? utf16_to_utf8(buffer, length) : NULL;
                if (part) {
                    size_t bytes = strlen(part);
                    if (size + bytes + 2 > capacity) {
                        size_t next = (size + bytes + 2) * 2;
                        void *grown = realloc(result, next);
                        if (grown) { result = grown; capacity = next; }
                    }
                    if (size + bytes + 2 <= capacity) {
                        if (size) result[size++] = '\n';
                        memcpy(result + size, part, bytes + 1);
                        size += bytes;
                    }
                    free(part);
                }
                free(buffer);
            }
            FPDFText_ClosePage(text);
        }
        if (page) FPDF_ClosePage(page);
    }
    return result;
}
