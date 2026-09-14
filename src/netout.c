/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * netout.c — Implémentation du serveur TCP de diffusion (fan-out).
 */

#include "netout.h"

#include <string.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

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
        /* TCP_NODELAY : chaque trame part tout de suite. Avec Nagle (défaut),
         * les petites écritures sont regroupées en attendant l'accusé de
         * réception, et l'accusé RETARDÉ du client amplifie l'effet : les
         * trames arrivent par rafales entrecoupées de pauses. Un récepteur
         * distant qui déduit la vitesse des positions successives (qtVlm)
         * voyait alors sa vitesse, et le TWA qui en découle, osciller. */
        {
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
        s->clients[s->n_clients++] = c;
        added++;
    }
    return added;
}

/* Envoi COMPLET d'un message à un client, ou échec. Retourne 1 si tout est
 * parti, 0 si le client doit être retiré, -1 si rien n'a pu partir (client
 * saturé mais flux encore intact : la perte est tolérée, message par message).
 *
 * Un envoi PARTIEL est le piège : la socket est non bloquante, send peut
 * n'écrire qu'une fraction du message et le reste était purement jeté, ce qui
 * livrait au client une ligne YDRAW ou une phrase 0183 coupée en deux. On
 * termine donc le message, avec une courte attente d'écrivabilité ; si le
 * client reste bouché en cours de message, le flux est désynchronisé et on le
 * ferme plutôt que de lui envoyer n'importe quoi. */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    int    waits = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (off == 0)
                return -1;              /* rien n'est parti : message sauté */
            if (++waits > NETOUT_PARTIAL_WAITS)
                return 0;               /* message à moitié écrit : on ferme */
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            poll(&pfd, 1, NETOUT_PARTIAL_MS);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return 0;                       /* déconnexion / erreur */
    }
    return 1;
}

int netout_broadcast(netout_t *s, const char *buf, size_t len)
{
    int ok = 0;
    for (int i = 0; i < s->n_clients; ) {
        int r = send_all(s->clients[i], buf, len);
        if (r == 0) {            /* déconnexion, erreur, ou flux désynchronisé */
            close(s->clients[i]);
            s->clients[i] = s->clients[--s->n_clients];
            continue;
        }
        if (r > 0)
            ok++;
        i++;                     /* r < 0 : client saturé, message sauté */
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
