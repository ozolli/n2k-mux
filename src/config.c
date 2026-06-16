/*
 * config.c — Implémentation du parser INI.
 *
 * Un seul passage ligne par ligne, avec un état "section courante". Chaque
 * ligne est nettoyée (commentaire ôté, espaces rognés) puis interprétée selon
 * la section. Tolérant aux sections/clés inconnues ; signale en revanche les
 * lignes franchement malformées et les débordements de table (err/err_line).
 */

#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

enum { SEC_NONE = 0, SEC_OUTPUT, SEC_SOURCES, SEC_PRIORITY, SEC_IGNORE, SEC_OTHER };

/* Copie bornée, toujours terminée NUL. */
static void cpy(char *dst, size_t sz, const char *src)
{
    if (!sz) return;
    size_t i = 0;
    for (; src[i] && i + 1 < sz; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Rogne espaces de début/fin (en place). Retourne le début utile. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int section_of(const char *n)
{
    if (strcasecmp(n, "output") == 0)   return SEC_OUTPUT;
    if (strcasecmp(n, "sources") == 0)  return SEC_SOURCES;
    if (strcasecmp(n, "priority") == 0 || strcasecmp(n, "priorities") == 0 ||
        strcasecmp(n, "priorites") == 0) return SEC_PRIORITY;
    if (strcasecmp(n, "ignore") == 0)   return SEC_IGNORE;
    return SEC_OTHER;
}

static bool fail(config_t *c, int lineno, const char *msg)
{
    snprintf(c->err, sizeof c->err, "%s", msg);
    c->err_line = lineno;
    return false;
}

static bool add_source(config_t *c, int lineno, const char *name, const char *ident)
{
    if (c->n_sources >= CFG_MAX_SOURCES)
        return fail(c, lineno, "trop de sources ([sources])");
    cfg_source_t *s = &c->sources[c->n_sources++];
    cpy(s->name, sizeof s->name, name);
    cpy(s->ident, sizeof s->ident, ident);
    return true;
}

/* Ajoute la liste d'entiers `val` (séparés par virgules) dans dst[]. */
static bool add_int_list(config_t *c, int lineno, char *val, int *dst, int *n, int max)
{
    char *save = NULL;
    for (char *tok = strtok_r(val, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *t = trim(tok);
        if (!*t) continue;
        if (*n >= max)
            return fail(c, lineno, "trop de valeurs ([ignore])");
        dst[(*n)++] = (int)strtol(t, NULL, 10);
    }
    return true;
}

static bool add_rule(config_t *c, int lineno, char *key, char *val)
{
    if (c->n_rules >= CFG_MAX_RULES)
        return fail(c, lineno, "trop de règles ([priority])");
    cfg_rule_t *r = &c->rules[c->n_rules];
    memset(r, 0, sizeof *r);

    /* clé : pgn [ '/' discriminant ] */
    char *slash = strchr(key, '/');
    if (slash) {
        *slash = '\0';
        cpy(r->discriminant, sizeof r->discriminant, trim(slash + 1));
    }
    char *pk = trim(key);
    r->pgn = (int)strtol(pk, NULL, 10);
    if (r->pgn <= 0)
        return fail(c, lineno, "PGN invalide dans [priority]");

    /* valeur : [ mode ':' ] liste de sources */
    r->mode = CFG_PICK_PRIORITY;
    char *list = val;
    char *colon = strchr(val, ':');
    if (colon) {
        *colon = '\0';
        char *m = trim(val);
        if (strcasecmp(m, "min") == 0)            r->mode = CFG_PICK_MIN;
        else if (strcasecmp(m, "fusion") == 0)    r->mode = CFG_PICK_FUSION;
        else if (strcasecmp(m, "priority") == 0)  r->mode = CFG_PICK_PRIORITY;
        else return fail(c, lineno, "mode inconnu (attendu min/fusion/priority)");
        list = colon + 1;
    }

    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *name = trim(tok);
        if (!*name) continue;
        if (r->n_sources >= CFG_MAX_PRIO)
            return fail(c, lineno, "trop de sources dans une règle");
        cpy(r->sources[r->n_sources++], CFG_NAME_LEN, name);
    }
    if (r->n_sources == 0)
        return fail(c, lineno, "règle sans source");

    c->n_rules++;
    return true;
}

/* Traite une ligne (modifiable). Retourne false sur erreur fatale. */
static bool feed(config_t *c, int *section, char *line, int lineno)
{
    /* couper le commentaire */
    for (char *p = line; *p; p++)
        if (*p == ';' || *p == '#') { *p = '\0'; break; }

    char *s = trim(line);
    if (!*s)
        return true;            /* ligne vide */

    if (*s == '[') {
        char *end = strchr(s, ']');
        if (!end)
            return fail(c, lineno, "section non terminée (']' manquant)");
        *end = '\0';
        *section = section_of(trim(s + 1));
        return true;
    }

    char *eq = strchr(s, '=');
    if (!eq)
        return fail(c, lineno, "ligne sans '='");
    *eq = '\0';
    char *key = trim(s);
    char *val = trim(eq + 1);
    if (!*key)
        return fail(c, lineno, "clé vide");

    switch (*section) {
        case SEC_OUTPUT:
            if (strcasecmp(key, "talker") == 0)
                cpy(c->talker, sizeof c->talker, val);
            return true;
        case SEC_SOURCES:
            return add_source(c, lineno, key, val);
        case SEC_PRIORITY:
            return add_rule(c, lineno, key, val);
        case SEC_IGNORE:
            if (strcasecmp(key, "src") == 0)
                return add_int_list(c, lineno, val, c->ignore_src,
                                    &c->n_ignore_src, CFG_MAX_IGNORE);
            if (strcasecmp(key, "pgn") == 0)
                return add_int_list(c, lineno, val, c->ignore_pgn,
                                    &c->n_ignore_pgn, CFG_MAX_IGNORE);
            return true;        /* clé inconnue dans [ignore] : ignorée */
        default:
            return true;        /* section inconnue : ignorée */
    }
}

void config_init(config_t *c)
{
    memset(c, 0, sizeof *c);
    cpy(c->talker, sizeof c->talker, "II");
}

bool config_load(config_t *c, const char *path)
{
    config_init(c);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(c->err, sizeof c->err, "ouverture impossible : %s", path);
        return false;
    }
    char line[512];
    int section = SEC_NONE, lineno = 0;
    bool ok = true;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (!feed(c, &section, line, lineno)) { ok = false; break; }
    }
    fclose(f);
    return ok;
}

bool config_parse_string(config_t *c, const char *text)
{
    config_init(c);
    int section = SEC_NONE, lineno = 0;
    char line[512];
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        lineno++;
        if (!feed(c, &section, line, lineno))
            return false;
        if (!nl) break;
        p = nl + 1;
    }
    return true;
}

const char *config_source_ident(const config_t *c, const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < c->n_sources; i++)
        if (strcmp(c->sources[i].name, name) == 0)
            return c->sources[i].ident;
    return NULL;
}

const char *config_source_name(const config_t *c, const char *ident)
{
    if (!ident || !ident[0]) return NULL;
    for (int i = 0; i < c->n_sources; i++)
        if (strcmp(c->sources[i].ident, ident) == 0)
            return c->sources[i].name;
    return NULL;
}

const cfg_rule_t *config_rule(const config_t *c, int pgn, const char *disc)
{
    const cfg_rule_t *generic = NULL;
    const cfg_rule_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < c->n_rules; i++) {
        const cfg_rule_t *r = &c->rules[i];
        if (r->pgn != pgn)
            continue;
        if (r->discriminant[0] == '\0') {
            if (!generic) generic = r;       /* règle « pour tout » */
            continue;
        }
        /* règle à discriminant : doit préfixer la valeur observée */
        if (disc && *disc) {
            size_t dl = strlen(r->discriminant);
            if (strncasecmp(r->discriminant, disc, dl) == 0 && dl > best_len) {
                best = r;
                best_len = dl;               /* on garde la plus spécifique */
            }
        }
    }
    return best ? best : generic;
}

static bool in_list(const int *a, int n, int v)
{
    for (int i = 0; i < n; i++)
        if (a[i] == v) return true;
    return false;
}

bool config_ignore_src(const config_t *c, int src)
{
    return in_list(c->ignore_src, c->n_ignore_src, src);
}

bool config_ignore_pgn(const config_t *c, int pgn)
{
    return in_list(c->ignore_pgn, c->n_ignore_pgn, pgn);
}
