#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static bool secure_dir(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
           !(st.st_mode & 0077);
}

bool ipc_init(Ipc *ipc) {
    *ipc = (Ipc){.server = -1, .lock = -1};
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    if (!dir || *dir != '/' || !secure_dir(dir) || strlen(dir) > 70) {
        snprintf(fallback, sizeof fallback, "/tmp/view-%lu", (unsigned long)getuid());
        if (mkdir(fallback, 0700) && errno != EEXIST) return false;
        if (!secure_dir(fallback)) return false;
        dir = fallback;
    }
    const char *display = getenv("DISPLAY");
    if (!display || !*display) return false;
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char *s = (const unsigned char *)display; *s; ++s)
        hash = (hash ^ *s) * 1099511628211ull;
    int n = snprintf(ipc->path, sizeof ipc->path, "%s/view-%016llx.sock", dir,
                     (unsigned long long)hash);
    return n > 0 && (size_t)n < sizeof ipc->path;
}

static int ipc_socket(void) {
    return socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
}

int ipc_send(Ipc *ipc, int argc, const char *const *argv) {
    int fd = ipc_socket();
    if (fd < 0) return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, ipc->path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
        close(fd);
        return -1;
    }
    char request[IPC_MAX], response[IPC_MAX + 1];
    size_t n = 0;
    for (int i = 0; i < argc; ++i) {
        size_t length = strlen(argv[i]) + 1;
        if (length > sizeof request - n) {
            fprintf(stderr, "view: command exceeds %d bytes\n", IPC_MAX);
            close(fd);
            return 1;
        }
        memcpy(request + n, argv[i], length);
        n += length;
    }
    struct timeval timeout = {60, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    ssize_t received = send(fd, request, n, MSG_NOSIGNAL) == (ssize_t)n ?
        recv(fd, response, IPC_MAX, 0) : -1;
    close(fd);
    if (received < 1 || (response[0] != '0' && response[0] != '1')) {
        fprintf(stderr, "view: the viewer did not return a reply\n");
        return 1;
    }
    response[received] = 0;
    bool ok = response[0] == '0';
    if (received > 1) fprintf(ok ? stdout : stderr, "%s%s\n", ok ? "" : "view: ", response + 1);
    return ok ? 0 : 1;
}

bool ipc_listen(Ipc *ipc) {
    char lock_name[sizeof ipc->path + 6];
    snprintf(lock_name, sizeof lock_name, "%s.lock", ipc->path);
    int lock = open(lock_name, O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR | O_NONBLOCK, 0600);
    if (lock < 0) return false;
    struct stat st;
    if (fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        flock(lock, LOCK_EX | LOCK_NB)) {
        close(lock);
        return false;
    }
    if (!lstat(ipc->path, &st) && (!S_ISSOCK(st.st_mode) || st.st_uid != getuid())) {
        close(lock);
        return false;
    }
    unlink(ipc->path);
    int fd = ipc_socket();
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, ipc->path);
    mode_t mask = umask(0077);
    bool ok = fd >= 0 && !bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(mask);
    if (!ok || listen(fd, 16)) {
        if (ok) unlink(ipc->path);
        if (fd >= 0) close(fd);
        close(lock);
        return false;
    }
    ipc->server = fd;
    ipc->lock = lock;
    return true;
}

int ipc_accept(Ipc *ipc, char buffer[IPC_MAX], size_t *length) {
    int fd = accept4(ipc->server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return -1;
    struct ucred creds;
    socklen_t size = sizeof creds;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &creds, &size) || creds.uid != getuid()) {
        close(fd);
        return -1;
    }
    struct pollfd input = {fd, POLLIN, 0};
    if (poll(&input, 1, 50) != 1 || !(input.revents & POLLIN)) {
        close(fd);
        return -1;
    }
    ssize_t n = recv(fd, buffer, IPC_MAX, MSG_TRUNC);
    if (n <= 0 || n > IPC_MAX || buffer[n - 1] != 0) {
        ipc_reply(fd, false, "invalid request");
        return -1;
    }
    *length = n;
    return fd;
}

void ipc_reply(int fd, bool ok, const char *message) {
    char packet[IPC_MAX];
    int length = snprintf(packet, sizeof packet, "%c%s", ok ? '0' : '1', message ? message : "");
    if (length > 0)
        (void)send(fd, packet, length < (int)sizeof packet ? length : (int)sizeof packet - 1, MSG_NOSIGNAL);
    close(fd);
}

void ipc_close(Ipc *ipc) {
    if (ipc->server >= 0) {
        close(ipc->server);
        unlink(ipc->path);
    }
    if (ipc->lock >= 0) close(ipc->lock);
}

char *absolute_path(const char *path) {
    if (!path || !*path) return NULL;
    if (*path == '/') return strdup(path);
    if (*path == '~' && path[1] == '/') {
        const char *home = getenv("HOME");
        if (home && *home) {
            char *out;
            return asprintf(&out, "%s%s", home, path + 1) >= 0 ? out : NULL;
        }
    }
    char *dir = getcwd(NULL, 0);
    if (!dir) return NULL;
    char *out;
    int length = asprintf(&out, "%s/%s", dir, path);
    free(dir);
    return length >= 0 ? out : NULL;
}
