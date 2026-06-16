/*
 * registry.c — Implémentation de la table src → identité stable.
 *
 * Invariant maintenu : au plus un enregistrement `used` par adresse src
 * (les autres références à cette adresse sont détachées, src = N2K_ADDR_NULL).
 * Les emplacements ne sont jamais libérés : `n` est le high-water mark.
 *
 * Corrélation des deux PGN d'identité :
 *   - 60928 porte le Unique Number : identifiant matériel fiable. On suit le
 *     device par ce numéro et on met à jour quelle adresse il occupe.
 *   - 126996 ne porte aucun identifiant matériel : on ne peut le corréler que
 *     par l'adresse src courante (celui qui occupe l'adresse à cet instant).
 */

#include "registry.h"
#include <string.h>
#include <stdio.h>

/* Copie bornée, toujours terminée NUL (strlcpy n'est pas portable). */
static void cpy(char *dst, size_t sz, const char *src)
{
    if (!sz)
        return;
    size_t i = 0;
    for (; src[i] && i + 1 < sz; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static reg_device_t *find_by_src(registry_t *r, int src)
{
    if (src < 0)
        return NULL;
    for (int i = 0; i < r->n; i++)
        if (r->dev[i].used && r->dev[i].src == src)
            return &r->dev[i];
    return NULL;
}

static reg_device_t *find_by_unique(registry_t *r, uint32_t u)
{
    for (int i = 0; i < r->n; i++)
        if (r->dev[i].used && r->dev[i].has_unique && r->dev[i].unique == u)
            return &r->dev[i];
    return NULL;
}

/* Détache de l'adresse src tout enregistrement autre que `keep`. */
static void detach_addr(registry_t *r, int src, const reg_device_t *keep)
{
    for (int i = 0; i < r->n; i++)
        if (r->dev[i].used && &r->dev[i] != keep && r->dev[i].src == src)
            r->dev[i].src = N2K_ADDR_NULL;
}

/* Alloue un emplacement neuf (sans adresse). NULL si table pleine. */
static reg_device_t *alloc_dev(registry_t *r)
{
    if (r->n >= REG_MAX_DEVICES)
        return NULL;
    reg_device_t *d = &r->dev[r->n++];
    memset(d, 0, sizeof *d);
    d->used = true;
    d->src  = N2K_ADDR_NULL;
    return d;
}

/* Retrouve le device à l'adresse src, ou en crée un provisoire. */
static reg_device_t *touch_by_src(registry_t *r, int src)
{
    reg_device_t *d = find_by_src(r, src);
    if (d)
        return d;
    d = alloc_dev(r);
    if (d)
        d->src = src;
    return d;
}

/* Recalcule l'identité stable : serial, sinon unique en décimal, sinon "". */
static void recompute_ident(reg_device_t *d)
{
    if (d->serial[0])
        cpy(d->ident, sizeof d->ident, d->serial);
    else if (d->has_unique)
        snprintf(d->ident, sizeof d->ident, "%lu", (unsigned long)d->unique);
    else
        d->ident[0] = '\0';
}

/* Met à jour le device depuis un PGN 60928 (ISO Address Claim). */
static reg_device_t *from_claim(registry_t *r, const jsonl_msg_t *m)
{
    int      src = m->src;
    bool     has_u = false;
    uint32_t u = 0;
    double   v;

    if (jsonl_get_num(m, "Unique Number", &v)) {
        u = (uint32_t)v;
        has_u = true;
    }

    reg_device_t *d = NULL;
    if (has_u) {
        d = find_by_unique(r, u);
        if (d) {
            /* device connu : a-t-il changé d'adresse ? */
            if (d->src != src) {
                detach_addr(r, src, d);   /* l'ancien occupant cède la place */
                d->src = src;
            }
        } else {
            /* unique inconnu : adopter un provisoire à cette adresse, sinon
             * évincer un autre device et créer un enregistrement neuf. */
            reg_device_t *occ = find_by_src(r, src);
            if (occ && !occ->has_unique) {
                d = occ;
            } else {
                if (occ)
                    occ->src = N2K_ADDR_NULL;
                d = alloc_dev(r);
                if (d)
                    d->src = src;
            }
            if (d) {
                d->unique = u;
                d->has_unique = true;
            }
        }
    } else {
        /* claim malformé sans Unique Number : corréler par adresse. */
        d = touch_by_src(r, src);
    }
    if (!d)
        return NULL;

    const char *name;
    if (jsonl_get_str(m, "Manufacturer Code", &name))
        cpy(d->mfg, sizeof d->mfg, name);
    double mc;
    if (jsonl_get_num(m, "Manufacturer Code", &mc)) {
        d->mfg_code = (int)mc;
        d->has_mfg = true;
    }
    return d;
}

/* Met à jour le device depuis un PGN 126996 (Product Information). */
static void from_product(reg_device_t *d, const jsonl_msg_t *m)
{
    const char *s;
    if (jsonl_get_str(m, "Model Serial Code", &s) && s[0])
        cpy(d->serial, sizeof d->serial, s);
    if (jsonl_get_str(m, "Model ID", &s) && s[0])
        cpy(d->model, sizeof d->model, s);
}

void registry_init(registry_t *r)
{
    if (!r)
        return;
    memset(r, 0, sizeof *r);
}

const reg_device_t *registry_observe(registry_t *r, const jsonl_msg_t *m)
{
    if (!r || !m || !m->has_pgn || !m->has_src || m->src < 0)
        return NULL;

    reg_device_t *d;
    if (m->pgn == 60928) {
        d = from_claim(r, m);
    } else {
        d = touch_by_src(r, m->src);
        if (d && m->pgn == 126996)
            from_product(d, m);
    }
    if (!d)
        return NULL;

    d->seen++;
    recompute_ident(d);
    return d;
}

const reg_device_t *registry_by_src(const registry_t *r, int src)
{
    if (!r || src < 0)
        return NULL;
    for (int i = 0; i < r->n; i++)
        if (r->dev[i].used && r->dev[i].src == src)
            return &r->dev[i];
    return NULL;
}

const reg_device_t *registry_by_ident(const registry_t *r, const char *ident)
{
    if (!r || !ident || !ident[0])
        return NULL;
    for (int i = 0; i < r->n; i++)
        if (r->dev[i].used && strcmp(r->dev[i].ident, ident) == 0)
            return &r->dev[i];
    return NULL;
}

const char *registry_ident(const registry_t *r, int src)
{
    const reg_device_t *d = registry_by_src(r, src);
    if (!d || !d->ident[0])
        return NULL;
    return d->ident;
}
