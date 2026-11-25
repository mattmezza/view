#include "metadata.h"
#include "fpdf_doc.h"
#include <avif/avif.h>
#include <libexif/exif-data.h>
#include <libexif/exif-loader.h>
#include <libexif/exif-utils.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define METADATA_LIMIT (4u * 1024 * 1024)
#define MAX_FIELDS 2048

static void add(Metadata *m, const char *label, const char *value) {
    if (!label || !*label || !value || !*value || m->count >= MAX_FIELDS) return;
    if (m->count == m->capacity) {
        int capacity = m->capacity ? m->capacity * 2 : 32;
        MetadataRow *rows = realloc(m->rows, (size_t)capacity * sizeof *rows);
        if (!rows) return;
        m->rows = rows;
        m->capacity = capacity;
    }
    char *key = strndup(label, 128), *text = strndup(value, 4096);
    if (!key || !text) { free(key); free(text); return; }
    for (char *p = key; *p; ++p)
        if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
    for (char *p = text; *p; ++p)
        if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
    m->rows[m->count++] = (MetadataRow){key, text};
}

static void add_time(Metadata *m, const char *name, time_t timestamp) {
    char value[48];
    struct tm local;
    if (localtime_r(&timestamp, &local) && strftime(value, sizeof value, "%Y-%m-%d %H:%M:%S %z", &local))
        add(m, name, value);
}

static uint32_t u32(const unsigned char *p, bool little) {
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n = (n << 8) | p[little ? 3 - i : i];
    return n;
}

static bool tiff_header(const unsigned char *p, size_t n) {
    return n >= 8 && (!memcmp(p, "II\x2a\0", 4) || !memcmp(p, "MM\0\x2a", 4));
}

static void read_exif(ExifData *exif, const unsigned char *p, size_t n) {
    if (n > METADATA_LIMIT) return;
    if (!tiff_header(p, n)) {
        if (n >= 6 && !memcmp(p, "Exif\0\0", 6)) exif_data_load_data(exif, p, n);
        return;
    }
    unsigned char *block = malloc(n + 6);
    if (!block) return;
    memcpy(block, "Exif\0\0", 6);
    memcpy(block + 6, p, n);
    exif_data_load_data(exif, block, n + 6);
    free(block);
}

static bool coordinate(ExifData *exif, ExifTag tag, ExifTag ref_tag, double *out) {
    ExifContent *gps = exif->ifd[EXIF_IFD_GPS];
    ExifEntry *entry = exif_content_get_entry(gps, tag);
    ExifEntry *ref = exif_content_get_entry(gps, ref_tag);
    if (!entry || entry->format != EXIF_FORMAT_RATIONAL || entry->size < 24 ||
        !ref || !ref->size || !ref->data[0] || !strchr("NSEW", ref->data[0])) return false;
    double n = 0, divisor = 1;
    for (int i = 0; i < 3; ++i) {
        ExifRational ratio = exif_get_rational(entry->data + i * 8, exif_data_get_byte_order(exif));
        if (!ratio.denominator) return false;
        n += (double)ratio.numerator / ratio.denominator / divisor;
        divisor *= 60;
    }
    *out = ref->data[0] == 'S' || ref->data[0] == 'W' ? -n : n;
    return true;
}

static void exif_rows(Metadata *m, ExifData *exif) {
    char value[4097];
    double lat, lon;
    if (coordinate(exif, EXIF_TAG_GPS_LATITUDE, EXIF_TAG_GPS_LATITUDE_REF, &lat) &&
        coordinate(exif, EXIF_TAG_GPS_LONGITUDE, EXIF_TAG_GPS_LONGITUDE_REF, &lon)) {
        snprintf(value, sizeof value, "%.6f, %.6f", lat, lon);
        add(m, "GPS coordinates", value);
    }
    for (ExifIfd i = 0; i < EXIF_IFD_COUNT; ++i) {
        ExifContent *content = exif->ifd[i];
        for (unsigned j = 0; j < content->count && m->count < MAX_FIELDS; ++j) {
            ExifEntry *entry = content->entries[j];
            const char *name = exif_tag_get_title_in_ifd(entry->tag, i);
            char fallback[80];
            if (!name || i == EXIF_IFD_1) {
                if (name) snprintf(fallback, sizeof fallback, "Thumbnail %s", name);
                else snprintf(fallback, sizeof fallback, "%s tag 0x%04X",
                              exif_ifd_get_name(i), (unsigned)entry->tag);
                name = fallback;
            }
            add(m, name, exif_entry_get_value(entry, value, sizeof value));
        }
    }
    ExifMnoteData *notes = exif_data_get_mnote_data(exif);
    unsigned count = notes ? exif_mnote_data_count(notes) : 0;
    for (unsigned i = 0; i < count && m->count < MAX_FIELDS; ++i)
        add(m, exif_mnote_data_get_title(notes, i), exif_mnote_data_get_value(notes, i, value, sizeof value));
}

static void png_text(Metadata *m, const char type[4], unsigned char *data, size_t size) {
    unsigned char *end = data + size, *p = memchr(data, 0, size);
    if (!p || p == data || p - data > 79) return;
    ++p;
    bool compressed = !memcmp(type, "zTXt", 4), latin1 = memcmp(type, "iTXt", 4) != 0;
    if (!latin1) {
        if (end - p < 2 || p[0] > 1 || p[1] != 0) return;
        compressed = *p != 0;
        p += 2;
        for (int i = 0; i < 2; ++i) {
            p = memchr(p, 0, end - p);
            if (!p) return;
            ++p;
        }
    } else if (compressed) {
        if (p == end || *p++) return;
    }
    unsigned char decoded[8193];
    if (compressed) {
        unsigned long length = sizeof decoded - 1;
        if (uncompress(decoded, &length, p, end - p) != Z_OK) return;
        p = decoded;
        end = decoded + length;
    }
    char label[321], value[4097];
    size_t k = 0, n = 0;
    for (const unsigned char *q = data; *q; ++q) k += utf8_encode(label + k, *q);
    label[k] = 0;
    if (latin1) {
        for (; p < end && n < sizeof value - 4; ++p) n += utf8_encode(value + n, *p);
    } else {
        n = (size_t)(end - p) < sizeof value - 1 ? (size_t)(end - p) : sizeof value - 1;
        memcpy(value, p, n);
    }
    value[n] = 0;
    add(m, label, value);
}

static void container_rows(Metadata *m, ExifData *exif, FILE *file, bool webp, off_t end) {
    unsigned char header[8];
    for (unsigned chunks = 0; chunks < 16384 && m->count < MAX_FIELDS &&
         fread(header, 1, 8, file) == 8; ++chunks) {
        const char *name = (const char *)header + (webp ? 0 : 4);
        uint32_t size = u32(header + (webp ? 4 : 0), webp);
        off_t position = ftello(file);
        off_t next = position + (off_t)size + (webp ? size % 2 : 4);
        if (position < 0 || next > end) break;
        bool exif_chunk = !memcmp(name, webp ? "EXIF" : "eXIf", 4);
        bool text_chunk = !webp && (!memcmp(name, "tEXt", 4) || !memcmp(name, "iTXt", 4) ||
                                    !memcmp(name, "zTXt", 4));
        if ((exif_chunk || text_chunk) && size && size <= METADATA_LIMIT) {
            unsigned char *p = malloc(size);
            if (p && fread(p, 1, size, file) == size) {
                if (exif_chunk) read_exif(exif, p, size);
                else png_text(m, name, p, size);
            }
            free(p);
        }
        if (fseeko(file, next, SEEK_SET)) break;
    }
}

static void image_rows(Metadata *m, const char *path, off_t size) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    FILE *file = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) ? fdopen(fd, "rb") : NULL;
    ExifData *exif = file ? exif_data_new() : NULL;
    if (!exif) {
        if (file) fclose(file);
        else if (fd >= 0) close(fd);
        return;
    }
    exif_data_unset_option(exif, EXIF_DATA_OPTION_IGNORE_UNKNOWN_TAGS);
    exif_data_unset_option(exif, EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
    unsigned char head[12];
    if (fread(head, 1, sizeof head, file) == sizeof head) {
        if (!memcmp(head, "\xff\xd8", 2)) {
            ExifLoader *loader = exif_loader_new();
            if (loader) {
                unsigned char bytes[4096];
                rewind(file);
                size_t nread;
                while ((nread = fread(bytes, 1, sizeof bytes, file)) &&
                       exif_loader_write(loader, bytes, nread)) {}
                const unsigned char *p = NULL;
                unsigned n = 0;
                exif_loader_get_buf(loader, &p, &n);
                if (p && n) read_exif(exif, p, n);
                exif_loader_unref(loader);
            }
        } else if (!memcmp(head, "\x89PNG\r\n\x1a\n", 8)) {
            if (!fseeko(file, 8, SEEK_SET)) container_rows(m, exif, file, false, size);
        } else if (!memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WEBP", 4)) {
            container_rows(m, exif, file, true, size);
        } else if (tiff_header(head, sizeof head)) {
            size_t length = size > METADATA_LIMIT ? METADATA_LIMIT : (size_t)size;
            unsigned char *p = malloc(length);
            rewind(file);
            if (p && fread(p, 1, length, file) == length) read_exif(exif, p, length);
            free(p);
        } else if (!memcmp(head + 4, "ftyp", 4)) {
            avifDecoder *decoder = avifDecoderCreate();
            if (decoder) {
                decoder->ignoreXMP = AVIF_TRUE;
                if (avifDecoderSetIOFile(decoder, path) == AVIF_RESULT_OK &&
                    avifDecoderParse(decoder) == AVIF_RESULT_OK)
                    read_exif(exif, decoder->image->exif.data, decoder->image->exif.size);
                avifDecoderDestroy(decoder);
            }
        }
    }
    exif_rows(m, exif);
    exif_data_unref(exif);
    fclose(file);
}

static void pdf_rows(Metadata *m, FPDF_DOCUMENT pdf) {
    static const char *const keys[] = {"Title", "Author", "Subject", "Keywords",
        "Creator", "Producer", "CreationDate", "ModDate"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i) {
        unsigned long n = FPDF_GetMetaText(pdf, keys[i], NULL, 0);
        unsigned short *wide = n > 2 && n <= 16384 ? malloc(n) : NULL;
        if (!wide) continue;
        if (FPDF_GetMetaText(pdf, keys[i], wide, n) == n) {
            char *value = utf16_to_utf8(wide, n / 2);
            const char *label = i == 6 ? "Created (PDF)" : i == 7 ? "Modified (PDF)" : keys[i];
            add(m, label, value);
            free(value);
        }
        free(wide);
    }
}

void metadata_load(Metadata *m, const Document *d) {
    if (m->loaded || !d->path) return;
    m->loaded = true;
    char value[96];
    add(m, "File", base_name(d->path));
    add(m, "Location", d->path);
    if (d->pdf) {
        snprintf(value, sizeof value, "%d", d->count);
        add(m, "Pages", value);
        int version;
        if (FPDF_GetFileVersion(d->pdf, &version)) {
            snprintf(value, sizeof value, "PDF %d.%d", version / 10, version % 10);
            add(m, "Format", value);
        }
    } else {
        snprintf(value, sizeof value, "%d × %d px", d->picture.w, d->picture.h);
        add(m, "Dimensions", value);
    }
    struct stat st;
    bool exists = !stat(d->path, &st);
    if (exists) {
        snprintf(value, sizeof value, "%lld bytes", (long long)st.st_size);
        add(m, "File size", value);
        add_time(m, "File modified", st.st_mtime);
    }
    if (d->pdf) pdf_rows(m, d->pdf);

    if (job_start(&m->extra, 196608) == 0) {
        if (!d->pdf && exists) {
            Metadata native = {0};
            image_rows(&native, d->path, st.st_size);
            for (int i = 0; i < native.count; ++i)
                printf("%s\t%s\n", native.rows[i].label, native.rows[i].value);
            fflush(stdout);
        }
        execlp("exiftool", "exiftool", "-config", "", "-a", "-u", "-G1", "-t",
               "--System:all", "--ExifTool:all", "--File:all", "--", d->path, (char *)NULL);
        _exit(127);
    }
}

void metadata_extra(Metadata *m) {
    char *rest = m->extra.text, *line;
    while ((line = strsep(&rest, "\n")) && m->count < MAX_FIELDS) {
        if (!rest && m->extra.length + 1 == m->extra.capacity) break;
        char *first = strchr(line, '\t');
        if (!first) continue;
        *first++ = 0;
        char *next = strchr(first, '\t');
        if (!next) { add(m, line, first); continue; }
        *next++ = 0;
        if (!strcmp(line, "PDF") &&
            (!strcmp(first, "PDF Version") || !strcmp(first, "Page Count") || !strcmp(first, "Title") ||
             !strcmp(first, "Author") || !strcmp(first, "Subject") || !strcmp(first, "Keywords") ||
             !strcmp(first, "Producer") || !strcmp(first, "Creator") || !strcmp(first, "Create Date") ||
             !strcmp(first, "Modify Date"))) continue;
        bool duplicate = false;
        for (int i = 0; i < m->count; ++i)
            if (!strcasecmp(m->rows[i].label, first) && !strcmp(m->rows[i].value, next)) {
                duplicate = true;
                break;
            }
        if (duplicate) continue;
        char label[148];
        snprintf(label, sizeof label, "%.94s · %.48s", first, line);
        add(m, label, next);
    }
    job_close(&m->extra, false);
}

void metadata_close(Metadata *m) {
    for (int i = 0; i < m->count; ++i) {
        free(m->rows[i].label);
        free(m->rows[i].value);
    }
    free(m->rows);
    job_close(&m->extra, true);
    *m = (Metadata){.extra.fd = -1};
}
