/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * polar.c — Lecture des polaires qtVlm et interpolation bilinéaire.
 */

#include "polar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool fail(polar_t *p, const char *msg)
{
    snprintf(p->err, sizeof p->err, "%s", msg);
    return false;
}

/* Lit un nombre dans [b, e) : espaces ignorés, virgule décimale acceptée.
 * Retourne false si la cellule est vide ou non numérique. */
static bool cell_num(const char *b, const char *e, double *out)
{
    char buf[48];
    size_t n = 0;
    for (const char *q = b; q < e && n + 1 < sizeof buf; q++) {
        if (*q == ' ' || *q == '\t' || *q == '\r')
            continue;
        buf[n++] = (*q == ',') ? '.' : *q;
    }
    buf[n] = '\0';
    if (!n)
        return false;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (!end || *end != '\0')
        return false;
    *out = v;
    return true;
}

/* Découpe la ligne [b, e) sur `sep` et appelle `fn` pour chaque cellule. */
typedef bool (*cell_fn)(void *ctx, int idx, const char *b, const char *e);

static bool split(const char *b, const char *e, char sep, cell_fn fn, void *ctx)
{
    int idx = 0;
    const char *c = b;
    for (const char *q = b; ; q++) {
        if (q == e || *q == sep) {
            if (!fn(ctx, idx++, c, q))
                return false;
            if (q == e)
                break;
            c = q + 1;
        }
    }
    return true;
}

/* --- en-tête : TWA\TWS puis les vitesses de vent --- */
typedef struct { polar_t *p; } hdr_ctx;

static bool hdr_cell(void *vctx, int idx, const char *b, const char *e)
{
    polar_t *p = ((hdr_ctx *)vctx)->p;
    if (idx == 0)
        return true;                    /* « TWA\TWS », déjà vérifié */
    double v;
    if (!cell_num(b, e, &v))
        return true;                    /* cellule vide de fin de ligne */
    if (p->n_tws >= POLAR_MAX_TWS)
        return fail(p, "trop de colonnes de vent");
    if (p->n_tws > 0 && v < p->tws[p->n_tws - 1])
        return fail(p, "vitesses de vent non croissantes dans l'en-tête");
    p->tws[p->n_tws++] = v;
    return true;
}

/* --- ligne de données : TWA puis une vitesse par colonne --- */
typedef struct { polar_t *p; int row; bool has_twa; } row_ctx;

static bool row_cell(void *vctx, int idx, const char *b, const char *e)
{
    row_ctx *rc = vctx;
    polar_t *p = rc->p;
    double v;
    if (idx == 0) {
        if (!cell_num(b, e, &v))
            return true;                /* ligne vide ou commentaire : ignorée */
        rc->has_twa = true;
        p->twa[rc->row] = v;
        return true;
    }
    if (!rc->has_twa || idx > p->n_tws)
        return true;                    /* cellules en trop : ignorées */
    /* cellule vide ou illisible : vitesse 0 (la table est déjà remise à zéro) */
    if (cell_num(b, e, &v)) {
        p->spd[rc->row][idx - 1] = (float)v;
        if (v > p->max_speed)
            p->max_speed = v;
    }
    return true;
}

bool polar_parse_string(polar_t *p, const char *text)
{
    memset(p, 0, sizeof *p);
    if (!text)
        return fail(p, "texte absent");

    const char *s = text;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s += 3;                         /* BOM UTF-8 */

    /* 1re ligne non vide = en-tête */
    while (*s == '\r' || *s == '\n' || *s == ' ' || *s == '\t')
        s++;
    const char *he = s;
    while (*he && *he != '\n')
        he++;

    if (strncasecmp(s, "TWA", 3) != 0)
        return fail(p, "pas une polaire de vitesse (en-tête TWA\\TWS attendu)");

    /* Séparateur : « ; » d'abord (la virgule peut alors être décimale), puis
     * tabulation, puis virgule en dernier recours. */
    char sep = 0;
    for (const char *q = s; q < he; q++) if (*q == ';')  { sep = ';';  break; }
    if (!sep) for (const char *q = s; q < he; q++) if (*q == '\t') { sep = '\t'; break; }
    if (!sep) for (const char *q = s; q < he; q++) if (*q == ',')  { sep = ',';  break; }
    if (!sep)
        return fail(p, "séparateur introuvable (« ; », tabulation ou « , »)");

    hdr_ctx hc = { p };
    if (!split(s, he, sep, hdr_cell, &hc))
        return false;
    if (p->n_tws < 1)
        return fail(p, "aucune vitesse de vent dans l'en-tête");

    const char *line = *he ? he + 1 : he;
    while (*line) {
        const char *le = line;
        while (*le && *le != '\n')
            le++;
        if (p->n_twa >= POLAR_MAX_TWA)
            return fail(p, "trop de lignes d'angle");
        row_ctx rc = { p, p->n_twa, false };
        if (!split(line, le, sep, row_cell, &rc))
            return false;
        if (rc.has_twa) {
            if (p->n_twa > 0 && p->twa[p->n_twa] < p->twa[p->n_twa - 1])
                return fail(p, "angles de vent non croissants");
            p->n_twa++;
        }
        line = *le ? le + 1 : le;
    }
    if (p->n_twa < 1)
        return fail(p, "aucune ligne d'angle");
    return true;
}

bool polar_load(polar_t *p, const char *path)
{
    static char buf[512 * 1024];
    FILE *f = fopen(path, "rb");
    if (!f) {
        memset(p, 0, sizeof *p);
        return fail(p, "ouverture impossible");
    }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    int too_big = !feof(f);
    fclose(f);
    if (too_big) {
        memset(p, 0, sizeof *p);
        return fail(p, "fichier trop gros pour une polaire");
    }
    buf[n] = '\0';
    return polar_parse_string(p, buf);
}

/* Indice i tel que v[i] <= x <= v[i+1], x déjà borné à [v[0], v[n-1]]. */
static int bracket(const double *v, int n, double x)
{
    if (n < 2)
        return 0;
    for (int i = 0; i < n - 2; i++)
        if (x < v[i + 1])
            return i;
    return n - 2;
}

double polar_speed(const polar_t *p, double twa_deg, double tws_kn)
{
    if (!p || p->n_twa < 1 || p->n_tws < 1 || isnan(twa_deg) || isnan(tws_kn))
        return 0.0;

    /* Polaire symétrique : on replie l'angle dans [0, 180]. */
    double a = fmod(twa_deg, 360.0);
    if (a < 0)     a += 360.0;
    if (a > 180.0) a = 360.0 - a;

    /* Bornage aux extrêmes de la table : pas d'extrapolation. */
    if (a < p->twa[0])            a = p->twa[0];
    if (a > p->twa[p->n_twa - 1]) a = p->twa[p->n_twa - 1];
    double w = tws_kn;
    if (w < p->tws[0])            w = p->tws[0];
    if (w > p->tws[p->n_tws - 1]) w = p->tws[p->n_tws - 1];

    int i = bracket(p->twa, p->n_twa, a);
    int j = bracket(p->tws, p->n_tws, w);
    int i2 = (p->n_twa > 1) ? i + 1 : i;
    int j2 = (p->n_tws > 1) ? j + 1 : j;

    /* Colonnes ou lignes en double (écart nul) : poids 0, pas de division. */
    double da = p->twa[i2] - p->twa[i];
    double dw = p->tws[j2] - p->tws[j];
    double fa = (da > 0) ? (a - p->twa[i]) / da : 0.0;
    double fw = (dw > 0) ? (w - p->tws[j]) / dw : 0.0;

    return (1 - fa) * (1 - fw) * p->spd[i][j]
         +      fa  * (1 - fw) * p->spd[i2][j]
         + (1 - fa) *      fw  * p->spd[i][j2]
         +      fa  *      fw  * p->spd[i2][j2];
}

bool polar_name_ok(const char *name)
{
    if (!name || name[0] == '.')
        return false;
    size_t n = strlen(name);
    if (n >= 12 && strcasecmp(name + n - 12, ".polwave.csv") == 0)
        return false;                   /* table de vagues, pas une polaire */
    return (n > 4 && (strcasecmp(name + n - 4, ".pol") == 0 ||
                      strcasecmp(name + n - 4, ".csv") == 0));
}
