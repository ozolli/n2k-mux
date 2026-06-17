/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * ydraw.h — Formateur YDRAW (Yacht Devices RAW text) pour le N2K en réseau.
 *
 * qtVlm (≥ 5.12.27) lit le NMEA 2000 sur le réseau au format YDRAW : une ligne
 * texte par trame CAN, qu'il auto-détecte (NmeaSource::looksLikeYdraw) sur
 * n'importe quelle source NMEA (TCP/UDP/série). On émettra donc le flux N2K
 * arbitré en YDRAW via le module netout (TCP), pour qtVlm local OU distant.
 *
 * Format d'une ligne (CR LF final) :
 *   hh:mm:ss.ddd R <ID 29 bits en hex> <octet hex> <octet hex> ...
 * Exemple :
 *   12:34:56.789 R 09F80103 00 FF FF 7F FF 7F FF FF
 * Direction « R » = reçu (sens bus → application), ce que qtVlm attend en entrée.
 *
 * Ce module fournit aussi l'encodage/décodage de l'identifiant CAN 29 bits
 * (J1939/N2K) : l'ID porte priorité + PGN + adresse source (+ destination pour
 * les PGN adressés), ce dont l'arbitrage a besoin côté entrée socketcan. Comme le
 * reste du projet : zéro allocation, buffers fournis par l'appelant.
 */

#ifndef N2KMUX_YDRAW_H
#define N2KMUX_YDRAW_H

#include <stddef.h>
#include <stdint.h>

/* Longueur max d'une ligne YDRAW (8 octets) + marge, CRLF + NUL inclus. */
#define YDRAW_MAX_LEN 64

/* Construit l'identifiant CAN 29 bits (J1939/N2K) à partir de la priorité, du
 * PGN, de l'adresse source et de la destination. Pour un PGN PDU1 (adressé,
 * PF < 240) la destination occupe le champ PS ; pour un PGN PDU2 (broadcast,
 * PF ≥ 240) la destination est ignorée. */
uint32_t ydraw_encode_id(int prio, int pgn, int src, int dst);

/* Décode un identifiant CAN 29 bits → priorité / PGN / source / destination.
 * Tout pointeur de sortie NULL est ignoré. Pour un PGN PDU2, dst = 255. */
void ydraw_decode_id(uint32_t can_id, int *prio, int *pgn, int *src, int *dst);

/* Formate une trame en une ligne YDRAW dans `buf` (taille `sz`). `data`/`len`
 * sont les octets de données (len ≤ 8 en CAN classique). `tod_ms` = millisecondes
 * écoulées depuis minuit (pour l'horodatage). Retourne la longueur écrite (CRLF
 * inclus, hors NUL), ou -1 si dépassement. */
int ydraw_format(char *buf, size_t sz, uint32_t can_id,
                 const uint8_t *data, int len, uint32_t tod_ms);

/* Millisecondes écoulées depuis minuit (heure locale) — pour la production. */
uint32_t ydraw_tod_ms_now(void);

#endif /* N2KMUX_YDRAW_H */
