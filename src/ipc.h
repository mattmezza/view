#ifndef IPC_H
#define IPC_H
#include "view.h"
#include <sys/un.h>

enum { IPC_MAX = 16384 };
typedef struct {
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    int server, lock;
} Ipc;

bool ipc_init(Ipc *);
int ipc_send(Ipc *, int argc, const char *const *argv);
bool ipc_listen(Ipc *);
int ipc_accept(Ipc *, char buffer[IPC_MAX], size_t *length);
void ipc_reply(int, bool success, const char *);
void ipc_close(Ipc *);
char *absolute_path(const char *);
#endif
