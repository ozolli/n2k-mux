/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * cansock.h — Aides SocketCAN partagées (ouverture + émission d'ISO Request).
 *
 * Sert au daemon (émission des ISO Request en TX socketcan, remplaçant le FIFO +
 * actisense-serial) et aux outils socketcan. Zéro allocation.
 */

#ifndef N2KMUX_CANSOCK_H
#define N2KMUX_CANSOCK_H

/* Ouvre un socket CAN RAW (SocketCAN) lié à l'interface `ifname` (ex. "can0").
 * Retourne le descripteur, ou -1 (message d'erreur sur stderr). */
int cansock_open(const char *ifname);

/* Émet une ISO Request (PGN 59904, dst=255 broadcast) demandant `req_pgn`
 * (ex. 60928 puis 126996), depuis l'adresse source `src_addr` (0-251).
 * Construit la trame N2K (ID CAN 29 bits) et l'écrit telle quelle. 0 ou -1. */
int cansock_send_iso_request(int fd, int src_addr, int req_pgn);

/* Lit UNE trame en attente (non bloquant). Si présente, met sa DLC dans *dlc et
 * retourne 1 ; sinon 0. Le socket CAN RAW reçoit tout le bus → sert à mesurer la
 * charge réelle (trames exactes + DLC) en plus du TX des ISO Request. */
int cansock_recv_dlc(int fd, int *dlc);

#endif /* N2KMUX_CANSOCK_H */
