/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * registry.h — Table src → identité stable des équipements NMEA 2000.
 *
 * Module (b). Observe le flux décodé par jsonl (module a) et construit une
 * table associant chaque adresse source (src, 0..251) à une identité STABLE,
 * indépendante des réattributions d'adresse (ISO address claim).
 *
 * L'identité provient de deux PGN :
 *   - 60928  (ISO Address Claim)   → "Unique Number" + "Manufacturer Code"
 *   - 126996 (Product Information) → "Model Serial Code" + "Model ID"
 *
 * Cascade d'identité : Model Serial Code si connu, sinon Unique Number (en
 * décimal). C'est cette chaîne que la config INI (module d) mappe sur un nom
 * logique (SCX, MAD, DST_BB, ...), puis que l'arbitre (module e) utilise.
 *
 * Pourquoi "stable" : un device peut changer d'adresse (re-claim). Le Unique
 * Number (60928) et le Model Serial Code (126996) ne changent pas ; on suit
 * donc le device par son identité et on met à jour quelle adresse il occupe.
 *
 * Comme le reste du projet : zéro allocation dynamique, stockage fourni par
 * l'appelant (registry_t), tolérant aux champs et aux PGN manquants.
 */

#ifndef N2KMUX_REGISTRY_H
#define N2KMUX_REGISTRY_H

#include "jsonl.h"
#include <stdint.h>
#include <stdbool.h>

/* Le bus NMEA 2000 adresse 0..251 ; en pratique une poignée de devices. */
#define REG_MAX_DEVICES   64
#define REG_IDENT_LEN     64
#define REG_MODEL_LEN     48
#define REG_SERIAL_LEN    48
#define REG_MFG_LEN       48
#define REG_MAX_PGNS      40   /* PGN distincts suivis par device */

/* Un PGN observé pour un device, avec son compteur de messages. */
typedef struct {
    int           pgn;
    unsigned long count;
} reg_pgn_t;

/* Enregistrement détaché : connu par identité mais sans adresse courante
 * (device évincé d'une adresse reprise par un autre). */
#define N2K_ADDR_NULL     (-1)

/* Un équipement vu sur le bus. */
typedef struct {
    bool     used;

    int      src;                    /* adresse courante 0..251, ou N2K_ADDR_NULL */

    /* depuis 60928 (ISO Address Claim) */
    bool     has_unique;
    uint32_t unique;                 /* Unique Number (identifiant matériel) */
    bool     has_mfg;
    int      mfg_code;               /* Manufacturer Code (valeur numérique) */
    char     mfg[REG_MFG_LEN];       /* nom du fabricant si fourni (name/value) */

    /* depuis 126996 (Product Information) */
    char     model[REG_MODEL_LEN];   /* Model ID */
    char     serial[REG_SERIAL_LEN]; /* Model Serial Code */

    /* identité stable calculée : serial, sinon unique en décimal, sinon "" */
    char     ident[REG_IDENT_LEN];

    unsigned long seen;              /* nb de messages observés pour ce device */

    /* PGN publiés par ce device (liste + compteur), pour la GUI / le diagnostic. */
    reg_pgn_t     pgns[REG_MAX_PGNS];
    int           n_pgns;
} reg_device_t;

typedef struct {
    reg_device_t dev[REG_MAX_DEVICES];
    int          n;                  /* nb d'emplacements utilisés (jamais libérés) */
} registry_t;

/* Réinitialise la table. */
void registry_init(registry_t *r);

/*
 * Intègre un message décodé. Enrichit l'identité à partir des PGN 60928 et
 * 126996 ; pour tout autre PGN, se contente de créer/retrouver le device à
 * l'adresse src et d'incrémenter son compteur `seen` (utile pour lister les
 * sources vues, même sans identité encore connue).
 *
 * Retourne le device concerné (créé ou mis à jour), ou NULL si le message n'a
 * ni pgn ni src exploitable, ou si la table est pleine.
 *
 * Note : ne filtre pas src=0 ni les PGN de contrôle 262xxx — c'est le rôle de
 * l'appelant (arbitre / daemon) selon les règles d'arbitrage.
 */
const reg_device_t *registry_observe(registry_t *r, const jsonl_msg_t *m);

/* Device actuellement à l'adresse src, ou NULL. */
const reg_device_t *registry_by_src(const registry_t *r, int src);

/* Device par identité stable (serial ou unique en décimal), ou NULL. */
const reg_device_t *registry_by_ident(const registry_t *r, const char *ident);

/* Identité stable de l'adresse src, ou NULL si inconnue / pas encore résolue. */
const char *registry_ident(const registry_t *r, int src);

#endif /* N2KMUX_REGISTRY_H */
