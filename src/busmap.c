/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

#include "busmap.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <libgen.h>
#include <unistd.h>

/* Échappe " et \ pour le JSON. */
static void json_escape(char *dst, size_t sz, const char *src)
{
    size_t i = 0;
    for (; src && *src && i + 2 < sz; src++) {
        if (*src == '"' || *src == '\\')
            dst[i++] = '\\';
        dst[i++] = *src;
    }
    dst[i] = '\0';
}

/* Code court du statut d'arbitrage, pour le JSON / l'UI. */
static const char *bm_status(int r)
{
    switch (r) {
        case ARB_ACCEPT:              return "accept";
        case ARB_REJECT_PRIORITY:     return "reject_priority";
        case ARB_REJECT_NO_RULE:      return "no_rule";
        case ARB_REJECT_UNCONFIGURED: return "unconfigured";
        case ARB_REJECT_NOT_IN_RULE:  return "not_in_rule";
        case ARB_REJECT_UNKNOWN_SRC:  return "unknown_src";
        case ARB_IGNORED:             return "ignored";
        default:                      return "?";
    }
}

void busmap_observe(busmap_t *bm, const jsonl_msg_t *m,
                    const arb_decision_t *d, uint64_t now)
{
    if (!bm || !m || !m->has_pgn || !m->has_src)
        return;
    const char *disc = (d && d->discriminant[0]) ? d->discriminant : "";

    for (int i = 0; i < bm->n; i++) {
        bm_entry_t *e = &bm->e[i];
        if (e->pgn == m->pgn && e->src == m->src && strcmp(e->disc, disc) == 0) {
            e->result = d ? (int)d->result : 0;
            e->last = now;
            if (d && d->identity)    snprintf(e->ident, sizeof e->ident, "%s", d->identity);
            if (d && d->source_name) snprintf(e->name,  sizeof e->name,  "%s", d->source_name);
            return;
        }
    }
    if (bm->n >= BUSMAP_MAX)
        return;
    bm_entry_t *e = &bm->e[bm->n++];
    memset(e, 0, sizeof *e);
    e->pgn = m->pgn;
    e->src = m->src;
    snprintf(e->disc, sizeof e->disc, "%s", disc);
    e->result = d ? (int)d->result : 0;
    e->last = now;
    if (d && d->identity)    snprintf(e->ident, sizeof e->ident, "%s", d->identity);
    if (d && d->source_name) snprintf(e->name,  sizeof e->name,  "%s", d->source_name);
}

/* Purge les entrées non vues depuis plus de BUSMAP_KEEP_MS. */
static void busmap_prune(busmap_t *bm, uint64_t now)
{
    int k = 0;
    for (int i = 0; i < bm->n; i++)
        if (now - bm->e[i].last <= BUSMAP_KEEP_MS)
            bm->e[k++] = bm->e[i];
    bm->n = k;
}

int busmap_to_json(busmap_t *bm, char *buf, size_t sz, uint64_t now)
{
    if (!bm || !buf || sz == 0)
        return -1;
    busmap_prune(bm, now);

    size_t n = 0;
    int w = snprintf(buf, sz, "{\"units\":[\n");
    if (w < 0 || (size_t)w >= sz) return -1;
    n += w;

    char done[BUSMAP_MAX];
    memset(done, 0, sizeof done);
    bool first_unit = true;

    for (int i = 0; i < bm->n; i++) {
        if (done[i])
            continue;
        int pgn = bm->e[i].pgn;
        const char *disc = bm->e[i].disc;

        char edisc[CFG_DISC_LEN * 2];
        json_escape(edisc, sizeof edisc, disc);
        w = snprintf(buf + n, sz - n, "%s{\"pgn\":%d,\"disc\":\"%s\",\"sources\":[",
                     first_unit ? "" : ",\n", pgn, edisc);
        if (w < 0 || (size_t)w >= sz - n) return -1;
        n += w;
        first_unit = false;

        bool first_src = true;
        for (int j = i; j < bm->n; j++) {
            bm_entry_t *e = &bm->e[j];
            if (done[j] || e->pgn != pgn || strcmp(e->disc, disc) != 0)
                continue;
            done[j] = 1;
            char eident[CFG_IDENT_LEN * 2], ename[CFG_NAME_LEN * 2];
            json_escape(eident, sizeof eident, e->ident);
            json_escape(ename, sizeof ename, e->name);
            int alive = (now - e->last) <= BUSMAP_ALIVE_MS;
            w = snprintf(buf + n, sz - n,
                         "%s{\"src\":%d,\"ident\":\"%s\",\"name\":\"%s\",\"alive\":%s,\"status\":\"%s\"}",
                         first_src ? "" : ",", e->src, eident, ename,
                         alive ? "true" : "false", bm_status(e->result));
            if (w < 0 || (size_t)w >= sz - n) return -1;
            n += w;
            first_src = false;
        }
        w = snprintf(buf + n, sz - n, "]}");
        if (w < 0 || (size_t)w >= sz - n) return -1;
        n += w;
    }

    w = snprintf(buf + n, sz - n, "\n]}\n");
    if (w < 0 || (size_t)w >= sz - n) return -1;
    n += w;
    return (int)n;
}

int busmap_write(busmap_t *bm, const char *path, uint64_t now)
{
    static char buf[BUSMAP_MAX * 200 + 64];
    int len = busmap_to_json(bm, buf, sizeof buf, now);
    if (len < 0)
        return -1;

    char dircopy[512];
    snprintf(dircopy, sizeof dircopy, "%s", path);
    char *dir = dirname(dircopy);
    if (dir && strcmp(dir, ".") != 0)
        mkdir(dir, 0755);   /* best-effort */

    char tmp[576];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    size_t wr = fwrite(buf, 1, (size_t)len, f);
    if (fclose(f) != 0 || wr != (size_t)len) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}
