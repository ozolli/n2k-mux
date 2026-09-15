/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * nmea0183.c — Implémentation du générateur de phrases NMEA 0183.
 *
 * Le noyau écrit caractère par caractère dans le buffer fourni, en bornant
 * chaque ajout (drapeau `ok` levé en cas de dépassement). Les champs sont
 * toujours préfixés d'une virgule ; un champ absent se traduit par une
 * virgule seule. Le checksum est le XOR des octets entre '$' et '*'.
 */

#include "nmea0183.h"
#include <stdio.h>
#include <string.h>

/* ---- primitives bas niveau ---- */

static void put_c(nmea_t *s, char c)
{
    if (!s->ok)
        return;
    if (s->len + 1 >= sizeof s->buf) {   /* +1 : garder la place du NUL */
        s->ok = false;
        return;
    }
    s->buf[s->len++] = c;
    s->buf[s->len]   = '\0';
}

static void put_s(nmea_t *s, const char *str)
{
    if (!str)
        return;
    for (; *str; str++)
        put_c(s, *str);
}

void nmea_begin(nmea_t *s, const char *talker, const char *type)
{
    s->len = 0;
    s->ok  = true;
    s->buf[0] = '\0';
    put_c(s, '$');
    if (talker)
        for (int i = 0; i < 2 && talker[i]; i++)
            put_c(s, talker[i]);
    put_s(s, type);
}

void nmea_field_str(nmea_t *s, const char *v)
{
    put_c(s, ',');
    put_s(s, v);
}

void nmea_field_int(nmea_t *s, int v)
{
    char t[16];
    snprintf(t, sizeof t, "%d", v);
    put_c(s, ',');
    put_s(s, t);
}

void nmea_field_intw(nmea_t *s, int v, int width)
{
    char t[16];
    snprintf(t, sizeof t, "%0*d", width, v);
    put_c(s, ',');
    put_s(s, t);
}

void nmea_field_f(nmea_t *s, double v, int decimals)
{
    put_c(s, ',');
    if (isnan(v))
        return;
    char t[32];
    snprintf(t, sizeof t, "%.*f", decimals, v);
    put_s(s, t);
}

void nmea_field_char(nmea_t *s, char c)
{
    put_c(s, ',');
    if (c)
        put_c(s, c);
}

void nmea_field_empty(nmea_t *s)
{
    put_c(s, ',');
}

unsigned char nmea_checksum(const char *sentence)
{
    unsigned char ck = 0;
    const char *p = sentence;
    if (*p == '$' || *p == '!')
        p++;
    for (; *p && *p != '*'; p++)
        ck ^= (unsigned char)*p;
    return ck;
}

const char *nmea_end(nmea_t *s)
{
    if (!s->ok)
        return NULL;
    unsigned char ck = 0;
    for (size_t i = 1; i < s->len; i++)   /* sauter le '$' initial */
        ck ^= (unsigned char)s->buf[i];
    char t[8];
    snprintf(t, sizeof t, "*%02X\r\n", ck);
    put_s(s, t);
    return s->ok ? s->buf : NULL;
}

/* ---- helpers de champs composés ---- */

/* Champ valeur signée → "magnitude,E" / "magnitude,W" (ou ",," si absent). */
static void field_ew(nmea_t *s, double v, int decimals)
{
    if (isnan(v)) {
        nmea_field_empty(s);
        nmea_field_empty(s);
        return;
    }
    nmea_field_f(s, v < 0 ? -v : v, decimals);
    nmea_field_char(s, v >= 0 ? 'E' : 'W');
}

/* Angle → "d…dmm.mmmm,H" : `dw` chiffres de degrés, hémisphère `pos`/`neg`.
 * L'arrondi se fait sur l'angle ENTIER, en dix-millièmes de minute, AVANT de
 * séparer degrés et minutes. Arrondir les minutes seules sortait « 60.0000 »
 * (47,99999999° → 4760.0000 au lieu de 4800.0000), valeur invalide en 0183. */
static void field_angle(nmea_t *s, double v, int dw, char pos, char neg)
{
    if (isnan(v)) {
        nmea_field_empty(s);
        nmea_field_empty(s);
        return;
    }
    long long u   = llround((v < 0 ? -v : v) * 600000.0);   /* 1e-4 minute */
    long long deg = u / 600000;
    long long min = (u % 600000) / 10000;
    long long frac = (u % 600000) % 10000;
    char t[32];
    snprintf(t, sizeof t, ",%0*lld%02lld.%04lld,%c", dw, deg % 1000, min, frac,
             v >= 0 ? pos : neg);
    put_s(s, t);
}

/* Latitude → "ddmm.mmmm,N|S" (ou ",," si absente). */
static void field_lat(nmea_t *s, double lat)
{
    field_angle(s, lat, 2, 'N', 'S');
}

/* Longitude → "dddmm.mmmm,E|W" (ou ",," si absente). */
static void field_lon(nmea_t *s, double lon)
{
    field_angle(s, lon, 3, 'E', 'W');
}

/* Heure UTC → "hhmmss.ss" (ou champ vide si hh < 0). Les secondes sont
 * TRONQUÉES au centième, pas arrondies : 59,996 s donnait « 60.00 », invalide,
 * et reporter la retenue sur la minute ferait déborder l'heure et la date à
 * minuit. Un centième de moins est sans conséquence. */
static void field_time(nmea_t *s, int hh, int mm, double ss)
{
    if (hh < 0) {
        nmea_field_empty(s);
        return;
    }
    long long cs = (long long)floor(ss * 100.0 + 1e-6);
    if (cs < 0)    cs = 0;
    if (cs > 5999) cs = 5999;
    char t[24];
    snprintf(t, sizeof t, ",%02d%02d%02lld.%02lld", hh % 100, mm % 100, cs / 100, cs % 100);
    put_s(s, t);
}

/* ---- constructeurs de phrases ---- */

const char *nmea_hdg(nmea_t *s, const char *talker,
                     double heading_mag, double deviation, double variation)
{
    nmea_begin(s, talker, "HDG");
    nmea_field_f(s, heading_mag, 1);
    field_ew(s, deviation, 1);
    field_ew(s, variation, 1);
    return nmea_end(s);
}

const char *nmea_hdt(nmea_t *s, const char *talker, double heading_true)
{
    nmea_begin(s, talker, "HDT");
    nmea_field_f(s, heading_true, 1);
    nmea_field_char(s, 'T');
    return nmea_end(s);
}

const char *nmea_hdm(nmea_t *s, const char *talker, double heading_mag)
{
    nmea_begin(s, talker, "HDM");
    nmea_field_f(s, heading_mag, 1);
    nmea_field_char(s, 'M');
    return nmea_end(s);
}

const char *nmea_vtg(nmea_t *s, const char *talker,
                     double cog_true, double cog_mag, double sog_knots)
{
    double kmh = isnan(sog_knots) ? NMEA_NA : sog_knots * 1.852;
    nmea_begin(s, talker, "VTG");
    nmea_field_f(s, cog_true, 1);  nmea_field_char(s, 'T');
    nmea_field_f(s, cog_mag, 1);   nmea_field_char(s, 'M');
    nmea_field_f(s, sog_knots, 1); nmea_field_char(s, 'N');
    nmea_field_f(s, kmh, 1);       nmea_field_char(s, 'K');
    nmea_field_char(s, 'A');       /* mode : autonome */
    return nmea_end(s);
}

const char *nmea_gll(nmea_t *s, const char *talker,
                     double lat, double lon, int hh, int mm, double ss)
{
    nmea_begin(s, talker, "GLL");
    field_lat(s, lat);
    field_lon(s, lon);
    field_time(s, hh, mm, ss);
    nmea_field_char(s, 'A');   /* statut : valide */
    nmea_field_char(s, 'A');   /* mode : autonome */
    return nmea_end(s);
}

const char *nmea_gga(nmea_t *s, const char *talker, int hh, int mm, double ss,
                     double lat, double lon, int quality, int num_sats,
                     double hdop, double altitude, double geoidal_sep)
{
    nmea_begin(s, talker, "GGA");
    field_time(s, hh, mm, ss);
    field_lat(s, lat);
    field_lon(s, lon);
    nmea_field_int(s, quality);
    nmea_field_intw(s, num_sats, 2);
    nmea_field_f(s, hdop, 1);
    nmea_field_f(s, altitude, 1);    nmea_field_char(s, 'M');
    nmea_field_f(s, geoidal_sep, 1); nmea_field_char(s, 'M');
    nmea_field_empty(s);             /* âge des corrections DGPS */
    nmea_field_empty(s);             /* identifiant station DGPS */
    return nmea_end(s);
}

const char *nmea_gsa(nmea_t *s, const char *talker,
                     char mode, int fix, double pdop, double hdop, double vdop)
{
    nmea_begin(s, talker, "GSA");
    nmea_field_char(s, mode);
    nmea_field_int(s, fix);
    for (int i = 0; i < 12; i++)
        nmea_field_empty(s);          /* PRN des satellites (non portés par 129539) */
    nmea_field_f(s, pdop, 1);
    nmea_field_f(s, hdop, 1);
    nmea_field_f(s, vdop, 1);
    return nmea_end(s);
}

const char *nmea_gsv(nmea_t *s, const char *talker,
                     int total, int index, int in_view,
                     const int *prn, const double *elev,
                     const double *azim, const double *snr, int nsat)
{
    nmea_begin(s, talker, "GSV");
    nmea_field_int(s, total);
    nmea_field_int(s, index);
    nmea_field_intw(s, in_view, 2);
    for (int i = 0; i < nsat; i++) {
        nmea_field_intw(s, prn[i], 2);
        if (isnan(elev[i])) nmea_field_empty(s); else nmea_field_intw(s, (int)lround(elev[i]), 2);
        if (isnan(azim[i])) nmea_field_empty(s); else nmea_field_intw(s, (int)lround(azim[i]), 3);
        if (isnan(snr[i]))  nmea_field_empty(s); else nmea_field_intw(s, (int)lround(snr[i]),  2);
    }
    return nmea_end(s);
}

const char *nmea_mwv(nmea_t *s, const char *talker,
                     double angle, char reference, double speed, char unit)
{
    nmea_begin(s, talker, "MWV");
    nmea_field_f(s, angle, 1);
    nmea_field_char(s, reference);
    nmea_field_f(s, speed, 1);
    nmea_field_char(s, unit);
    nmea_field_char(s, 'A');   /* statut : valide */
    return nmea_end(s);
}

const char *nmea_mwd(nmea_t *s, const char *talker,
                     double dir_true, double dir_mag, double speed_knots)
{
    double ms = isnan(speed_knots) ? NMEA_NA : speed_knots / 1.943844;
    nmea_begin(s, talker, "MWD");
    nmea_field_f(s, dir_true, 1); nmea_field_char(s, 'T');
    nmea_field_f(s, dir_mag, 1);  nmea_field_char(s, 'M');
    nmea_field_f(s, speed_knots, 1); nmea_field_char(s, 'N');
    nmea_field_f(s, ms, 1);          nmea_field_char(s, 'M');
    return nmea_end(s);
}

const char *nmea_vdr(nmea_t *s, const char *talker,
                     double dir_true, double dir_mag, double drift_knots)
{
    nmea_begin(s, talker, "VDR");
    nmea_field_f(s, dir_true, 1); nmea_field_char(s, 'T');
    nmea_field_f(s, dir_mag, 1);  nmea_field_char(s, 'M');
    nmea_field_f(s, drift_knots, 1); nmea_field_char(s, 'N');
    return nmea_end(s);
}

const char *nmea_dpt(nmea_t *s, const char *talker, double depth, double offset)
{
    nmea_begin(s, talker, "DPT");
    nmea_field_f(s, depth, 1);
    nmea_field_f(s, offset, 1);
    return nmea_end(s);
}

const char *nmea_mtw(nmea_t *s, const char *talker, double temp_c)
{
    nmea_begin(s, talker, "MTW");
    nmea_field_f(s, temp_c, 1);
    nmea_field_char(s, 'C');
    return nmea_end(s);
}

const char *nmea_rot(nmea_t *s, const char *talker, double rate)
{
    nmea_begin(s, talker, "ROT");
    nmea_field_f(s, rate, 1);
    /* Statut : 'A' seulement si la valeur existe. Annoncer « valide » sur un
     * champ vide invite le consommateur à retenir une donnée qu'on n'a pas. */
    nmea_field_char(s, isnan(rate) ? 'V' : 'A');
    return nmea_end(s);
}

const char *nmea_rsa(nmea_t *s, const char *talker, double starboard, double port)
{
    nmea_begin(s, talker, "RSA");
    nmea_field_f(s, starboard, 1);
    nmea_field_char(s, isnan(starboard) ? 'V' : 'A');
    nmea_field_f(s, port, 1);
    nmea_field_char(s, isnan(port) ? 'V' : 'A');
    return nmea_end(s);
}

const char *nmea_vhw(nmea_t *s, const char *talker,
                     double heading_true, double heading_mag, double speed_knots)
{
    double kmh = isnan(speed_knots) ? NMEA_NA : speed_knots * 1.852;
    nmea_begin(s, talker, "VHW");
    nmea_field_f(s, heading_true, 1); nmea_field_char(s, 'T');
    nmea_field_f(s, heading_mag, 1);  nmea_field_char(s, 'M');
    nmea_field_f(s, speed_knots, 1);  nmea_field_char(s, 'N');
    nmea_field_f(s, kmh, 1);          nmea_field_char(s, 'K');
    return nmea_end(s);
}

const char *nmea_vlw(nmea_t *s, const char *talker,
                     double total_water_nm, double trip_water_nm)
{
    nmea_begin(s, talker, "VLW");
    nmea_field_f(s, total_water_nm, 1); nmea_field_char(s, 'N');
    nmea_field_f(s, trip_water_nm, 1);  nmea_field_char(s, 'N');
    /* champs sol (cumul + trajet) laissés vides : distance dans l'eau seule. */
    nmea_field_empty(s); nmea_field_empty(s);
    nmea_field_empty(s); nmea_field_empty(s);
    return nmea_end(s);
}

const char *nmea_xdr(nmea_t *s, const char *talker,
                     char type, double value, int decimals, char unit, const char *id)
{
    nmea_begin(s, talker, "XDR");
    nmea_field_char(s, type);
    nmea_field_f(s, value, decimals);
    nmea_field_char(s, unit);
    nmea_field_str(s, id);
    return nmea_end(s);
}

const char *nmea_xdr_attitude(nmea_t *s, const char *talker,
                              double pitch, double roll)
{
    /* Aucun des deux axes : pas de phrase. Un "$IIXDR*hh" sans le moindre
     * champ n'apporte rien et son type n'est même pas extractible. */
    if (isnan(pitch) && isnan(roll))
        return NULL;
    nmea_begin(s, talker, "XDR");
    if (!isnan(pitch)) {
        nmea_field_char(s, 'A');
        nmea_field_f(s, pitch, 1);
        nmea_field_char(s, 'D');
        nmea_field_str(s, "PTCH");
    }
    if (!isnan(roll)) {
        nmea_field_char(s, 'A');
        nmea_field_f(s, roll, 1);
        nmea_field_char(s, 'D');
        nmea_field_str(s, "ROLL");
    }
    return nmea_end(s);
}

const char *nmea_mda(nmea_t *s, const char *talker,
                     double pressure_bar, double air_temp_c)
{
    double inhg = isnan(pressure_bar) ? NMEA_NA : pressure_bar * 29.529983;
    nmea_begin(s, talker, "MDA");
    nmea_field_f(s, inhg, 4);        nmea_field_char(s, 'I');   /* pression inHg */
    nmea_field_f(s, pressure_bar, 4); nmea_field_char(s, 'B');  /* pression bar */
    nmea_field_f(s, air_temp_c, 1);  nmea_field_char(s, 'C');   /* temp air */
    nmea_field_empty(s);             nmea_field_char(s, 'C');   /* temp eau */
    nmea_field_empty(s);                                        /* humidité rel. */
    nmea_field_empty(s);                                        /* humidité abs. */
    nmea_field_empty(s);             nmea_field_char(s, 'C');   /* point de rosée */
    nmea_field_empty(s);             nmea_field_char(s, 'T');   /* dir vent vrai */
    nmea_field_empty(s);             nmea_field_char(s, 'M');   /* dir vent mag */
    nmea_field_empty(s);             nmea_field_char(s, 'N');   /* vent nœuds */
    nmea_field_empty(s);             nmea_field_char(s, 'M');   /* vent m/s */
    return nmea_end(s);
}

const char *nmea_rmc(nmea_t *s, const char *talker, int hh, int mm, double ss,
                     bool valid, double lat, double lon,
                     double sog_knots, double cog_true,
                     int day, int month, int year, double variation)
{
    nmea_begin(s, talker, "RMC");
    field_time(s, hh, mm, ss);
    nmea_field_char(s, valid ? 'A' : 'V');
    field_lat(s, lat);
    field_lon(s, lon);
    nmea_field_f(s, sog_knots, 1);
    nmea_field_f(s, cog_true, 1);
    /* Date en ddmmyy (2 chiffres d'année), format imposé par la phrase. */
    if (day > 0 && month > 0 && year > 0) {
        char t[16];
        /* bornage par modulo : comme ailleurs dans le projet, il rassure
         * -Wformat-truncation sans changer le résultat sur des dates valides. */
        snprintf(t, sizeof t, ",%02d%02d%02d", day % 100, month % 100, year % 100);
        put_s(s, t);
    } else {
        nmea_field_empty(s);
    }
    field_ew(s, variation, 1);
    nmea_field_char(s, valid ? 'A' : 'N');   /* indicateur de mode (NMEA 2.3+) */
    return nmea_end(s);
}

const char *nmea_zda(nmea_t *s, const char *talker,
                     int hh, int mm, double ss, int day, int month, int year)
{
    nmea_begin(s, talker, "ZDA");
    field_time(s, hh, mm, ss);
    nmea_field_intw(s, day, 2);
    nmea_field_intw(s, month, 2);
    nmea_field_intw(s, year, 4);
    nmea_field_intw(s, 0, 2);   /* fuseau local : heures */
    nmea_field_intw(s, 0, 2);   /* fuseau local : minutes */
    return nmea_end(s);
}
