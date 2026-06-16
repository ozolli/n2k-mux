/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_arbiter.c — Vérifie le cœur de sélection.
 *
 * Monte un registre (identités résolues via des messages JSONL réalistes) et
 * une config, puis contrôle les décisions : priorité, failover par timeout,
 * mode min (participants multiples), discriminant, source non résolue / non
 * configurée / hors-règle, et exclusions (src<=0, PGN>=262144).
 */

#include "jsonl.h"
#include "registry.h"
#include "config.h"
#include "arbiter.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(const char *what, arb_result_t got, arb_result_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s : attendu %s, obtenu %s\n",
                what, arb_result_str(want), arb_result_str(got));
        failures++;
    }
}

/* Donne une identité (Model Serial Code) à une adresse via un 126996. */
static void reg_add(registry_t *r, int src, const char *serial)
{
    char line[256];
    snprintf(line, sizeof line,
             "{\"src\":%d,\"pgn\":126996,\"fields\":{\"Model Serial Code\":\"%s\"}}",
             src, serial);
    jsonl_msg_t m;
    if (jsonl_parse(line, &m)) registry_observe(r, &m);
}

/* Parse un message et le soumet à l'arbitre. */
static arb_decision_t decide(arbiter_t *a, const char *json, uint64_t now)
{
    jsonl_msg_t m;
    arb_decision_t d;
    if (!jsonl_parse(json, &m)) { memset(&d, 0, sizeof d); d.result = ARB_IGNORED; return d; }
    return arbiter_decide(a, &m, now);
}

static const char *INI =
    "[sources]\n"
    "SCX    = SER_SCX\n"
    "VER    = SER_VER\n"
    "MAD    = SER_MAD\n"
    "DST_BB = SER_DSTBB\n"
    "DST_TB = SER_DSTTB\n"
    "[priority]\n"
    "129025          = SCX, VER, MAD\n"
    "130306/Apparent = MAD\n"
    "128267          = min: DST_BB, DST_TB\n"
    "[ignore]\n"
    "pgn = 130311\n";

int main(void)
{
    config_t cfg;
    if (!config_parse_string(&cfg, INI)) {
        fprintf(stderr, "config invalide (ligne %d : %s)\n", cfg.err_line, cfg.err);
        return 2;
    }

    registry_t reg;
    registry_init(&reg);
    reg_add(&reg, 5,  "SER_SCX");
    reg_add(&reg, 6,  "SER_VER");
    reg_add(&reg, 7,  "SER_MAD");
    reg_add(&reg, 12, "SER_DSTBB");
    reg_add(&reg, 13, "SER_DSTTB");
    reg_add(&reg, 20, "SER_UNKNOWN");   /* résolu mais hors [sources] */

    arbiter_t arb;
    arbiter_init(&arb, &cfg, &reg);     /* timeout = 5000 ms */

    /* --- priorité : SCX (src5) gagne, VER/MAD supplantés --- */
    expect("129025 SCX @1000", decide(&arb, "{\"src\":5,\"pgn\":129025,\"fields\":{\"Latitude\":47.5}}", 1000).result, ARB_ACCEPT);
    expect("129025 VER @1000", decide(&arb, "{\"src\":6,\"pgn\":129025,\"fields\":{\"Latitude\":47.5}}", 1000).result, ARB_REJECT_PRIORITY);
    expect("129025 MAD @1000", decide(&arb, "{\"src\":7,\"pgn\":129025,\"fields\":{\"Latitude\":47.5}}", 1000).result, ARB_REJECT_PRIORITY);

    /* SCX reste maître tant qu'il est vivant */
    expect("129025 VER @4000", decide(&arb, "{\"src\":6,\"pgn\":129025,\"fields\":{}}", 4000).result, ARB_REJECT_PRIORITY);

    /* --- failover : SCX se tait ; à t=7000 (>5000 après son dernier @1000) VER prend la main --- */
    expect("129025 VER @7000 (failover)", decide(&arb, "{\"src\":6,\"pgn\":129025,\"fields\":{}}", 7000).result, ARB_ACCEPT);
    /* MAD reste derrière VER vivant */
    expect("129025 MAD @7000", decide(&arb, "{\"src\":7,\"pgn\":129025,\"fields\":{}}", 7000).result, ARB_REJECT_PRIORITY);
    /* SCX revient → reprend la priorité */
    expect("129025 SCX @7500 (retour)", decide(&arb, "{\"src\":5,\"pgn\":129025,\"fields\":{}}", 7500).result, ARB_ACCEPT);
    expect("129025 VER @7600", decide(&arb, "{\"src\":6,\"pgn\":129025,\"fields\":{}}", 7600).result, ARB_REJECT_PRIORITY);

    /* arbiter_current_source reflète le vainqueur courant */
    const char *cur = arbiter_current_source(&arb, 129025, NULL, 7600);
    if (!cur || strcmp(cur, "SCX") != 0) {
        fprintf(stderr, "FAIL current_source @7600 : attendu SCX, obtenu %s\n", cur ? cur : "(NULL)");
        failures++;
    }

    /* --- mode min : les deux DST participent --- */
    expect("128267 DST_BB", decide(&arb, "{\"src\":12,\"pgn\":128267,\"fields\":{\"Depth\":3.2}}", 8000).result, ARB_ACCEPT);
    expect("128267 DST_TB", decide(&arb, "{\"src\":13,\"pgn\":128267,\"fields\":{\"Depth\":3.4}}", 8000).result, ARB_ACCEPT);

    /* --- discriminant --- */
    expect("130306 Apparent (MAD)", decide(&arb, "{\"src\":7,\"pgn\":130306,\"fields\":{\"Reference\":\"Apparent\",\"Wind Speed\":4.2}}", 9000).result, ARB_ACCEPT);
    expect("130306 True (pas de règle)", decide(&arb, "{\"src\":7,\"pgn\":130306,\"fields\":{\"Reference\":\"True (ground referenced to North)\"}}", 9000).result, ARB_REJECT_NO_RULE);

    /* --- cas négatifs --- */
    expect("129025 src inconnu (registre)", decide(&arb, "{\"src\":99,\"pgn\":129025,\"fields\":{}}", 9000).result, ARB_REJECT_UNKNOWN_SRC);
    expect("129025 src non configuré", decide(&arb, "{\"src\":20,\"pgn\":129025,\"fields\":{}}", 9000).result, ARB_REJECT_UNCONFIGURED);
    expect("128267 SCX hors-règle", decide(&arb, "{\"src\":5,\"pgn\":128267,\"fields\":{}}", 9000).result, ARB_REJECT_NOT_IN_RULE);
    expect("pgn sans règle", decide(&arb, "{\"src\":5,\"pgn\":127999,\"fields\":{}}", 9000).result, ARB_REJECT_NO_RULE);

    /* --- exclusions --- */
    expect("src=0 ignoré", decide(&arb, "{\"src\":0,\"pgn\":129025,\"fields\":{}}", 9000).result, ARB_IGNORED);
    expect("PGN>=262144 ignoré", decide(&arb, "{\"src\":5,\"pgn\":262161,\"fields\":{}}", 9000).result, ARB_IGNORED);
    expect("PGN [ignore] config", decide(&arb, "{\"src\":5,\"pgn\":130311,\"fields\":{}}", 9000).result, ARB_IGNORED);

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
