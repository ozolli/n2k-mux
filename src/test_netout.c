/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_netout.c — Vérifie le serveur TCP de diffusion : écoute, accept,
 * broadcast vers un client loopback, et retrait d'un client déconnecté.
 */

#include "netout.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int failures = 0;

static void ok(const char *what, int cond)
{
    if (!cond) { fprintf(stderr, "FAIL %s\n", what); failures++; }
}

/* Ouvre un client TCP bloquant vers 127.0.0.1:port. */
static int connect_client(int port)
{
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(c, (struct sockaddr *)&a, sizeof a) != 0) { close(c); return -1; }
    return c;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    netout_t s;
    ok("ouverture sur port éphémère", netout_open(&s, 0) == 0);
    ok("port effectif > 0", s.port > 0);
    ok("aucun client au départ", netout_clients(&s) == 0);

    /* un client se connecte → accept doit le capter */
    int c1 = connect_client(s.port);
    ok("client connecté", c1 >= 0);
    ok("accept = 1", netout_accept(&s) == 1);
    ok("1 client", netout_clients(&s) == 1);

    /* diffusion → le client reçoit exactement les octets */
    const char *msg = "$IIVDR,95.0,T,,M,1.4,N*2E\r\n";
    ok("broadcast à 1 client", netout_broadcast(&s, msg, strlen(msg)) == 1);
    char rx[128];
    ssize_t n = read(c1, rx, sizeof rx - 1);
    ok("octets reçus", n == (ssize_t)strlen(msg));
    if (n > 0) { rx[n] = '\0'; ok("contenu identique", strcmp(rx, msg) == 0); }

    /* un 2e client en parallèle */
    int c2 = connect_client(s.port);
    ok("2e client connecté", c2 >= 0);
    ok("accept = 1 (2e)", netout_accept(&s) == 1);
    ok("2 clients", netout_clients(&s) == 2);
    ok("broadcast à 2 clients", netout_broadcast(&s, msg, strlen(msg)) == 2);

    /* déconnexion d'un client → retiré au(x) prochain(s) envoi(s) */
    close(c1);
    for (int i = 0; i < 5 && netout_clients(&s) > 1; i++)
        netout_broadcast(&s, msg, strlen(msg));
    ok("client déconnecté retiré", netout_clients(&s) == 1);

    close(c2);
    netout_close(&s);
    ok("écoute fermée", s.listen_fd == -1);

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
