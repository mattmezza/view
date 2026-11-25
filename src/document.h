#ifndef DOCUMENT_H
#define DOCUMENT_H
#include "view.h"
#include "fpdfview.h"
#include "fpdf_text.h"

typedef struct { double w, h, offset; } PageSize;
typedef struct { int page, start, count; } Match;
typedef struct { int page, character; } TextPoint;

typedef struct {
    int item, width;
    bool thumb, attempted;
    Image image;
    unsigned stamp;
    uint64_t age;
} CacheEntry;

typedef struct {
    char *path;
    char **files;
    int count, current;
    FPDF_DOCUMENT pdf;
    PageSize *pages;
    double max_width, total_height;
    Image picture, scaled_picture;
    CacheEntry cache[96];
    uint64_t cache_age;
    size_t cache_bytes;
    unsigned stamp;
    Match *matches;
    size_t num_matches, match_capacity;
    int active_match;
    char query[512];
    unsigned short query16[512];
    int search_page;
    bool searching, search_limited;
    TextPoint anchor, end;
} Document;

typedef enum { DOC_OK, DOC_FAIL, DOC_PASSWORD } OpenResult;
void document_close(Document *);
OpenResult document_open(Document *, const char *path, const char *password, char error[256]);
bool document_image(Document *, int index, char error[256]);
Image *document_cached(Document *, int index, bool thumb, int width, bool load, bool *pending);
void document_search(Document *, const char *);
void document_search_step(Document *);
FPDF_TEXTPAGE document_text(Document *, int, FPDF_PAGE *);
char *document_selection(Document *);
bool document_range(const Document *, int page, int *from, int *count);
#endif
