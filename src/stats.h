/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * stats.h — Statistiques de trafic NMEA 2000 (débit + charge de bus estimée).
 *
 * Le daemon est EN AVAL de `analyzer -json` : il voit des messages PGN déjà
 * réassemblés, pas les trames CAN brutes. La charge du bus est donc ESTIMÉE :
 *   - on compte les messages par PGN (exact) ;
 *   - on estime le nombre de trames CAN par message (1 si single-frame, sinon
 *     fast-packet : 1 + ceil((octets-6)/7), via une table compacte des PGN
 *     fast-packet courants ; défaut = 1 trame) ;
 *   - charge ≈ trames/s × ~130 bits / 250000 bit/s.
 * Précision typique de l'ordre de ±15 % ; pour une mesure exacte il faudrait
 * compter les trames brutes en amont d'`analyzer`.
 *
 * Fenêtre glissante : les débits sont calculés sur l'intervalle écoulé depuis
 * le dernier reset (stats_reset / stats_write). zéro allocation dynamique.
 */

#ifndef N2KMUX_STATS_H
#define N2KMUX_STATS_H

#include <stdint.h>
#include <stddef.h>

#define STATS_MAX_PGNS        128
#define STATS_BUS_BPS         250000   /* NMEA 2000 : 250 kbit/s */
#define STATS_BITS_PER_FRAME  130      /* trame CAN étendue ~130 bits (approx) */

typedef struct {
    int           pgn;
    unsigned long count;   /* messages dans la fenêtre courante */
    unsigned long total;   /* messages depuis le début */
} stats_pgn_t;

typedef struct {
    uint64_t      window_start;        /* début de la fenêtre (ms) */
    unsigned long msgs;                /* messages dans la fenêtre */
    unsigned long frames;              /* trames CAN estimées dans la fenêtre */
    stats_pgn_t   pgns[STATS_MAX_PGNS];
    int           n_pgns;
} stats_t;

/* Réinitialise tout (y compris les totaux). */
void stats_init(stats_t *s, uint64_t now);

/* Réinitialise la fenêtre (garde les totaux par PGN). */
void stats_reset(stats_t *s, uint64_t now);

/* Enregistre un message d'un PGN (trames estimées en interne). */
void stats_observe(stats_t *s, int pgn);

/* Nombre de trames CAN estimé pour un PGN (1 par défaut / single-frame). */
int  stats_frames_for(int pgn);

/* Débits de la fenêtre courante (sans reset). Tout pointeur peut être NULL. */
void stats_summary(const stats_t *s, uint64_t now,
                   double *msg_per_s, double *frames_per_s, double *load_pct);

/* Sérialise l'état en JSON (sans reset). Retourne la longueur, ou -1. */
int  stats_to_json(const stats_t *s, char *buf, size_t sz, uint64_t now);

/* Écrit le JSON dans `path` de façon atomique (tmp+rename) PUIS reset la
 * fenêtre. Crée au mieux le répertoire parent. Retourne 0 / -1. */
int  stats_write(stats_t *s, const char *path, uint64_t now);

#endif /* N2KMUX_STATS_H */
