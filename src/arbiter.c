/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * arbiter.c — Implémentation du cœur de sélection.
 *
 * Pour chaque message : exclusions → discriminant → règle (config) →
 * résolution src→identité→nom (registre+config) → décision selon le mode,
 * avec suivi de vivacité par (règle, slot de source) pour le failover.
 */

#include "arbiter.h"
#include <string.h>

static void cpy(char *dst, size_t sz, const char *src)
{
    if (!sz) return;
    size_t i = 0;
    for (; src[i] && i + 1 < sz; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Champ porteur du discriminant pour ce PGN, ou NULL si le PGN n'en a pas. */
static const char *discriminant_of(int pgn, const jsonl_msg_t *m)
{
    const char *v = NULL;
    switch (pgn) {
        case 130306:   /* Wind Data : Apparent / True */
        case 127250:   /* Vessel Heading : Magnetic / True */
            if (jsonl_get_str(m, "Reference", &v)) return v;
            return NULL;
        case 130312:   /* Temperature : Sea / Outside */
            if (jsonl_get_str(m, "Temperature Source", &v)) return v;
            if (jsonl_get_str(m, "Source", &v)) return v;
            return NULL;
        default:
            return NULL;
    }
}

/* Nom logique d'une identité, ou NULL si absente de [sources]. */
static const char *name_by_ident(const config_t *c, const char *ident)
{
    for (int i = 0; i < c->n_sources; i++)
        if (strcmp(c->sources[i].ident, ident) == 0)
            return c->sources[i].name;
    return NULL;
}

void arbiter_init(arbiter_t *a, const config_t *cfg, const registry_t *reg)
{
    memset(a, 0, sizeof *a);
    a->cfg = cfg;
    a->reg = reg;
    a->timeout_ms = ARB_DEFAULT_TIMEOUT_MS;
}

void arbiter_set_timeout(arbiter_t *a, unsigned timeout_ms)
{
    a->timeout_ms = timeout_ms ? timeout_ms : ARB_DEFAULT_TIMEOUT_MS;
}

static bool slot_live(const arbiter_t *a, int ri, int slot, uint64_t now)
{
    if (!a->seen[ri][slot])
        return false;
    return (now - a->last_seen[ri][slot]) <= a->timeout_ms;
}

/* Index du 1er slot vivant de la règle (priorité décroissante), ou -1. */
static int live_winner(const arbiter_t *a, const cfg_rule_t *r, int ri, uint64_t now)
{
    for (int i = 0; i < r->n_sources; i++)
        if (slot_live(a, ri, i, now))
            return i;
    return -1;
}

arb_decision_t arbiter_decide(arbiter_t *a, const jsonl_msg_t *m, uint64_t now_ms)
{
    arb_decision_t d;
    memset(&d, 0, sizeof d);

    if (!m || !m->has_pgn) { d.result = ARB_IGNORED; return d; }

    int pgn = m->pgn;
    int src = m->has_src ? m->src : -1;

    /* exclusions d'office + [ignore] */
    if (src <= 0 || pgn >= 262144 ||
        config_ignore_src(a->cfg, src) || config_ignore_pgn(a->cfg, pgn)) {
        d.result = ARB_IGNORED;
        return d;
    }

    /* discriminant */
    const char *disc = discriminant_of(pgn, m);
    if (disc)
        cpy(d.discriminant, sizeof d.discriminant, disc);

    /* règle */
    const cfg_rule_t *r = config_rule(a->cfg, pgn, disc);
    if (!r) { d.result = ARB_REJECT_NO_RULE; return d; }
    d.rule = r;
    d.mode = r->mode;

    /* src → identité → nom logique */
    const reg_device_t *dev = registry_by_src(a->reg, src);
    if (!dev || !dev->ident[0]) { d.result = ARB_REJECT_UNKNOWN_SRC; return d; }
    d.identity = dev->ident;

    const char *name = name_by_ident(a->cfg, dev->ident);
    if (!name) { d.result = ARB_REJECT_UNCONFIGURED; return d; }
    d.source_name = name;

    /* slot de cette source dans la règle */
    int slot = -1;
    for (int i = 0; i < r->n_sources; i++)
        if (strcmp(r->sources[i], name) == 0) { slot = i; break; }
    if (slot < 0) { d.result = ARB_REJECT_NOT_IN_RULE; return d; }

    /* mise à jour de la vivacité */
    int ri = (int)(r - a->cfg->rules);
    a->seen[ri][slot]      = true;
    a->last_seen[ri][slot] = now_ms;

    if (r->mode == CFG_PICK_PRIORITY) {
        int win = live_winner(a, r, ri, now_ms);
        d.result = (win == slot) ? ARB_ACCEPT : ARB_REJECT_PRIORITY;
    } else {
        /* min / fusion : toute source listée et vivante participe */
        d.result = ARB_ACCEPT;
    }
    return d;
}

const char *arbiter_current_source(const arbiter_t *a, int pgn, const char *disc,
                                   uint64_t now_ms)
{
    const cfg_rule_t *r = config_rule(a->cfg, pgn, disc);
    if (!r || r->mode != CFG_PICK_PRIORITY)
        return NULL;
    int ri = (int)(r - a->cfg->rules);
    int win = live_winner(a, r, ri, now_ms);
    return win >= 0 ? r->sources[win] : NULL;
}

const char *arb_result_str(arb_result_t r)
{
    switch (r) {
        case ARB_ACCEPT:              return "ACCEPT";
        case ARB_REJECT_PRIORITY:     return "REJECT_PRIORITY";
        case ARB_REJECT_NO_RULE:      return "REJECT_NO_RULE";
        case ARB_REJECT_UNCONFIGURED: return "REJECT_UNCONFIGURED";
        case ARB_REJECT_NOT_IN_RULE:  return "REJECT_NOT_IN_RULE";
        case ARB_REJECT_UNKNOWN_SRC:  return "REJECT_UNKNOWN_SRC";
        case ARB_IGNORED:             return "IGNORED";
        default:                      return "?";
    }
}
