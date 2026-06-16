/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * nmea0183.h — Générateur de phrases NMEA 0183 (+ checksum).
 *
 * Module (c). Deux couches :
 *   1. Noyau : assemblage d'une phrase "$ttSSS,champ,champ,...*hh\r\n",
 *      calcul du checksum XOR, formateurs de champs (flottants, lat/lon,
 *      heure, entiers zéro-paddés).
 *   2. Constructeurs de phrases prêts à l'emploi pour la table de conversion
 *      PGN → 0183 (HDG, HDT, HDM, VTG, GLL, MWV, DPT, MTW, ROT, RSA, VHW,
 *      XDR, MDA, ZDA).
 *
 * Comme le reste du projet : zéro allocation dynamique. L'appelant fournit un
 * `nmea_t` (buffer fixe). Les constructeurs retournent un pointeur vers la
 * phrase complète (terminée par CRLF, prête à écrire sur le flux 0183) ou NULL
 * en cas de dépassement.
 *
 * Convention "valeur absente" : passer NMEA_NA (NaN) pour un champ flottant
 * laisse ce champ vide dans la phrase. Pour l'heure, hh < 0 = champ vide.
 * Variation/déviation : valeur SIGNÉE, Est positif → magnitude + 'E'/'W'.
 */

#ifndef N2KMUX_NMEA0183_H
#define N2KMUX_NMEA0183_H

#include <stdbool.h>
#include <stddef.h>
#include <math.h>

/* Phrase 0183 standard : 82 octets max (CRLF inclus). Marge de sécurité. */
#define NMEA_MAX_LEN   100

/* Sentinelle "champ flottant absent". */
#define NMEA_NA        ((double)NAN)

/* Talker par défaut (qtVlm ignore le talker ; II = Integrated Instruments). */
#define NMEA_TALKER    "II"

/* Phrase en cours de construction (fournie par l'appelant). */
typedef struct {
    char   buf[NMEA_MAX_LEN];
    size_t len;     /* longueur courante (avant *hh\r\n) */
    bool   ok;      /* false si un dépassement a eu lieu */
} nmea_t;

/* ---- Noyau ---- */

/* Démarre une phrase : "$" + talker (2 car.) + type. Réinitialise s. */
void nmea_begin(nmea_t *s, const char *talker, const char *type);

/* Ajoute un champ (chacun préfixé d'une virgule). */
void nmea_field_str(nmea_t *s, const char *v);          /* chaîne (NULL → vide) */
void nmea_field_int(nmea_t *s, int v);                  /* entier décimal */
void nmea_field_intw(nmea_t *s, int v, int width);      /* entier zéro-paddé */
void nmea_field_f(nmea_t *s, double v, int decimals);   /* flottant ; NMEA_NA → vide */
void nmea_field_char(nmea_t *s, char c);                /* 1 caractère (0 → vide) */
void nmea_field_empty(nmea_t *s);                       /* champ vide */

/* Termine la phrase : ajoute "*hh\r\n". Retourne s->buf, ou NULL si dépassement. */
const char *nmea_end(nmea_t *s);

/* Checksum XOR des octets entre '$' (exclu) et '*'/fin (exclu). */
unsigned char nmea_checksum(const char *sentence);

/* ---- Constructeurs de phrases (retournent s->buf complet, ou NULL) ---- */

/* Cap : magnétique + déviation + variation (signées, Est +). */
const char *nmea_hdg(nmea_t *s, const char *talker,
                     double heading_mag, double deviation, double variation);
/* Cap vrai / cap magnétique seuls. */
const char *nmea_hdt(nmea_t *s, const char *talker, double heading_true);
const char *nmea_hdm(nmea_t *s, const char *talker, double heading_mag);

/* Route/vitesse fond : COG vrai, COG magnétique, SOG (nœuds). km/h calculé. */
const char *nmea_vtg(nmea_t *s, const char *talker,
                     double cog_true, double cog_mag, double sog_knots);

/* Position : lat/lon en degrés décimaux (Nord/Est +), heure UTC. */
const char *nmea_gll(nmea_t *s, const char *talker,
                     double lat, double lon, int hh, int mm, double ss);

/* Position GNSS complète : heure UTC, lat/lon, qualité de fix (0/1/2),
 * nb satellites, HDOP, altitude (m), séparation géoïdale (m). */
const char *nmea_gga(nmea_t *s, const char *talker, int hh, int mm, double ss,
                     double lat, double lon, int quality, int num_sats,
                     double hdop, double altitude, double geoidal_sep);

/* Vent : angle (deg), reference 'R' (apparent) ou 'T' (vrai), vitesse,
 * unite 'N' (nœuds) / 'K' (km/h) / 'M' (m/s). */
const char *nmea_mwv(nmea_t *s, const char *talker,
                     double angle, char reference, double speed, char unit);

/* Direction/vitesse du vent vrai/sol : direction vraie (deg) + vitesse (nœuds).
 * km/h... non : émet vitesse en nœuds et en m/s. */
const char *nmea_mwd(nmea_t *s, const char *talker,
                     double dir_true, double speed_knots);

/* Profondeur sous capteur (m) + offset capteur→surface/quille (m). */
const char *nmea_dpt(nmea_t *s, const char *talker, double depth, double offset);

/* Température eau (°C). */
const char *nmea_mtw(nmea_t *s, const char *talker, double temp_c);

/* Taux de giration (deg/min, tribord +). */
const char *nmea_rot(nmea_t *s, const char *talker, double rate);

/* Angle de barre : tribord + bâbord (deg). Bâbord NMEA_NA → champ vide. */
const char *nmea_rsa(nmea_t *s, const char *talker, double starboard, double port);

/* Vitesse/cap surface : cap vrai, cap mag, vitesse surface (nœuds). */
const char *nmea_vhw(nmea_t *s, const char *talker,
                     double heading_true, double heading_mag, double speed_knots);

/* XDR générique : un quadruplet (type, valeur, unité, identifiant). */
const char *nmea_xdr(nmea_t *s, const char *talker,
                     char type, double value, int decimals, char unit, const char *id);
/* XDR attitude : pitch + roll (deg). Chaque champ NMEA_NA est omis. */
const char *nmea_xdr_attitude(nmea_t *s, const char *talker,
                              double pitch, double roll);

/* Composite météo : pression (bar, inHg calculé) + température air (°C). */
const char *nmea_mda(nmea_t *s, const char *talker,
                     double pressure_bar, double air_temp_c);

/* Date/heure UTC. */
const char *nmea_zda(nmea_t *s, const char *talker,
                     int hh, int mm, double ss, int day, int month, int year);

#endif /* N2KMUX_NMEA0183_H */
