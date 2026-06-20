/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_stats.c — Vérifie l'estimation des trames, les débits et le JSON.
 */

#include "stats.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void ok(const char *what, int cond)
{
    if (!cond) { fprintf(stderr, "FAIL %s\n", what); failures++; }
}

int main(void)
{
    /* --- estimation des trames --- */
    ok("129025 single-frame", stats_frames_for(129025) == 1);
    ok("inconnu single-frame", stats_frames_for(999999) == 1);
    /* 126996 : 134 octets → 1 + ceil(128/7) = 1 + 19 = 20 */
    ok("126996 = 20 trames", stats_frames_for(126996) == 20);
    /* 129810 : 35 octets → 1 + ceil(29/7) = 1 + 5 = 6 */
    ok("129810 = 6 trames", stats_frames_for(129810) == 6);
    /* 129040 : 54 octets → 1 + ceil(48/7) = 1 + 7 = 8 */
    ok("129040 = 8 trames", stats_frames_for(129040) == 8);

    /* --- débits sur une fenêtre connue --- */
    stats_t s;
    stats_init(&s, 1000);                 /* t0 = 1000 ms */
    for (int i = 0; i < 10; i++) stats_observe(&s, 129025);   /* 10 msg, 10 trames */
    for (int i = 0; i < 2;  i++) stats_observe(&s, 126996);   /* 2 msg, 40 trames */
    /* fenêtre = 2 s → 12 msg / 2 = 6 msg/s ; 50 trames / 2 = 25 trames/s */
    double mps, fps, load;
    stats_summary(&s, 3000, &mps, &fps, &load);
    ok("6 msg/s", mps > 5.99 && mps < 6.01);
    ok("25 trames/s", fps > 24.9 && fps < 25.1);
    /* charge = 25 * 130 / 250000 * 100 = 1.3 % */
    ok("charge ~1.3%", load > 1.29 && load < 1.31);

    /* --- sortie 0183 : phrases + octets + charge (réf. 4800 bauds) --- */
    stats_observe_out(&s, "GLL", 30);   /* 2 s plus tard la fenêtre fait 2 s */
    stats_observe_out(&s, "GLL", 30);
    stats_observe_out(&s, "VTG", 40);
    double osps, obps, oload;
    stats_summary_out(&s, 3000, &osps, &obps, &oload);
    ok("3 phrases / 2 s = 1.5/s", osps > 1.49 && osps < 1.51);
    ok("100 o / 2 s = 50 o/s", obps > 49.9 && obps < 50.1);
    /* charge = 50 / (4800/10) * 100 = 10.42 % */
    ok("charge 0183 ~10.4%", oload > 10.3 && oload < 10.5);

    /* --- pagination : 1 rafale GSV de 3 pages → le TYPE n'est compté qu'une
       fois (intervalle = rafale), mais les 3 pages pèsent sur la bande
       passante (charge 0183). --- */
    stats_observe_out(&s, "GSV", 30);    /* page 1 : type + octets */
    stats_observe_out_bytes(&s, 30);     /* page 2 : octets seuls */
    stats_observe_out_bytes(&s, 30);     /* page 3 : octets seuls */
    int gsv_count = -1;
    for (int i = 0; i < s.n_types; i++)
        if (strcmp(s.types[i].type, "GSV") == 0) { gsv_count = (int)s.types[i].count; break; }
    ok("GSV compté 1 fois (rafale)", gsv_count == 1);
    stats_summary_out(&s, 3000, &osps, NULL, NULL);
    ok("6 phrases / 2 s = 3/s (bande passante)", osps > 2.99 && osps < 3.01);

    /* --- JSON : contient les champs attendus --- */
    char buf[4096];
    int len = stats_to_json(&s, buf, sizeof buf, 3000);
    ok("json len > 0", len > 0);
    ok("json bus_load", strstr(buf, "\"bus_load_pct\":") != NULL);
    ok("json out_load", strstr(buf, "\"out_load_pct\":") != NULL);
    ok("json pgn 129025", strstr(buf, "\"pgn\":129025") != NULL);
    ok("json pgn 126996", strstr(buf, "\"pgn\":126996") != NULL);
    ok("json type GLL", strstr(buf, "\"type\":\"GLL\"") != NULL);

    /* --- reset : la fenêtre repart, les totaux restent --- */
    stats_reset(&s, 3000);
    stats_summary(&s, 4000, &mps, NULL, NULL);
    ok("0 msg/s après reset", mps == 0.0);
    stats_observe(&s, 129025);            /* total 129025 doit valoir 11 */
    int found = 0;
    for (int i = 0; i < s.n_pgns; i++)
        if (s.pgns[i].pgn == 129025) { found = (s.pgns[i].total == 11); break; }
    ok("total 129025 = 11", found);

    fputs(buf, stdout);
    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
