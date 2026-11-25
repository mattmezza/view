#ifndef JOB_H
#define JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int fd;
    pid_t pid;
    char *text;
    size_t length, capacity;
} Job;

/* Like fork. The child owns stdout, the parent reads it through the Job. */
pid_t job_start(Job *, size_t capacity);
bool job_read(Job *);
void job_close(Job *, bool cancel);

#endif
