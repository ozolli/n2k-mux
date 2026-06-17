/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * netout.c — Implémentation du serveur TCP de diffusion (fan-out).
 */

#include "netout.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int netout_open(netout_t *s, int port)
{
    memset(s, 0, sizeof *s);
    s->listen_fd = -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(fd, 8) != 0 ||
        set_nonblock(fd) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    /* port effectif (cas port == 0 : choisi par l'OS) */
    struct sockaddr_in got;
    socklen_t gl = sizeof got;
    s->port = (getsockname(fd, (struct sockaddr *)&got, &gl) == 0)
              ? (int)ntohs(got.sin_port) : port;

    s->listen_fd = fd;
    return 0;
}

int netout_accept(netout_t *s)
{
    if (s->listen_fd < 0)
        return 0;

    int added = 0;
    for (;;) {
        int c = accept(s->listen_fd, NULL, NULL);
        if (c < 0)
            break;   /* EAGAIN/EWOULDBLOCK : plus rien en attente */
        if (s->n_clients >= NETOUT_MAX_CLIENTS) {
            close(c);            /* table pleine : on refuse */
            continue;
        }
        set_nonblock(c);
        s->clients[s->n_clients++] = c;
        added++;
    }
    return added;
}

int netout_broadcast(netout_t *s, const char *buf, size_t len)
{
    int ok = 0;
    for (int i = 0; i < s->n_clients; ) {
        ssize_t n = send(s->clients[i], buf, len, MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            i++;                 /* client lent : on saute (perte tolérée) */
            continue;
        }
        if (n <= 0) {            /* déconnexion / erreur : retrait */
            close(s->clients[i]);
            s->clients[i] = s->clients[--s->n_clients];
            continue;
        }
        ok++;
        i++;
    }
    return ok;
}

int netout_clients(const netout_t *s)
{
    return s->n_clients;
}

void netout_close(netout_t *s)
{
    for (int i = 0; i < s->n_clients; i++)
        close(s->clients[i]);
    s->n_clients = 0;
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    s->listen_fd = -1;
}
