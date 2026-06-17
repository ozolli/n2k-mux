/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * netout.h — Serveur TCP de diffusion (fan-out) d'un flux d'octets.
 *
 * Module générique, indépendant du contenu : il accepte des clients TCP et leur
 * rediffuse les octets qu'on lui pousse. Sert aujourd'hui le flux NMEA 0183 ;
 * réutilisable tel quel pour le futur flux NMEA 2000 arbitré (qtVlm natif).
 *
 * Comme le reste du projet : zéro allocation dynamique (table de fds fixe).
 * Sockets non bloquants ; un client trop lent (tampon noyau plein, EAGAIN) perd
 * des octets plutôt que de bloquer le daemon — acceptable pour de la télémétrie
 * de navigation à haute cadence. Un client déconnecté est retiré au prochain
 * envoi.
 */

#ifndef N2KMUX_NETOUT_H
#define N2KMUX_NETOUT_H

#include <stddef.h>

#define NETOUT_MAX_CLIENTS 16

typedef struct {
    int listen_fd;                      /* socket d'écoute, -1 si fermé */
    int clients[NETOUT_MAX_CLIENTS];    /* fds des clients connectés */
    int n_clients;
    int port;                           /* port effectif (utile si ouvert sur 0) */
} netout_t;

/* Ouvre l'écoute TCP sur `port` (0 = port éphémère choisi par l'OS), toutes
 * interfaces, non bloquant, SO_REUSEADDR. Retourne 0 si ok (port effectif dans
 * s->port), -1 sinon (errno positionné). */
int netout_open(netout_t *s, int port);

/* Accepte les connexions en attente (non bloquant). À appeler périodiquement.
 * Retourne le nombre de clients acceptés lors de cet appel. */
int netout_accept(netout_t *s);

/* Diffuse `len` octets à tous les clients. Les clients en erreur (déconnexion)
 * sont retirés. Retourne le nombre de clients à qui l'envoi a (au moins
 * partiellement) réussi. Sans client, ne fait rien et retourne 0. */
int netout_broadcast(netout_t *s, const char *buf, size_t len);

/* Nombre de clients actuellement connectés. */
int netout_clients(const netout_t *s);

/* Ferme l'écoute et tous les clients. */
void netout_close(netout_t *s);

#endif /* N2KMUX_NETOUT_H */
