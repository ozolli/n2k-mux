/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * aisdedup.h — Déduplication/fusion AIS par MMSI (mode --ais-json du daemon).
 *
 * Deux récepteurs AIS (em-trak + DataHub) voient en partie les MÊMES navires
 * (même MMSI) → sans filtrage, chaque cible apparaîtrait en double dans qtVlm.
 * Ce module, en amont de n2kd (qui encode le VDM), ne laisse passer qu'UNE
 * source par MMSI : la plus prioritaire (ordre de la règle [priority] en mode
 * fusion, ex. "129039 = fusion: AIS, DH" → em-trak avant DataHub) parmi celles
 * vues récemment. Une source vue seule fournit donc ses cibles (union) ; quand
 * deux sources voient une cible, la prioritaire gagne ; si elle se tait au-delà
 * du timeout, l'autre prend le relais.
 *
 * Travaille au niveau JSON (le MMSI = champ "User ID" est en clair), pas au
 * niveau VDM. Stockage fixe, zéro allocation dynamique.
 */

#ifndef N2KMUX_AISDEDUP_H
#define N2KMUX_AISDEDUP_H

#include "config.h"
#include "registry.h"
#include <stdint.h>
#include <stdbool.h>

#define AIS_MAX_OBS         2048      /* (MMSI, src) suivis simultanément */
#define AIS_DEFAULT_TIMEOUT 600000u   /* 10 min : > cadence statique Classe B */

typedef struct {
    uint32_t mmsi;
    int      src;
    uint64_t t;       /* dernier passage (ms) */
    bool     used;
} ais_obs_t;

typedef struct {
    const config_t   *cfg;
    const registry_t *reg;
    unsigned          timeout_ms;
    ais_obs_t         obs[AIS_MAX_OBS];
} aisdedup_t;

void aisdedup_init(aisdedup_t *a, const config_t *cfg, const registry_t *reg);

/* Ajuste le délai de bascule (0 = défaut). */
void aisdedup_set_timeout(aisdedup_t *a, unsigned timeout_ms);

/* true si ce PGN est un message AIS (à dédupliquer). */
bool aisdedup_is_ais(int pgn);

/*
 * Enregistre (mmsi, src) à l'instant now et retourne true si cette source est
 * la source RETENUE pour ce MMSI (donc à transmettre), false sinon.
 */
bool aisdedup_accept(aisdedup_t *a, int pgn, uint32_t mmsi, int src, uint64_t now);

#endif /* N2KMUX_AISDEDUP_H */
