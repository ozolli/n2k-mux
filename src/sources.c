/*
 * sources.c — Sérialisation/relecture des sources vues (JSON).
 *
 * Format : un tableau JSON, un objet device par ligne, p.ex.
 *   [
 *   {"src":2,"ident":"917661","mfg":"","model":"veratron GO GPS","serial":"917661","seen":132}
 *   ]
 * La relecture est volontairement simple (extraction par clé sur chaque ligne),
 * adaptée à NOTRE format ; pas un parser JSON généraliste.
 */

#include "sources.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>
#include <unistd.h>

/* Échappe " et \ d'une chaîne dans dst (JSON). */
static void json_escape(char *dst, size_t sz, const char *src)
{
    size_t i = 0;
    for (; *src && i + 2 < sz; src++) {
        if (*src == '"' || *src == '\\')
            dst[i++] = '\\';
        dst[i++] = *src;
    }
    dst[i] = '\0';
}

int sources_to_json(const registry_t *r, char *buf, size_t sz)
{
    if (!r || !buf || sz == 0)
        return -1;
    size_t n = 0;
    int w = snprintf(buf, sz, "[\n");
    if (w < 0 || (size_t)w >= sz) return -1;
    n += w;

    bool first = true;
    for (int i = 0; i < r->n; i++) {
        const reg_device_t *d = &r->dev[i];
        if (!d->used)
            continue;
        char emfg[REG_MFG_LEN * 2], emodel[REG_MODEL_LEN * 2], eserial[REG_SERIAL_LEN * 2], eident[REG_IDENT_LEN * 2];
        json_escape(eident, sizeof eident, d->ident);
        json_escape(emfg, sizeof emfg, d->mfg);
        json_escape(emodel, sizeof emodel, d->model);
        json_escape(eserial, sizeof eserial, d->serial);
        w = snprintf(buf + n, sz - n,
                     "%s{\"src\":%d,\"ident\":\"%s\",\"mfg\":\"%s\",\"model\":\"%s\",\"serial\":\"%s\",\"seen\":%lu}\n",
                     first ? "" : ",", d->src, eident, emfg, emodel, eserial, d->seen);
        if (w < 0 || (size_t)w >= sz - n) return -1;
        n += w;
        first = false;
    }
    w = snprintf(buf + n, sz - n, "]\n");
    if (w < 0 || (size_t)w >= sz - n) return -1;
    n += w;
    return (int)n;
}

int sources_write(const registry_t *r, const char *path)
{
    char buf[SRC_VIEW_MAX * 256 + 16];
    int len = sources_to_json(r, buf, sizeof buf);
    if (len < 0)
        return -1;

    /* crée au mieux le répertoire parent */
    char dircopy[512];
    snprintf(dircopy, sizeof dircopy, "%s", path);
    char *dir = dirname(dircopy);
    if (dir && strcmp(dir, ".") != 0)
        mkdir(dir, 0755);   /* best-effort ; ignore l'erreur (existe déjà / droits) */

    char tmp[576];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    size_t wr = fwrite(buf, 1, (size_t)len, f);
    if (fclose(f) != 0 || wr != (size_t)len) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Extrait la valeur chaîne de "key":"..." sur une ligne. */
static bool jget_str(const char *line, const char *key, char *out, size_t sz)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p)
        return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < sz) {
        if (*p == '\\' && p[1]) p++;   /* déséchappe */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* Extrait la valeur numérique de "key":N sur une ligne. */
static bool jget_num(const char *line, const char *key, long *out)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p)
        return false;
    *out = strtol(p + strlen(pat), NULL, 10);
    return true;
}

int sources_load(const char *path, sources_view_t *v)
{
    if (!v)
        return -1;
    v->n = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        long src;
        if (!jget_num(line, "src", &src))     /* ignore [ , ] et lignes vides */
            continue;
        if (v->n >= SRC_VIEW_MAX)
            break;
        src_entry_t *e = &v->e[v->n];
        memset(e, 0, sizeof *e);
        e->src = (int)src;
        jget_str(line, "ident", e->ident, sizeof e->ident);
        jget_str(line, "mfg", e->mfg, sizeof e->mfg);
        jget_str(line, "model", e->model, sizeof e->model);
        jget_str(line, "serial", e->serial, sizeof e->serial);
        long seen = 0;
        jget_num(line, "seen", &seen);
        e->seen = (unsigned long)seen;
        v->n++;
    }
    fclose(f);
    return v->n;
}
