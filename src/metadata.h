#ifndef METADATA_H
#define METADATA_H

#include "document.h"
#include "job.h"

typedef struct { char *label, *value; } MetadataRow;
typedef struct {
    MetadataRow *rows;
    int count, capacity;
    bool loaded;
    Job extra;
} Metadata;

void metadata_load(Metadata *, const Document *);
void metadata_extra(Metadata *);
void metadata_close(Metadata *);
#endif
