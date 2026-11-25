#include "app.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define RELEASE_URL "https://github.com/mattmezza/view/releases/latest"

static bool version(const char *s, unsigned value[3]) {
    if (*s == 'v') ++s;
    for (int i = 0; i < 3; ++i) {
        if (*s < '0' || *s > '9') return false;
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (n > 1000000) return false;
        }
        value[i] = n;
        if (i < 2 && *s++ != '.') return false;
    }
    return *s == 0;
}

bool update_newer(const char *current, const char *candidate) {
    unsigned have[3], want[3];
    if (!version(current, have) || !version(candidate, want)) return false;
    for (int i = 0; i < 3; ++i)
        if (want[i] != have[i]) return want[i] > have[i];
    return false;
}

void update_begin(App *a) {
    a->update_at = now() + 4 * 60 * 60;
    unsigned value[3];
    if (!a->config.check_updates || a->update_fd >= 0 || !version(VIEW_VERSION, value)) return;
    int output[2];
    if (pipe2(output, O_CLOEXEC)) return;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, STDERR_FILENO);
        dup2(output[1], STDOUT_FILENO);
        execlp("curl", "curl", "-q", "--location", "--fail", "--silent", "--head",
               "--connect-timeout", "3", "--max-time", "7", "--proto", "=https",
               "--proto-redir", "=https", "--output", "/dev/null", "--write-out",
               "%{url_effective}", RELEASE_URL, (char *)NULL);
        _exit(127);
    }
    close(output[1]);
    if (pid < 0) { close(output[0]); return; }
    a->update_fd = output[0];
    a->update_pid = pid;
    a->update_length = 0;
    fcntl(a->update_fd, F_SETFL, O_NONBLOCK);
}

void update_close(App *a) {
    if (a->update_fd >= 0) close(a->update_fd);
    a->update_fd = -1;
    if (a->update_pid > 0) {
        kill(a->update_pid, SIGTERM);
        waitpid(a->update_pid, NULL, 0);
        a->update_pid = 0;
    }
}

void update_read(App *a) {
    size_t room = sizeof a->update_buffer - a->update_length - 1;
    if (!room) { update_close(a); return; }
    ssize_t n = read(a->update_fd, a->update_buffer + a->update_length, room);
    if (n > 0) { a->update_length += n; return; }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    close(a->update_fd);
    a->update_fd = -1;
    int status = 0;
    pid_t pid;
    do { pid = waitpid(a->update_pid, &status, 0); } while (pid < 0 && errno == EINTR);
    a->update_pid = 0;
    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) || n < 0) return;
    a->update_buffer[a->update_length] = 0;
    const char *prefix = "https://github.com/mattmezza/view/releases/tag/";
    if (strncmp(a->update_buffer, prefix, strlen(prefix))) return;
    const char *tag = a->update_buffer + strlen(prefix);
    if (update_newer(VIEW_VERSION, tag)) {
        snprintf(a->update, sizeof a->update, "%.63s", tag);
        a->dirty = true;
    }
}
