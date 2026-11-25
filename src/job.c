#include "job.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

pid_t job_start(Job *job, size_t capacity) {
    int pipefd[2];
    char *text = calloc(capacity, 1);
    if (!text) return -1;
    if (pipe2(pipefd, O_CLOEXEC)) {
        free(text);
        return -1;
    }
    pid_t pid = fork();
    if (!pid) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO) close(null);
        }
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        close(pipefd[1]);
        return 0;
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        free(text);
        return -1;
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    *job = (Job){.fd = pipefd[0], .pid = pid, .text = text, .capacity = capacity};
    return pid;
}

bool job_read(Job *job) {
    char discard[4096];
    size_t space = job->capacity - job->length - 1;
    ssize_t n = read(job->fd, space ? job->text + job->length : discard,
                     space ? space : sizeof discard);
    if (n > 0) {
        if (space) job->text[job->length += n] = 0;
        return false;
    }
    return n == 0 || (errno != EINTR && errno != EAGAIN);
}

void job_close(Job *job, bool cancel) {
    if (job->fd >= 0) close(job->fd);
    if (job->pid > 0) {
        if (cancel) kill(job->pid, SIGTERM);
        waitpid(job->pid, NULL, WNOHANG);
    }
    free(job->text);
    *job = (Job){.fd = -1};
}
