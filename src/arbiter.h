/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * arbiter.h — Cœur d'arbitrage : choix de la source retenue.
 *
 * Module (e), 1re passe : SÉLECTION uniquement. Pour chaque message décodé,
 * décide s'il doit être retenu pour la sortie, en fonction de la config
 * (priorités, modes) et du registre (src → identité → nom logique).
 * La génération des phrases NMEA 0183 (mapping des champs PGN) viendra ensuite.
 *
 * Clé d'arbitrage : (pgn, src, discriminant).
 *   - discriminant : valeur d'un champ dont le NOM dépend du PGN. La table
 *     pgn→champ est intrinsèque et codée ici (130306/127250 → "Reference",
 *     130312 → "Temperature Source"/"Source"). Les VALEURS attendues sont dans
 *     la config (clés "pgn/discriminant").
 *   - mode de la règle (config) :
 *       priority : une seule source retenue = la plus prioritaire VIVANTE ;
 *       min/fusion : toutes les sources listées et vivantes sont retenues
 *                    (la combinaison min / la dédup MMSI se fera au mapping).
 *
 * Vivacité (failover) : une source est « vivante » si vue depuis moins de
 * timeout_ms. Si la source prioritaire se tait, la suivante prend le relais.
 * L'horloge (now_ms, monotone) est fournie par l'appelant (testable).
 *
 * Exclusions appliquées d'office : src <= 0 et PGN >= 262144 (messages de
 * contrôle/proprio Actisense/CANboat), en plus de la liste [ignore] de la config.
 *
 * Prérequis : l'appelant tient le registre à jour (registry_observe) AVANT
 * d'appeler arbiter_decide, pour que src → identité soit résolu.
 */

#ifndef N2KMUX_ARBITER_H
#define N2KMUX_ARBITER_H

#include "jsonl.h"
#include "registry.h"
#include "config.h"
#include <stdint.h>
#include <stdbool.h>

#define ARB_DEFAULT_TIMEOUT_MS  5000u

typedef enum {
    ARB_ACCEPT = 0,           /* message retenu (à convertir) */
    ARB_REJECT_PRIORITY,      /* supplanté par une source plus prioritaire vivante */
    ARB_REJECT_NO_RULE,       /* aucune règle pour ce (pgn, discriminant) */
    ARB_REJECT_UNCONFIGURED,  /* src résolu mais identité absente de [sources] */
    ARB_REJECT_NOT_IN_RULE,   /* source configurée mais hors liste de cette règle */
    ARB_REJECT_UNKNOWN_SRC,   /* identité pas encore résolue par le registre */
    ARB_IGNORED               /* exclu (src<=0 / PGN>=262144 / [ignore]) */
} arb_result_t;

typedef struct {
    arb_result_t      result;
    const cfg_rule_t *rule;          /* règle appliquée, ou NULL */
    cfg_mode_t        mode;          /* mode de la règle (si rule) */
    const char       *identity;      /* identité résolue (registre), ou NULL */
    const char       *source_name;   /* nom logique résolu (config), ou NULL */
    char              discriminant[CFG_DISC_LEN]; /* valeur observée ("" si aucun) */
} arb_decision_t;

typedef struct {
    const config_t   *cfg;
    const registry_t *reg;
    unsigned          timeout_ms;
    /* vivacité par (règle, slot de source) */
    uint64_t          last_seen[CFG_MAX_RULES][CFG_MAX_PRIO];
    bool              seen[CFG_MAX_RULES][CFG_MAX_PRIO];
} arbiter_t;

/* Initialise. cfg et reg doivent rester valides pendant toute la durée de vie. */
void arbiter_init(arbiter_t *a, const config_t *cfg, const registry_t *reg);

/* Ajuste le délai de vivacité (failover). 0 = valeur par défaut. */
void arbiter_set_timeout(arbiter_t *a, unsigned timeout_ms);

/* Décide du sort d'un message. Met à jour la vivacité interne. */
arb_decision_t arbiter_decide(arbiter_t *a, const jsonl_msg_t *m, uint64_t now_ms);

/* Source actuellement retenue pour une règle priority (nom logique), ou NULL
 * (pas de règle, mode non-priority, ou aucune source vivante). */
const char *arbiter_current_source(const arbiter_t *a, int pgn, const char *disc,
                                   uint64_t now_ms);

/* Libellé lisible d'un résultat (pour logs/tests). */
const char *arb_result_str(arb_result_t r);

#endif /* N2KMUX_ARBITER_H */
