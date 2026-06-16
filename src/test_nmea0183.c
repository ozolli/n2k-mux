/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_nmea0183.c — Vérifie le générateur de phrases NMEA 0183.
 *
 *  - Ancres externes/connues : checksum d'une phrase GLL de référence, et
 *    deux phrases complètes calculées à la main (HDT, MTW).
 *  - Auto-cohérence : chaque phrase produite commence par '$', finit par
 *    CRLF, et son checksum embarqué correspond au XOR recalculé.
 *  - Échantillonnage : imprime une phrase de chaque type de la table.
 *
 * Sort non-zéro si une vérification échoue.
 */

#include "nmea0183.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

static void expect_eq(const char *what, const char *got, const char *want)
{
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  attendu : %s\n  obtenu  : %s\n",
                what, want, got ? got : "(NULL)");
        failures++;
    }
}

/* Vérifie framing ($...*hh\r\n) et cohérence du checksum embarqué. */
static void check_frame(const char *what, const char *s)
{
    if (!s) { fprintf(stderr, "FAIL %s : NULL\n", what); failures++; return; }
    size_t n = strlen(s);
    if (s[0] != '$') { fprintf(stderr, "FAIL %s : pas de '$'\n", what); failures++; }
    if (n < 5 || s[n-2] != '\r' || s[n-1] != '\n') {
        fprintf(stderr, "FAIL %s : pas de CRLF final\n", what); failures++; return;
    }
    const char *star = strrchr(s, '*');
    if (!star) { fprintf(stderr, "FAIL %s : pas de '*'\n", what); failures++; return; }
    unsigned int embedded = 0;
    if (sscanf(star + 1, "%2x", &embedded) != 1) {
        fprintf(stderr, "FAIL %s : checksum illisible\n", what); failures++; return;
    }
    unsigned char calc = nmea_checksum(s);
    if (calc != embedded) {
        fprintf(stderr, "FAIL %s : checksum %02X != %02X\n", what, embedded, calc);
        failures++;
    }
}

static void sample(const char *s)
{
    if (s) fputs(s, stdout);   /* contient déjà CRLF */
    else   puts("(NULL — dépassement)");
}

int main(void)
{
    nmea_t s;

    /* ---- ancres ---- */
    /* Exemple GLL public bien connu : checksum 0x31. */
    if (nmea_checksum("$GPGLL,4916.45,N,12311.12,W,225444,A*31") != 0x31) {
        fprintf(stderr, "FAIL checksum de référence (GLL) != 0x31\n");
        failures++;
    }
    expect_eq("HDT 123.4", nmea_hdt(&s, "II", 123.4), "$IIHDT,123.4,T*26\r\n");
    expect_eq("MTW 21.5",  nmea_mtw(&s, "II", 21.5),  "$IIMTW,21.5,C*15\r\n");

    /* ---- auto-cohérence + échantillon de toute la table ---- */
    printf("=== Échantillon de phrases générées ===\n");

    check_frame("HDG", nmea_hdg(&s, "II", 312.5, -2.0, 1.5)); sample(s.buf);
    check_frame("HDT", nmea_hdt(&s, "II", 313.8));            sample(s.buf);
    check_frame("HDM", nmea_hdm(&s, "II", 312.5));            sample(s.buf);
    check_frame("VTG", nmea_vtg(&s, "II", 287.3, 285.8, 6.4)); sample(s.buf);
    check_frame("GLL", nmea_gll(&s, "II", 47.5, -3.21, 14, 30, 12.5)); sample(s.buf);
    check_frame("GGA", nmea_gga(&s, "II", 14, 30, 12.5, 47.5, -3.21, 1, 9, 0.8, 63.5, -47.9)); sample(s.buf);
    check_frame("MWV", nmea_mwv(&s, "II", 42.0, 'R', 12.3, 'N')); sample(s.buf);
    check_frame("MWD", nmea_mwd(&s, "II", 215.0, 12.3));          sample(s.buf);
    check_frame("DPT", nmea_dpt(&s, "II", 18.7, 0.3));        sample(s.buf);
    check_frame("MTW", nmea_mtw(&s, "II", 21.5));             sample(s.buf);
    check_frame("ROT", nmea_rot(&s, "II", -4.2));             sample(s.buf);
    check_frame("RSA", nmea_rsa(&s, "II", -3.5, NMEA_NA));    sample(s.buf);
    check_frame("VHW", nmea_vhw(&s, "II", NMEA_NA, NMEA_NA, 6.2)); sample(s.buf);
    check_frame("XDR", nmea_xdr_attitude(&s, "II", 2.1, -3.4)); sample(s.buf);
    check_frame("MDA", nmea_mda(&s, "II", 1.013, 19.8));      sample(s.buf);
    check_frame("ZDA", nmea_zda(&s, "II", 14, 30, 12.5, 15, 6, 2026)); sample(s.buf);

    /* cas "valeur absente" : COG vrai manquant dans VTG */
    check_frame("VTG (COG vrai absent)", nmea_vtg(&s, "II", NMEA_NA, 285.8, 6.4));
    sample(s.buf);

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
