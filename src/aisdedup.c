/*
 * aisdedup.c — Implémentation de la fusion/dédup AIS par MMSI.
 *
 * Pour chaque (MMSI, src) on mémorise le dernier passage. Le « gagnant » d'un
 * MMSI = la source de meilleur rang parmi celles vues depuis moins de
 * timeout_ms. Le rang vient de la règle [priority] fusion du PGN (résolu via
 * registre src→identité puis config identité→nom) ; à défaut (identité non
 * encore résolue), repli stable par adresse source.
 */

#include "aisdedup.h"
#include <string.h>
#include <limits.h>

void aisdedup_init(aisdedup_t *a, const config_t *cfg, const registry_t *reg)
{
    memset(a, 0, sizeof *a);
    a->cfg = cfg;
    a->reg = reg;
    a->timeout_ms = AIS_DEFAULT_TIMEOUT;
}

void aisdedup_set_timeout(aisdedup_t *a, unsigned timeout_ms)
{
    a->timeout_ms = timeout_ms ? timeout_ms : AIS_DEFAULT_TIMEOUT;
}

bool aisdedup_is_ais(int pgn)
{
    switch (pgn) {
        case 129038: case 129039: case 129040: case 129041:
        case 129793: case 129794: case 129798:
        case 129801: case 129802: case 129809: case 129810:
            return true;
        default:
            return false;
    }
}

/* Rang de priorité d'une source pour ce PGN (plus petit = prioritaire). */
static int rank_of(const aisdedup_t *a, int pgn, int src)
{
    const char *ident = registry_ident(a->reg, src);
    const char *name  = ident ? config_source_name(a->cfg, ident) : NULL;
    const cfg_rule_t *r = config_rule(a->cfg, pgn, NULL);
    if (name && r && r->mode == CFG_PICK_FUSION) {
        for (int i = 0; i < r->n_sources; i++)
            if (strcmp(r->sources[i], name) == 0)
                return i;
    }
    /* repli : sources non configurées après les configurées, ordre stable */
    return 1000 + (src & 0xff);
}

bool aisdedup_accept(aisdedup_t *a, int pgn, uint32_t mmsi, int src, uint64_t now)
{
    /* enregistre/rafraîchit (mmsi, src) ; sinon réutilise un slot libre, sinon
     * évince le plus ancien */
    ais_obs_t *slot = NULL, *freeslot = NULL, *oldest = NULL;
    for (int i = 0; i < AIS_MAX_OBS; i++) {
        ais_obs_t *o = &a->obs[i];
        if (o->used && o->mmsi == mmsi && o->src == src) { slot = o; break; }
        if (!o->used) { if (!freeslot) freeslot = o; }
        else if (!oldest || o->t < oldest->t) oldest = o;
    }
    if (!slot) slot = freeslot ? freeslot : oldest;
    slot->used = true;
    slot->mmsi = mmsi;
    slot->src  = src;
    slot->t    = now;

    /* gagnant : meilleur rang parmi les sources vivantes de ce MMSI */
    int best_src = src, best_rank = INT_MAX;
    for (int i = 0; i < AIS_MAX_OBS; i++) {
        ais_obs_t *o = &a->obs[i];
        if (!o->used || o->mmsi != mmsi) continue;
        if (now - o->t > a->timeout_ms) continue;
        int r = rank_of(a, pgn, o->src);
        if (r < best_rank || (r == best_rank && o->src < best_src)) {
            best_rank = r;
            best_src  = o->src;
        }
    }
    return src == best_src;
}
