/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_mapper.c — Vérifie le mapping PGN → 0183 de bout en bout (config →
 * registry → arbiter → mapper), avec valeurs aux unités canboat réelles.
 *
 * Pour chaque phrase : contrôle du préfixe attendu (type + champs clés, hors
 * checksum) et de la cohérence du framing/checksum.
 */

#include "jsonl.h"
#include "registry.h"
#include "config.h"
#include "arbiter.h"
#include "mapper.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static config_t  cfg;
static registry_t reg;
static arbiter_t  arb;
static mapper_t   mp;

static void reg_add(int src, const char *serial)
{
    char line[256];
    snprintf(line, sizeof line,
             "{\"src\":%d,\"pgn\":126996,\"fields\":{\"Model Serial Code\":\"%s\"}}",
             src, serial);
    jsonl_msg_t m;
    if (jsonl_parse(line, &m)) registry_observe(&reg, &m);
}

static map_out_t run(const char *json, uint64_t now)
{
    map_out_t out; out.n = 0;
    jsonl_msg_t m;
    if (!jsonl_parse(json, &m)) { fprintf(stderr, "JSON invalide: %s\n", json); failures++; return out; }
    arb_decision_t d = arbiter_decide(&arb, &m, now);
    mapper_map(&mp, &m, &d, now, &out);
    return out;
}

static unsigned char xsum(const char *s)
{
    unsigned char c = 0;
    for (const char *p = s + 1; *p && *p != '*'; p++) c ^= (unsigned char)*p;
    return c;
}

/* Vérifie : préfixe attendu + framing/checksum cohérent. */
static void chk(const char *what, const map_out_t *o, int idx, const char *prefix)
{
    if (idx >= o->n) { fprintf(stderr, "FAIL %s : phrase %d absente (n=%d)\n", what, idx, o->n); failures++; return; }
    const char *s = o->s[idx];
    if (strncmp(s, prefix, strlen(prefix)) != 0) {
        fprintf(stderr, "FAIL %s :\n  préfixe attendu : %s\n  obtenu          : %s", what, prefix, s);
        failures++;
        return;
    }
    const char *star = strrchr(s, '*');
    unsigned int emb = 0;
    if (!star || sscanf(star + 1, "%2x", &emb) != 1 || (unsigned char)emb != xsum(s)) {
        fprintf(stderr, "FAIL %s : checksum/framing incohérent : %s", what, s);
        failures++;
    }
    fputs(s, stdout);   /* échantillon (CRLF inclus) */
}

static const char *INI =
    "[sources]\n"
    "GPS=SGPS\nCMP=SCMP\nWND=SWND\nDST_BB=SBB\nDST_TB=STB\n"
    "RUD=SRUD\nLOG=SLOG\nTMP=STMP\nBAR=SBAR\n"
    "[priority]\n"
    "129025=GPS\n129026=GPS\n126992=GPS\n129029=GPS\n"
    "127250=CMP\n127251=CMP\n127257=CMP\n"
    "130306/Apparent=WND\n130306/True=WND\n"
    "127245=RUD\n128259=LOG\n129291=WND\n"
    "130316/Sea=TMP\n130316/Outside=TMP\n"
    "130312/Sea=TMP\n130312/Outside=TMP\n130314=BAR\n"
    "128267=min: DST_BB, DST_TB\n"
    "128275=max: DST_BB, DST_TB\n";

int main(void)
{
    if (!config_parse_string(&cfg, INI)) {
        fprintf(stderr, "config invalide (ligne %d : %s)\n", cfg.err_line, cfg.err);
        return 2;
    }
    registry_init(&reg);
    reg_add(2, "SGPS"); reg_add(7, "SCMP"); reg_add(8, "SWND");
    reg_add(12, "SBB"); reg_add(13, "STB"); reg_add(9, "SRUD");
    reg_add(10, "SLOG"); reg_add(11, "STMP"); reg_add(14, "SBAR");
    arbiter_init(&arb, &cfg, &reg);
    mapper_init(&mp, cfg.talker);

    map_out_t o;
    printf("=== Phrases produites ===\n");

    /* GPS (valeurs issues de la capture réelle Veratron) */
    o = run("{\"src\":2,\"pgn\":129025,\"fields\":{\"Latitude\":46.3107906,\"Longitude\":-0.9934259}}", 1000);
    chk("GLL", &o, 0, "$IIGLL,4618.6474,N,00059.6056,W,,A,A*");

    o = run("{\"src\":2,\"pgn\":129026,\"fields\":{\"COG Reference\":\"True\",\"COG\":141.5,\"SOG\":0.08}}", 1000);
    chk("VTG", &o, 0, "$IIVTG,141.5,T,,M,0.2,N,");

    o = run("{\"src\":2,\"pgn\":126992,\"fields\":{\"Date\":\"2026.06.15\",\"Time\":\"21:55:25.0000\"}}", 1000);
    chk("ZDA", &o, 0, "$IIZDA,215525.00,15,06,2026,00,00*");

    o = run("{\"src\":2,\"pgn\":129029,\"fields\":{\"Date\":\"2026.06.15\",\"Time\":\"21:55:25.0000\",\"Latitude\":46.3107902,\"Longitude\":-0.9934264,\"Altitude\":63.555,\"Method\":\"GNSS fix\",\"Number of SVs\":24,\"HDOP\":0.7,\"Geoidal Separation\":-47.95}}", 1000);
    chk("GGA", &o, 0, "$IIGGA,215525.00,4618.6474,N,00059.6056,W,1,24,0.7,");

    /* Cap magnétique → HDG + HDM ; cap vrai → HDT */
    o = run("{\"src\":7,\"pgn\":127250,\"fields\":{\"Heading\":312.5,\"Deviation\":-2.0,\"Variation\":1.5,\"Reference\":\"Magnetic\"}}", 1000);
    chk("HDG", &o, 0, "$IIHDG,312.5,2.0,W,1.5,E*");
    chk("HDM", &o, 1, "$IIHDM,312.5,M*");
    o = run("{\"src\":7,\"pgn\":127250,\"fields\":{\"Heading\":100.0,\"Reference\":\"True\"}}", 1000);
    chk("HDT", &o, 0, "$IIHDT,100.0,T*");

    /* ROT : 0.5 deg/s → 30 deg/min */
    o = run("{\"src\":7,\"pgn\":127251,\"fields\":{\"Rate\":0.5}}", 1000);
    chk("ROT", &o, 0, "$IIROT,30.0,A*");

    /* Attitude → XDR pitch/roll */
    o = run("{\"src\":7,\"pgn\":127257,\"fields\":{\"Pitch\":2.0,\"Roll\":-3.0}}", 1000);
    chk("XDR", &o, 0, "$IIXDR,A,2.0,D,PTCH,A,-3.0,D,ROLL*");

    /* Vent apparent → MWV(R) ; vent vrai → MWV(T) + MWD. 5 m/s → 9.7 kn */
    o = run("{\"src\":8,\"pgn\":130306,\"fields\":{\"Reference\":\"Apparent\",\"Wind Speed\":5.0,\"Wind Angle\":30.0}}", 1000);
    chk("MWV(R)", &o, 0, "$IIMWV,30.0,R,9.7,N,A*");
    o = run("{\"src\":8,\"pgn\":130306,\"fields\":{\"Reference\":\"True (ground referenced to North)\",\"Wind Speed\":5.0,\"Wind Angle\":215.0}}", 1000);
    chk("MWV(T)", &o, 0, "$IIMWV,215.0,T,9.7,N,A*");
    chk("MWD",    &o, 1, "$IIMWD,215.0,T,,M,9.7,N,5.0,M*");

    /* Courant (set & drift) → VDR. 0.72 m/s → 1.4 kn ; True → champ T rempli */
    o = run("{\"src\":8,\"pgn\":129291,\"fields\":{\"Set Reference\":\"True\",\"Set\":95.0,\"Drift\":0.72}}", 1000);
    chk("VDR", &o, 0, "$IIVDR,95.0,T,,M,1.4,N*");

    /* Barre, loch, températures, pression */
    o = run("{\"src\":9,\"pgn\":127245,\"fields\":{\"Position\":4.5}}", 1000);
    chk("RSA", &o, 0, "$IIRSA,4.5,A,,V*");
    o = run("{\"src\":10,\"pgn\":128259,\"fields\":{\"Speed Water Referenced\":3.0}}", 1000);
    chk("VHW", &o, 0, "$IIVHW,,T,,M,5.8,N,");
    o = run("{\"src\":11,\"pgn\":130316,\"fields\":{\"Source\":\"Sea Temperature\",\"Temperature\":18.5}}", 1000);
    chk("MTW", &o, 0, "$IIMTW,18.5,C*");
    o = run("{\"src\":11,\"pgn\":130316,\"fields\":{\"Source\":\"Outside Temperature\",\"Temperature\":21.0}}", 1000);
    chk("MDA(air)", &o, 0, "$IIMDA,,I,,B,21.0,C,");
    /* 130312 déprécié mais toujours accepté (champ "Actual Temperature") */
    o = run("{\"src\":11,\"pgn\":130312,\"fields\":{\"Source\":\"Sea Temperature\",\"Actual Temperature\":18.5}}", 1000);
    chk("MTW(130312)", &o, 0, "$IIMTW,18.5,C*");
    o = run("{\"src\":14,\"pgn\":130314,\"fields\":{\"Pressure\":1.013}}", 1000);
    chk("MDA(press)", &o, 0, "$IIMDA,29.9139,I,1.0130,B,");

    /* Profondeur min : DST_BB 3.2 puis DST_TB 2.8 → DPT à 2.8 */
    o = run("{\"src\":12,\"pgn\":128267,\"fields\":{\"Depth\":3.2,\"Offset\":0.5}}", 2000);
    chk("DPT (BB seul)", &o, 0, "$IIDPT,3.2,0.5*");
    o = run("{\"src\":13,\"pgn\":128267,\"fields\":{\"Depth\":2.8,\"Offset\":0.5}}", 2000);
    chk("DPT (min BB/TB)", &o, 0, "$IIDPT,2.8,0.5*");

    /* Loch max : TB en retard (9260 m = 5.0 NM) puis BB (18520 m = 10.0 NM,
     * 1852 m trip = 1.0 NM) → VLW garde le MAX même si le petit est arrivé avant */
    o = run("{\"src\":13,\"pgn\":128275,\"fields\":{\"Log\":9260,\"Trip Log\":926}}", 3000);
    chk("VLW (TB seul)", &o, 0, "$IIVLW,5.0,N,0.5,N,");
    o = run("{\"src\":12,\"pgn\":128275,\"fields\":{\"Log\":18520,\"Trip Log\":1852}}", 3000);
    chk("VLW (max BB/TB)", &o, 0, "$IIVLW,10.0,N,1.0,N,");

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
