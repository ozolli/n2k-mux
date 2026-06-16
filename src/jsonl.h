/*
 * jsonl.h — Parser de lignes JSON produites par `analyzer -json` (canboat).
 *
 * Ce n'est PAS un parser JSON généraliste. Il est taillé pour la structure
 * stable et plate de la sortie canboat : un objet par ligne, avec des clés
 * de premier niveau (src, pgn, prio, dst, timestamp, description) et un
 * sous-objet "fields" contenant des paires clé:valeur.
 *
 * Objectifs : zéro allocation dynamique par ligne (buffers fournis par
 * l'appelant), tolérance aux espaces et aux champs manquants, robustesse
 * face aux valeurs "name/value" ({"value":N,"name":"..."}).
 */

#ifndef N2KMUX_JSONL_H
#define N2KMUX_JSONL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Limites dimensionnées large pour le NMEA 2000 : aucun PGN courant
 * ne dépasse ~30 champs. Ajustables si besoin. */
#define JSONL_MAX_FIELDS      48
#define JSONL_KEY_LEN         48
#define JSONL_VAL_LEN         96
#define JSONL_DESC_LEN        96
#define JSONL_TS_LEN          32

/* Type de valeur d'un champ, tel que déduit du JSON. */
typedef enum {
    JSONL_NULL = 0,   /* champ absent / vide */
    JSONL_NUM,        /* nombre : la valeur est dans .num (et aussi .str brut) */
    JSONL_STR,        /* chaîne : la valeur est dans .str */
    JSONL_NV          /* objet {"value":N,"name":"S"} : .num = value, .str = name */
} jsonl_valtype_t;

/* Un champ du sous-objet "fields". */
typedef struct {
    char            key[JSONL_KEY_LEN];
    jsonl_valtype_t type;
    double          num;                 /* valeur numérique si NUM ou NV */
    char            str[JSONL_VAL_LEN];  /* texte (STR), nom (NV), ou nombre brut (NUM) */
} jsonl_field_t;

/* Un message décodé (une ligne JSON). */
typedef struct {
    bool     has_src, has_pgn, has_prio, has_dst;
    int      src;
    int      pgn;
    int      prio;
    int      dst;
    char     timestamp[JSONL_TS_LEN];
    char     description[JSONL_DESC_LEN];

    jsonl_field_t fields[JSONL_MAX_FIELDS];
    int      n_fields;

    bool     is_header;   /* true pour la ligne {"version":...} d'en-tête */
} jsonl_msg_t;

/*
 * Parse une ligne JSON dans `out`.
 * `line` n'a pas besoin d'être terminée par '\n' (toléré).
 * Retourne true si la ligne a été parsée (même partiellement et utilement),
 * false si la ligne est vide ou inexploitable.
 * `out` est entièrement réinitialisé à chaque appel.
 */
bool jsonl_parse(const char *line, jsonl_msg_t *out);

/* Recherche un champ par nom dans le message. NULL si absent. */
const jsonl_field_t *jsonl_get(const jsonl_msg_t *m, const char *key);

/* Helpers : récupèrent une valeur numérique / chaîne d'un champ donné.
 * Retournent false (et laissent *out inchangé) si le champ est absent
 * ou d'un type incompatible. */
bool jsonl_get_num(const jsonl_msg_t *m, const char *key, double *out);
bool jsonl_get_str(const jsonl_msg_t *m, const char *key, const char **out);

#endif /* N2KMUX_JSONL_H */
