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

/* Latitude → "ddmm.mmmm,N|S" (ou ",," si absente). */
static void field_lat(nmea_t *s, double lat)
{
    if (isnan(lat)) {
        nmea_field_empty(s);
        nmea_field_empty(s);
        return;
    }
    double a = lat < 0 ? -lat : lat;
    int    d = (int)a;
    double m = (a - d) * 60.0;
    char   t[24];
    snprintf(t, sizeof t, ",%02d%07.4f,%c", d, m, lat >= 0 ? 'N' : 'S');
    put_s(s, t);
}

/* Longitude → "dddmm.mmmm,E|W" (ou ",," si absente). */
static void field_lon(nmea_t *s, double lon)
{
    if (isnan(lon)) {
        nmea_field_empty(s);
        nmea_field_empty(s);
        return;
    }
    double a = lon < 0 ? -lon : lon;
    int    d = (int)a;
    double m = (a - d) * 60.0;
    char   t[24];
    snprintf(t, sizeof t, ",%03d%07.4f,%c", d, m, lon >= 0 ? 'E' : 'W');
    put_s(s, t);
}

/* Heure UTC → "hhmmss.ss" (ou champ vide si hh < 0). */
static void field_time(nmea_t *s, int hh, int mm, double ss)
{
    if (hh < 0) {
        nmea_field_empty(s);
        return;
    }
    char t[20];
    snprintf(t, sizeof t, ",%02d%02d%05.2f", hh, mm, ss);
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
                     double dir_true, double speed_knots)
{
    double ms = isnan(speed_knots) ? NMEA_NA : speed_knots / 1.943844;
    nmea_begin(s, talker, "MWD");
    nmea_field_f(s, dir_true, 1); nmea_field_char(s, 'T');
    nmea_field_empty(s);         nmea_field_char(s, 'M');  /* dir magnétique */
    nmea_field_f(s, speed_knots, 1); nmea_field_char(s, 'N');
    nmea_field_f(s, ms, 1);          nmea_field_char(s, 'M');
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
    nmea_field_char(s, 'A');   /* statut : valide */
    return nmea_end(s);
}

const char *nmea_rsa(nmea_t *s, const char *talker, double starboard, double port)
{
    nmea_begin(s, talker, "RSA");
    nmea_field_f(s, starboard, 1);
    nmea_field_char(s, 'A');
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
