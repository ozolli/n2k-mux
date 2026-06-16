/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * jsonl.c — Implémentation du parser de lignes JSON canboat.
 *
 * Approche : un seul passage de gauche à droite. On ne construit pas d'arbre ;
 * on extrait les clés de premier niveau qui nous intéressent et on aplatit
 * le sous-objet "fields". Le parsing est tolérant : tout ce qui n'est pas
 * reconnu est ignoré sans faire échouer la ligne entière.
 */

#include "jsonl.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* Avance p au-delà des espaces. */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/*
 * Lit une chaîne JSON entre guillemets. p doit pointer sur le '"' ouvrant.
 * Copie le contenu déséchappé dans dst (taille dstsz, toujours terminé NUL).
 * Retourne le pointeur juste après le '"' fermant, ou NULL si malformé.
 */
static const char *parse_string(const char *p, char *dst, size_t dstsz)
{
    if (*p != '"')
        return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                /* \uXXXX : on ne gère pas l'unicode complet ici ; on saute
                 * les 4 hex et on insère '?' (les champs canboat qui nous
                 * intéressent sont ASCII). */
                case 'u':
                    if (p[1] && p[2] && p[3] && p[4]) p += 4;
                    c = '?';
                    break;
                default:   c = *p;   break;
            }
        }
        if (i + 1 < dstsz)
            dst[i++] = c;
        p++;
    }
    if (*p != '"')
        return NULL;       /* chaîne non terminée */
    dst[i] = '\0';
    return p + 1;
}

/*
 * Saute une valeur JSON quelconque (nombre, chaîne, objet, tableau, bool, null)
 * sans l'interpréter. Sert à passer par-dessus ce qu'on ne veut pas lire.
 * Retourne le pointeur juste après la valeur, ou NULL si malformé.
 */
static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') {
        /* chaîne : sauter en gérant les échappements */
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        return (*p == '"') ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        while (*p) {
            if (in_str) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == '"') in_str = false;
            } else {
                if (*p == '"') in_str = true;
                else if (*p == open)  depth++;
                else if (*p == close) { depth--; if (depth == 0) return p + 1; }
            }
            p++;
        }
        return NULL;       /* objet/tableau non terminé */
    }
    /* nombre, true, false, null : avancer jusqu'au séparateur */
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

/*
 * Lit une valeur de champ (dans "fields") et remplit f->type/num/str.
 * Gère : nombre, chaîne, objet {"value":N,"name":"S"} (name/value).
 * Les tableaux/autres objets sont stockés comme STR brute tronquée.
 * Retourne le pointeur après la valeur, ou NULL si malformé.
 */
static const char *parse_field_value(const char *p, jsonl_field_t *f)
{
    p = skip_ws(p);

    if (*p == '"') {
        f->type = JSONL_STR;
        const char *q = parse_string(p, f->str, sizeof f->str);
        return q;
    }

    if (*p == '{') {
        /* Cas name/value : on cherche "value" (num) et "name" (str). */
        f->type = JSONL_NV;
        f->num  = 0;
        f->str[0] = '\0';
        const char *q = p + 1;
        bool got_value = false, got_name = false;
        while (*q) {
            q = skip_ws(q);
            if (*q == '}') { q++; break; }
            if (*q == ',') { q++; continue; }
            if (*q != '"') { q = skip_value(q); if (!q) return NULL; continue; }
            char k[24];
            q = parse_string(q, k, sizeof k);
            if (!q) return NULL;
            q = skip_ws(q);
            if (*q != ':') return NULL;
            q++;
            q = skip_ws(q);
            if (strcmp(k, "value") == 0) {
                f->num = strtod(q, NULL);
                got_value = true;
                q = skip_value(q);
            } else if (strcmp(k, "name") == 0) {
                q = parse_string(q, f->str, sizeof f->str);
                got_name = true;
            } else {
                q = skip_value(q);
            }
            if (!q) return NULL;
        }
        /* Si pas de structure name/value reconnue, dégrader proprement. */
        if (!got_value && !got_name) {
            f->type = JSONL_NULL;
        }
        return q;
    }

    if (*p == '[') {
        /* tableau : non interprété, on le saute et on marque NULL. */
        f->type = JSONL_NULL;
        return skip_value(p);
    }

    /* nombre / true / false / null */
    {
        const char *start = p;
        const char *end = p;
        while (*end && *end != ',' && *end != '}' && *end != ']' &&
               *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
            end++;
        size_t len = (size_t)(end - start);
        if (len == 0) { f->type = JSONL_NULL; return end; }

        if ((len == 4 && strncmp(start, "null", 4) == 0)) {
            f->type = JSONL_NULL;
        } else if ((len == 4 && strncmp(start, "true", 4) == 0)) {
            f->type = JSONL_NUM; f->num = 1;
            snprintf(f->str, sizeof f->str, "true");
        } else if ((len == 5 && strncmp(start, "false", 5) == 0)) {
            f->type = JSONL_NUM; f->num = 0;
            snprintf(f->str, sizeof f->str, "false");
        } else {
            /* nombre */
            f->type = JSONL_NUM;
            f->num  = strtod(start, NULL);
            size_t n = len < sizeof f->str - 1 ? len : sizeof f->str - 1;
            memcpy(f->str, start, n);
            f->str[n] = '\0';
        }
        return end;
    }
}

/* Parse le sous-objet "fields". p pointe sur le '{' ouvrant. */
static const char *parse_fields(const char *p, jsonl_msg_t *out)
{
    if (*p != '{')
        return NULL;
    p++;
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') { p++; break; }
        if (*p == ',') { p++; continue; }
        if (*p != '"') { p = skip_value(p); if (!p) return NULL; continue; }

        jsonl_field_t tmp;
        memset(&tmp, 0, sizeof tmp);
        p = parse_string(p, tmp.key, sizeof tmp.key);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = parse_field_value(p, &tmp);
        if (!p) return NULL;

        if (out->n_fields < JSONL_MAX_FIELDS)
            out->fields[out->n_fields++] = tmp;
    }
    return p;
}

bool jsonl_parse(const char *line, jsonl_msg_t *out)
{
    if (!line || !out)
        return false;

    memset(out, 0, sizeof *out);
    out->src = out->pgn = out->prio = out->dst = -1;

    const char *p = skip_ws(line);
    if (*p != '{')
        return false;          /* pas un objet JSON */
    p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }
        if (*p != '"') { p = skip_value(p); if (!p) return false; continue; }

        char key[JSONL_KEY_LEN];
        p = parse_string(p, key, sizeof key);
        if (!p) return false;
        p = skip_ws(p);
        if (*p != ':') return false;
        p++;
        p = skip_ws(p);

        if (strcmp(key, "src") == 0) {
            out->src = (int)strtol(p, NULL, 10); out->has_src = true;
            p = skip_value(p);
        } else if (strcmp(key, "pgn") == 0) {
            out->pgn = (int)strtol(p, NULL, 10); out->has_pgn = true;
            p = skip_value(p);
        } else if (strcmp(key, "prio") == 0) {
            out->prio = (int)strtol(p, NULL, 10); out->has_prio = true;
            p = skip_value(p);
        } else if (strcmp(key, "dst") == 0) {
            out->dst = (int)strtol(p, NULL, 10); out->has_dst = true;
            p = skip_value(p);
        } else if (strcmp(key, "timestamp") == 0) {
            p = parse_string(p, out->timestamp, sizeof out->timestamp);
        } else if (strcmp(key, "description") == 0) {
            p = parse_string(p, out->description, sizeof out->description);
        } else if (strcmp(key, "fields") == 0) {
            p = parse_fields(p, out);
        } else if (strcmp(key, "version") == 0) {
            out->is_header = true;
            p = skip_value(p);
        } else {
            p = skip_value(p);
        }
        if (!p) return false;
    }

    /* Une ligne utile a soit un pgn, soit c'est l'en-tête version. */
    return out->is_header || out->has_pgn;
}

const jsonl_field_t *jsonl_get(const jsonl_msg_t *m, const char *key)
{
    if (!m || !key)
        return NULL;
    for (int i = 0; i < m->n_fields; i++)
        if (strcmp(m->fields[i].key, key) == 0)
            return &m->fields[i];
    return NULL;
}

bool jsonl_get_num(const jsonl_msg_t *m, const char *key, double *out)
{
    const jsonl_field_t *f = jsonl_get(m, key);
    if (!f) return false;
    if (f->type == JSONL_NUM || f->type == JSONL_NV) {
        if (out) *out = f->num;
        return true;
    }
    return false;
}

bool jsonl_get_str(const jsonl_msg_t *m, const char *key, const char **out)
{
    const jsonl_field_t *f = jsonl_get(m, key);
    if (!f) return false;
    if (f->type == JSONL_STR || f->type == JSONL_NV) {
        if (out) *out = f->str;
        return true;
    }
    return false;
}
