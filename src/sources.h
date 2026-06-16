/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * sources.h — Publication des sources vues (pont daemon → GUI).
 *
 * Module (g, plomberie). Le daemon sérialise périodiquement le registre dans
 * un fichier JSON (par défaut /run/n2k-mux/sources.json) ; la GUI le relit pour
 * afficher la liste des équipements vus sur le bus (adresse, identité, modèle,
 * compteur). Format : un tableau JSON, un objet par ligne (facile à relire).
 *
 * Logique testable sans affichage (testeur ./test_sources). Zéro alloc.
 */

#ifndef N2KMUX_SOURCES_H
#define N2KMUX_SOURCES_H

#include "registry.h"
#include <stddef.h>

#define SRC_VIEW_MAX  REG_MAX_DEVICES

#define SRC_MAX_PGNS  REG_MAX_PGNS

/* Une source telle que relue depuis le JSON (pour la GUI). */
typedef struct {
    int           src;
    char          ident[REG_IDENT_LEN];
    char          mfg[REG_MFG_LEN];
    char          model[REG_MODEL_LEN];
    char          serial[REG_SERIAL_LEN];
    unsigned long seen;
    int           pgns[SRC_MAX_PGNS];        /* PGN publiés par ce device */
    unsigned long pgn_count[SRC_MAX_PGNS];
    int           n_pgns;
} src_entry_t;

typedef struct {
    src_entry_t e[SRC_VIEW_MAX];
    int         n;
} sources_view_t;

/* Sérialise le registre en JSON dans buf. Retourne la longueur, ou -1. */
int sources_to_json(const registry_t *r, char *buf, size_t sz);

/* Écrit le JSON dans `path` de façon atomique (tmp + rename). Crée au mieux le
 * répertoire parent. Retourne 0 si OK, -1 sinon. */
int sources_write(const registry_t *r, const char *path);

/* Relit un fichier sources.json dans `v`. Retourne le nombre d'entrées, ou -1. */
int sources_load(const char *path, sources_view_t *v);

#endif /* N2KMUX_SOURCES_H */
