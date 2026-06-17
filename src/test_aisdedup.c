/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_aisdedup.c — Vérifie la fusion/dédup AIS par MMSI.
 *
 * Scénarios : priorité em-trak > DataHub quand les deux voient une cible,
 * union (cible vue par une seule source), failover par timeout, retour de la
 * source prioritaire, et repli stable quand l'identité n'est pas résolue.
 */

#include "jsonl.h"
#include "registry.h"
#include "config.h"
#include "aisdedup.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(const char *what, bool got, bool want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s : attendu %s, obtenu %s\n",
                what, want ? "ACCEPT" : "drop", got ? "ACCEPT" : "drop");
        failures++;
    }
}

static void reg_add(registry_t *r, int src, const char *serial)
{
    char line[256];
    snprintf(line, sizeof line,
             "{\"src\":%d,\"pgn\":126996,\"fields\":{\"Model Serial Code\":\"%s\"}}",
             src, serial);
    jsonl_msg_t m;
    if (jsonl_parse(line, &m)) registry_observe(r, &m);
}

static const char *INI =
    "[sources]\n"
    "AIS = SER_EMTRAK\n"     /* em-trak B953 */
    "DH  = SER_DATAHUB\n"    /* DataHub PredictWind */
    "[priority]\n"
    "129039 = fusion: AIS, DH\n";   /* em-trak prioritaire sur DataHub */

int main(void)
{
    config_t cfg;
    if (!config_parse_string(&cfg, INI)) { fprintf(stderr, "config KO\n"); return 2; }

    registry_t reg; registry_init(&reg);
    reg_add(&reg, 20,  "SER_EMTRAK");
    reg_add(&reg, 178, "SER_DATAHUB");

    aisdedup_t d; aisdedup_init(&d, &cfg, &reg);
    aisdedup_set_timeout(&d, 1000);   /* timeout court pour tester le failover */

    /* détection des PGN AIS */
    expect("129039 est AIS", aisdedup_is_ais(129039), true);
    expect("129795 est AIS", aisdedup_is_ais(129795), true);
    expect("129796 est AIS", aisdedup_is_ais(129796), true);
    expect("129797 est AIS", aisdedup_is_ais(129797), true);
    expect("129025 n'est pas AIS", aisdedup_is_ais(129025), false);

    const int EMTRAK = 20, DH = 178;
    const uint32_t A = 227000111, B = 227000222;

    /* cible A vue par les deux → em-trak gagne, DataHub rejeté */
    expect("A em-trak @1000",   aisdedup_accept(&d, 129039, A, EMTRAK, 1000), true);
    expect("A DataHub @1000",   aisdedup_accept(&d, 129039, A, DH,     1000), false);
    expect("A em-trak @1500",   aisdedup_accept(&d, 129039, A, EMTRAK, 1500), true);
    expect("A DataHub @1500",   aisdedup_accept(&d, 129039, A, DH,     1500), false);

    /* cible B vue par DataHub seul → acceptée (union) */
    expect("B DataHub @1000",   aisdedup_accept(&d, 129039, B, DH,     1000), true);

    /* failover : em-trak se tait ; à @3000 (>1000 après son @1500) DataHub prend A */
    expect("A DataHub @3000 (failover)", aisdedup_accept(&d, 129039, A, DH, 3000), true);

    /* em-trak revient → reprend la priorité sur A */
    expect("A em-trak @3500 (retour)", aisdedup_accept(&d, 129039, A, EMTRAK, 3500), true);
    expect("A DataHub @3600",          aisdedup_accept(&d, 129039, A, DH,     3600), false);

    /* repli sans identité : deux sources inconnues du registre (src 50/51) */
    const uint32_t C = 227000333;
    expect("C src50 @4000", aisdedup_accept(&d, 129039, C, 50, 4000), true);
    expect("C src51 @4000", aisdedup_accept(&d, 129039, C, 51, 4000), false); /* 50 < 51 */
    expect("C src50 @4100", aisdedup_accept(&d, 129039, C, 50, 4100), true);

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
