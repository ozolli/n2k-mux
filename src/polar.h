/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * polar.h — Polaire de vitesse (format qtVlm .pol / .csv) + interpolation.
 *
 * Format lu : une table à deux entrées, première ligne = en-tête des vitesses
 * de vent vrai, puis une ligne par angle de vent vrai :
 *
 *     TWA\TWS;0;4;6;8;10;...          (ou TWA/TWS, twa/tws)
 *     0;0.00;0.00;0.00;...
 *     45;0.00;3.10;4.80;6.50;...      (vitesse surface, en nœuds)
 *
 * Variantes rencontrées dans les polaires réelles, toutes acceptées :
 *   - séparateur « ; » ou tabulation (virgule seulement en dernier recours) ;
 *   - décimale « . » ou « , » (la virgule n'est alors pas un séparateur) ;
 *   - fins de ligne CRLF, BOM UTF-8, cellules vides en fin de ligne ;
 *   - colonnes TWS en double (même vitesse de vent répétée).
 *
 * Refusé : tout fichier dont la première cellule ne commence pas par « TWA ».
 * C'est ce qui écarte les tables de correction de vagues (*.polwave.csv, en-tête
 * « TWS=… ») rangées dans le même dossier que les polaires.
 *
 * Interpolation BILINÉAIRE en (TWA, TWS). La polaire est symétrique : un TWA
 * au-delà de 180° est replié (200° = 160°). Hors de la table, les valeurs sont
 * BORNÉES aux extrêmes, jamais extrapolées : au-delà de la dernière colonne de
 * vent, la dernière vitesse fait foi (souvent 0 dans la tempête, ce qui est le
 * choix prudent).
 *
 * Zéro allocation dynamique : l'appelant fournit le polar_t.
 */

#ifndef N2KMUX_POLAR_H
#define N2KMUX_POLAR_H

#include <stdbool.h>
#include <stddef.h>

#define POLAR_MAX_TWS   64    /* colonnes de vent */
#define POLAR_MAX_TWA  256    /* lignes d'angle (1° de 0 à 180 tient large) */

typedef struct {
    int    n_tws, n_twa;
    double tws[POLAR_MAX_TWS];                     /* nœuds, non décroissant */
    double twa[POLAR_MAX_TWA];                     /* degrés, non décroissant */
    float  spd[POLAR_MAX_TWA][POLAR_MAX_TWS];      /* vitesse surface, nœuds */
    double max_speed;                              /* plus grande vitesse lue */
    char   err[160];                               /* raison d'un refus */
} polar_t;

/* Parse une polaire depuis un texte. false (et p->err) si ce n'est pas une
 * polaire de vitesse exploitable. */
bool polar_parse_string(polar_t *p, const char *text);

/* Idem depuis un fichier (lu en entier, 512 Ko au plus). */
bool polar_load(polar_t *p, const char *path);

/* Vitesse surface (nœuds) pour un angle et une vitesse de vent vrai. */
double polar_speed(const polar_t *p, double twa_deg, double tws_kn);

/* true si le nom se termine par .pol ou .csv (casse ignorée) et n'est pas une
 * table de vagues *.polwave.csv. Filtre de NOM seulement : le contenu reste à
 * valider par polar_load. */
bool polar_name_ok(const char *name);

#endif /* N2KMUX_POLAR_H */
