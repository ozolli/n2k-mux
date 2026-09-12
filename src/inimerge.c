/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * inimerge.c — Implémentation de la fusion INI préservant les commentaires.
 *
 * Deux passes : on indexe d'abord les couples (section, clé) → valeur du nouveau
 * contenu, puis on recopie l'ancien fichier ligne à ligne en substituant les
 * valeurs. Les clés du nouveau contenu non consommées sont ajoutées à la fin de
 * leur section (ou dans une section neuve, en fin de fichier).
 */

#include "inimerge.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char sec[INIM_SEC_LEN];     /* section canonique */
    char key[INIM_KEY_LEN];
    char val[INIM_VAL_LEN];
    bool done;                  /* déjà réécrite dans l'ancien squelette */
} entry_t;

/* --- petits utilitaires (aucune allocation) --- */

static void cpy(char *dst, size_t sz, const char *src, size_t n)
{
    if (!sz) return;
    size_t i = 0;
    for (; i < n && src[i] && i + 1 < sz; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Rogne les espaces de tête et de queue de [b, e) et copie dans dst. */
static void cpy_trim(char *dst, size_t sz, const char *b, const char *e)
{
    while (b < e && (*b == ' ' || *b == '\t')) b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    cpy(dst, sz, b, (size_t)(e - b));
}

/* Alias de section du parser ramenés à un nom canonique (cf. config.c). */
static void canon_section(char *s)
{
    for (char *p = s; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    if (strcmp(s, "priorities") == 0 || strcmp(s, "priorites") == 0)
        snprintf(s, INIM_SEC_LEN, "priority");
    else if (strcmp(s, "rates") == 0)     snprintf(s, INIM_SEC_LEN, "rate");
    else if (strcmp(s, "talkers") == 0)   snprintf(s, INIM_SEC_LEN, "talker");
    else if (strcmp(s, "sentences") == 0) snprintf(s, INIM_SEC_LEN, "sentence");
}

/* Début et fin (exclue) de la ligne courante ; avance *p sur la suivante. */
static const char *line_end(const char *p)
{
    const char *e = p;
    while (*e && *e != '\n') e++;
    return e;
}

/* Position du commentaire dans [b, e), ou NULL. */
static const char *comment_at(const char *b, const char *e)
{
    for (const char *p = b; p < e; p++)
        if (*p == ';' || *p == '#') return p;
    return NULL;
}

/* Ligne de section « [nom] » ? Si oui, remplit sec (canonique). */
static bool parse_section(const char *b, const char *e, char *sec, size_t secsz)
{
    while (b < e && (*b == ' ' || *b == '\t')) b++;
    if (b >= e || *b != '[') return false;
    const char *close = b + 1;
    while (close < e && *close != ']') close++;
    if (close >= e) return false;
    cpy_trim(sec, secsz, b + 1, close);
    canon_section(sec);
    return true;
}

/* Ligne « clé = valeur » ? Si oui, donne la clé rognée et la position du '='. */
static bool parse_kv(const char *b, const char *e, char *key, size_t keysz,
                     const char **eq)
{
    const char *cm = comment_at(b, e);
    const char *lim = cm ? cm : e;
    const char *p = b;
    while (p < lim && *p != '=') p++;
    if (p >= lim) return false;
    cpy_trim(key, keysz, b, p);
    if (!key[0]) return false;
    *eq = p;
    return true;
}

/* --- écriture bornée --- */

typedef struct { char *buf; size_t cap, len; bool ok; } out_t;

static void emit(out_t *o, const char *s, size_t n)
{
    if (!o->ok) return;
    if (o->len + n + 1 > o->cap) { o->ok = false; return; }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void emit_str(out_t *o, const char *s) { emit(o, s, strlen(s)); }

/* --- indexation du nouveau contenu --- */

static int index_new(const char *text, entry_t *e, int max,
                     char secs[][INIM_SEC_LEN], int *n_secs, int max_secs)
{
    int n = 0;
    char cur[INIM_SEC_LEN] = "";
    for (const char *p = text; *p; ) {
        const char *le = line_end(p);
        char sec[INIM_SEC_LEN];
        if (parse_section(p, le, sec, sizeof sec)) {
            snprintf(cur, sizeof cur, "%s", sec);
            bool known = false;
            for (int i = 0; i < *n_secs; i++)
                if (strcmp(secs[i], cur) == 0) { known = true; break; }
            if (!known) {
                if (*n_secs >= max_secs) return -1;
                snprintf(secs[(*n_secs)++], INIM_SEC_LEN, "%s", cur);
            }
        } else {
            char key[INIM_KEY_LEN];
            const char *eq;
            if (parse_kv(p, le, key, sizeof key, &eq)) {
                if (n >= max) return -1;
                const char *cm = comment_at(p, le);
                snprintf(e[n].sec, sizeof e[n].sec, "%s", cur);
                snprintf(e[n].key, sizeof e[n].key, "%s", key);
                cpy_trim(e[n].val, sizeof e[n].val, eq + 1, cm ? cm : le);
                e[n].done = false;
                n++;
            }
        }
        p = *le ? le + 1 : le;
    }
    return n;
}

/* Ajoute les clés d'une section restées non consommées. */
static void flush_section(out_t *o, entry_t *e, int n, const char *sec)
{
    for (int i = 0; i < n; i++) {
        if (e[i].done || strcmp(e[i].sec, sec) != 0) continue;
        emit_str(o, e[i].key);
        emit_str(o, " = ");
        emit_str(o, e[i].val);
        emit_str(o, "\n");
        e[i].done = true;
    }
}

int ini_merge(const char *old_text, const char *new_text,
              char *out, size_t outsz)
{
    if (!old_text || !new_text || !out || outsz == 0)
        return -1;

    static entry_t ents[INIM_MAX_ENTRIES];
    static char    secs[INIM_MAX_SECTIONS][INIM_SEC_LEN];
    int n_secs = 0;
    int n = index_new(new_text, ents, INIM_MAX_ENTRIES, secs, &n_secs,
                      INIM_MAX_SECTIONS);
    if (n < 0)
        return -1;

    out_t o = { out, outsz, 0, true };
    o.buf[0] = '\0';

    char cur[INIM_SEC_LEN] = "";
    bool seen_sec[INIM_MAX_SECTIONS] = { false };

    for (const char *p = old_text; *p; ) {
        const char *le = line_end(p);
        size_t      ln = (size_t)(le - p);
        char        sec[INIM_SEC_LEN];

        if (parse_section(p, le, sec, sizeof sec)) {
            /* On quitte une section : y placer ses clés nouvelles. */
            if (cur[0]) flush_section(&o, ents, n, cur);
            snprintf(cur, sizeof cur, "%s", sec);
            for (int i = 0; i < n_secs; i++)
                if (strcmp(secs[i], cur) == 0) seen_sec[i] = true;
            emit(&o, p, ln);
            emit_str(&o, "\n");
        } else {
            char key[INIM_KEY_LEN];
            const char *eq;
            if (parse_kv(p, le, key, sizeof key, &eq)) {
                int found = -1;
                for (int i = 0; i < n; i++)
                    if (!ents[i].done && strcmp(ents[i].sec, cur) == 0 &&
                        strcmp(ents[i].key, key) == 0) { found = i; break; }
                if (found >= 0) {
                    /* Tout ce qui précède le '=' est conservé tel quel :
                     * orthographe de la clé et alignement des colonnes. */
                    emit(&o, p, (size_t)(eq - p) + 1);
                    emit_str(&o, " ");
                    emit_str(&o, ents[found].val);
                    const char *cm = comment_at(p, le);
                    if (cm) {
                        /* On remet le commentaire dans SA colonne : la largeur
                         * occupée par l'ancienne valeur est reproduite quand la
                         * nouvelle est plus courte. Sinon deux espaces. Sans ça,
                         * chaque enregistrement décalait tous les commentaires
                         * de fin de ligne du fichier. */
                        size_t oldw = (size_t)(cm - (eq + 1));
                        size_t neww = 1 + strlen(ents[found].val);
                        size_t pad  = (oldw > neww) ? oldw - neww : 2;
                        for (size_t i = 0; i < pad; i++)
                            emit_str(&o, " ");
                        emit(&o, cm, (size_t)(le - cm));
                    }
                    emit_str(&o, "\n");
                    ents[found].done = true;
                }
                /* clé absente du nouveau contenu : ligne supprimée */
            } else {
                emit(&o, p, ln);          /* commentaire, ligne vide, inconnu */
                emit_str(&o, "\n");
            }
        }
        p = *le ? le + 1 : le;
    }

    /* Fin de fichier : clés nouvelles de la dernière section, puis sections
     * entièrement nouvelles. */
    if (cur[0]) flush_section(&o, ents, n, cur);
    for (int i = 0; i < n_secs; i++) {
        if (seen_sec[i]) continue;
        bool any = false;
        for (int j = 0; j < n; j++)
            if (!ents[j].done && strcmp(ents[j].sec, secs[i]) == 0) { any = true; break; }
        if (!any) continue;
        emit_str(&o, "\n[");
        emit_str(&o, secs[i]);
        emit_str(&o, "]\n");
        flush_section(&o, ents, n, secs[i]);
    }

    return o.ok ? (int)o.len : -1;
}
