/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * busmap.h — « Carte du bus » pour l'UI web (tableau d'arbitrage).
 *
 * Agrège ce que voit le daemon par UNITÉ D'ARBITRAGE (pgn [/discriminant]) :
 * la liste des sources qui l'émettent (doublons compris), avec pour chacune son
 * identité/nom résolus, sa vivacité (vue récemment) et le STATUT que lui donne
 * l'arbitre (retenue / supplantée / hors-règle…). Publié en JSON pour peupler le
 * futur tableau de sélection (entrée → sorties). Zéro allocation dynamique.
 *
 * Alimenté à chaque message décodé (busmap_observe, après arbiter_decide), publié
 * périodiquement (busmap_write). Distinct de sources.json (qui est src→PGN) : ici
 * c'est (pgn,disc)→sources, l'angle dont l'UI a besoin.
 */

#ifndef N2KMUX_BUSMAP_H
#define N2KMUX_BUSMAP_H

#include "jsonl.h"
#include "arbiter.h"
#include "config.h"
#include <stdint.h>
#include <stddef.h>

#define BUSMAP_MAX        256      /* (pgn,disc,src) distincts suivis */
#define BUSMAP_ALIVE_MS   6000u    /* vivant si vu depuis moins de */
#define BUSMAP_KEEP_MS    60000u   /* oublié (purgé) si non vu depuis plus de */

typedef struct {
    int      pgn;
    char     disc[CFG_DISC_LEN];     /* "" si pas de discriminant */
    int      src;
    char     ident[CFG_IDENT_LEN];   /* identité résolue ("" si inconnue) */
    char     name[CFG_NAME_LEN];     /* nom logique ("" si non configuré) */
    int      result;                 /* arb_result_t observé (dernier) */
    uint64_t last;                   /* dernière observation (ms) */
} bm_entry_t;

typedef struct {
    bm_entry_t e[BUSMAP_MAX];
    int        n;
} busmap_t;

/* Enregistre l'observation d'un message + sa décision d'arbitrage. */
void busmap_observe(busmap_t *bm, const jsonl_msg_t *m,
                    const arb_decision_t *d, uint64_t now);

/* Sérialise en JSON (groupé par pgn/disc). Retourne la longueur, ou -1. */
int  busmap_to_json(busmap_t *bm, char *buf, size_t sz, uint64_t now);

/* Écrit le JSON dans `path` de façon atomique (tmp+rename), purge les entrées
 * périmées. Crée au mieux le répertoire parent. Retourne 0 / -1. */
int  busmap_write(busmap_t *bm, const char *path, uint64_t now);

#endif /* N2KMUX_BUSMAP_H */
