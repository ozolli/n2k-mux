/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * config.h — Parser de configuration INI du multiplexeur.
 *
 * Module (d). Décrit, pour l'arbitre (module e), QUI choisir pour chaque
 * donnée. Trois informations, tirées des règles d'arbitrage :
 *   - sources nommées : nom logique (SCX, MAD, DST_BB, ...) → identité stable
 *     (Model Serial Code / Unique Number, telle que résolue par le registre) ;
 *   - priorités par PGN (éventuellement par discriminant) : liste ordonnée de
 *     noms logiques ;
 *   - mode de sélection : priority (1re source dispo), min (profondeur, par
 *     sécurité), max (loch : DST hors de l'eau sous-compte), fusion (AIS,
 *     dédup MMSI).
 *
 * Format INI accepté :
 *
 *   [output]
 *   talker = II
 *
 *   [sources]
 *   SCX = 4830123            ; nom logique = Model Serial Code
 *   VER = 917661
 *   DH  = 000A520AF6A0
 *
 *   [priority]
 *   ; clé = pgn  ou  pgn/discriminant   |   valeur = [mode:] liste de sources
 *   129025          = SCX, VER, MAD
 *   127250/Magnetic = SCX, MAD
 *   128267          = min: DST_BB, DST_TB
 *   130316/Sea      = DST_BB, DST_TB
 *   129038          = fusion: AIS, DH
 *
 *   [ignore]
 *   src = 0
 *   pgn = 262161, 262656
 *
 *   [rate]
 *   ; type de phrase = intervalle minimum en ms (0 ou absent = pas de limite)
 *   GLL = 1000              ; au plus une GLL par seconde
 *   GSV = 5000
 *
 * Commentaires : ';' ou '#' jusqu'à la fin de ligne. Espaces tolérés partout.
 * Sections/clés inconnues ignorées. Stockage fixe, zéro allocation dynamique.
 */

#ifndef N2KMUX_CONFIG_H
#define N2KMUX_CONFIG_H

#include <stdbool.h>

#define CFG_MAX_SOURCES   32
#define CFG_MAX_RULES     64
#define CFG_MAX_PRIO       8    /* sources par règle */
#define CFG_MAX_IGNORE    16
#define CFG_MAX_RATES     32   /* limites de débit par type de phrase */
#define CFG_NAME_LEN      24
#define CFG_IDENT_LEN     64
#define CFG_DISC_LEN      32
#define CFG_TYPE_LEN       8   /* type de phrase 0183 (ex. "GLL") */

/* Mode de sélection d'une règle. */
typedef enum {
    CFG_PICK_PRIORITY = 0, /* 1re source disponible dans l'ordre */
    CFG_PICK_MIN,          /* valeur minimale (profondeur : sécurité haut-fond) */
    CFG_PICK_MAX,          /* valeur maximale (loch : DST hors de l'eau sous-compte) */
    CFG_PICK_FUSION        /* fusion de plusieurs sources (AIS, dédup MMSI) */
} cfg_mode_t;

typedef struct {
    char name[CFG_NAME_LEN];   /* nom logique */
    char ident[CFG_IDENT_LEN]; /* identité stable (serial / unique) */
} cfg_source_t;

typedef struct {
    int        pgn;
    char       discriminant[CFG_DISC_LEN];          /* "" = s'applique à tout */
    cfg_mode_t mode;
    char       sources[CFG_MAX_PRIO][CFG_NAME_LEN]; /* noms, par priorité décroissante */
    int        n_sources;
} cfg_rule_t;

/* Limite de débit d'un type de phrase 0183 (throttle de la sortie). */
typedef struct {
    char type[CFG_TYPE_LEN];   /* type de phrase, ex. "GLL", "GSV" */
    int  min_interval_ms;      /* intervalle minimum entre émissions (0 = illimité) */
} cfg_rate_t;

typedef struct {
    char         talker[3];                 /* talker 0183 de sortie (défaut "II") */

    cfg_source_t sources[CFG_MAX_SOURCES];
    int          n_sources;

    cfg_rule_t   rules[CFG_MAX_RULES];
    int          n_rules;

    int          ignore_src[CFG_MAX_IGNORE];
    int          n_ignore_src;
    int          ignore_pgn[CFG_MAX_IGNORE];
    int          n_ignore_pgn;

    cfg_rate_t   rates[CFG_MAX_RATES];
    int          n_rates;

    char         err[160];  /* message de la dernière erreur de parsing */
    int          err_line;  /* ligne fautive (0 si aucune) */
} config_t;

/* Réinitialise (talker = "II", listes vides). */
void config_init(config_t *c);

/* Charge depuis un fichier. false sur erreur (voir c->err / c->err_line). */
bool config_load(config_t *c, const char *path);

/* Parse depuis une chaîne en mémoire (utile pour les tests). */
bool config_parse_string(config_t *c, const char *text);

/* Identité d'une source nommée, ou NULL si le nom est inconnu. */
const char *config_source_ident(const config_t *c, const char *name);

/* Nom logique d'une identité, ou NULL si l'identité n'est pas dans [sources]. */
const char *config_source_name(const config_t *c, const char *ident);

/* Règle pour (pgn, discriminant). `disc` peut être NULL/"".
 * Préfère la règle la plus spécifique (discriminant non vide qui préfixe
 * `disc`) avant la règle générique (discriminant vide). NULL si aucune. */
const cfg_rule_t *config_rule(const config_t *c, int pgn, const char *disc);

/* Règles d'exclusion explicites (listées dans [ignore]). */
bool config_ignore_src(const config_t *c, int src);
bool config_ignore_pgn(const config_t *c, int pgn);

/* Intervalle minimum (ms) entre deux phrases d'un type donné ([rate]).
 * 0 si le type n'est pas limité. La casse du type est ignorée. */
int config_rate_ms(const config_t *c, const char *type);

#endif /* N2KMUX_CONFIG_H */
