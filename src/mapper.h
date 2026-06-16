/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * mapper.h — Mapping PGN NMEA 2000 → phrases NMEA 0183.
 *
 * Module (e), 2e passe. Pour un message RETENU par l'arbitre (ARB_ACCEPT),
 * extrait les champs du PGN (aux unités canboat -json) et produit les phrases
 * 0183 correspondantes via le module nmea0183.
 *
 * Unités canboat -json (hors -si), confirmées via fixupUnit() :
 *   - angles      : degrés          (rad → deg)
 *   - taux giration: deg/s          (rad/s → deg/s) ; NMEA ROT veut deg/min (×60)
 *   - vitesses    : m/s (NON converti) ; NMEA veut des nœuds (×1.943844)
 *   - profondeur  : mètres
 *   - température : °C               (K → °C)
 *   - pression    : bar              (Pa → bar)
 *   - lat/lon     : degrés décimaux
 *   - Date "YYYY.MM.DD", Time "HH:MM:SS.ssss"
 *
 * Mode min (PGN 128267, profondeur) : le mapper retient la dernière profondeur
 * de chaque source vivante et émet un DPT à la valeur MINIMALE (sécurité).
 * Mode fusion (AIS / VDM) : non géré ici — question ouverte (déléguer à n2kd ?).
 */

#ifndef N2KMUX_MAPPER_H
#define N2KMUX_MAPPER_H

#include "jsonl.h"
#include "config.h"
#include "arbiter.h"
#include "nmea0183.h"
#include <stdint.h>

#define MAP_MAX_SENT  8   /* phrases max produites par un message (GSV paginé) */

typedef struct {
    char s[MAP_MAX_SENT][NMEA_MAX_LEN];
    int  n;
} map_out_t;

typedef struct {
    char     talker[3];
    unsigned timeout_ms;                 /* vivacité des sources de profondeur */
    /* état du mode min (PGN 128267), par slot de source de la règle */
    double   depth[CFG_MAX_PRIO];
    uint64_t depth_t[CFG_MAX_PRIO];
    bool     depth_seen[CFG_MAX_PRIO];
} mapper_t;

/* Initialise (talker NULL → "II"). */
void mapper_init(mapper_t *mp, const char *talker);

/*
 * Convertit un message RETENU en phrases 0183. Remplit `out` (out->n phrases).
 * `d` est la décision de l'arbitre pour ce message (donne la règle, le mode,
 * le discriminant). `now_ms` sert au mode min (vivacité). Retourne out->n.
 * Ne fait rien (retourne 0) si d->result != ARB_ACCEPT.
 */
int mapper_map(mapper_t *mp, const jsonl_msg_t *m, const arb_decision_t *d,
               uint64_t now_ms, map_out_t *out);

#endif /* N2KMUX_MAPPER_H */
