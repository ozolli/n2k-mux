/*
 * mapper.c — Implémentation du mapping PGN → phrases 0183.
 *
 * Un convertisseur par PGN. Chaque champ est lu via jsonl_get_* (valeur
 * absente → NaN → champ 0183 vide). Les conversions d'unités sont concentrées
 * ici (cf. mapper.h). La sortie est un petit jeu de phrases prêtes à écrire.
 */

#include "mapper.h"
#include "registry.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define MS_TO_KN  1.943844   /* m/s → nœuds (1 / 0.514444) */

/* Lecture tolérante : valeur numérique ou NaN si absente. */
static double getf(const jsonl_msg_t *m, const char *k)
{
    double v;
    return jsonl_get_num(m, k, &v) ? v : NAN;
}

static const char *gets(const jsonl_msg_t *m, const char *k)
{
    const char *v;
    return jsonl_get_str(m, k, &v) ? v : NULL;
}

static bool has_word(const char *s, const char *w)
{
    return s && strstr(s, w) != NULL;
}

/* "YYYY.MM.DD" → y/mo/d. */
static bool parse_date(const char *s, int *y, int *mo, int *d)
{
    return s && sscanf(s, "%d.%d.%d", y, mo, d) == 3;
}

/* "HH:MM:SS.ssss" → h/mi/s. */
static bool parse_time(const char *s, int *h, int *mi, double *se)
{
    return s && sscanf(s, "%d:%d:%lf", h, mi, se) == 3;
}

/* Ajoute une phrase (si non NULL) à la sortie. */
static void emit(map_out_t *o, const char *sentence)
{
    if (sentence && o->n < MAP_MAX_SENT) {
        size_t i = 0;
        for (; sentence[i] && i + 1 < NMEA_MAX_LEN; i++)
            o->s[o->n][i] = sentence[i];
        o->s[o->n][i] = '\0';
        o->n++;
    }
}

void mapper_init(mapper_t *mp, const char *talker)
{
    memset(mp, 0, sizeof *mp);
    snprintf(mp->talker, sizeof mp->talker, "%s", talker && talker[0] ? talker : "II");
    mp->timeout_ms = ARB_DEFAULT_TIMEOUT_MS;
}

/* PGN 128267 : profondeur retenue = min des sources vivantes. */
static const char *map_depth_min(mapper_t *mp, const jsonl_msg_t *m,
                                 const arb_decision_t *d, uint64_t now, nmea_t *s)
{
    double depth  = getf(m, "Depth");
    double offset = getf(m, "Offset");

    /* slot de la source courante dans la règle */
    int slot = -1;
    if (d->rule && d->source_name)
        for (int i = 0; i < d->rule->n_sources; i++)
            if (strcmp(d->rule->sources[i], d->source_name) == 0) { slot = i; break; }
    if (slot < 0 || isnan(depth))
        return NULL;

    mp->depth[slot]      = depth;
    mp->depth_t[slot]    = now;
    mp->depth_seen[slot] = true;

    /* minimum sur les sources vues récemment */
    double best = NAN;
    int    n = d->rule ? d->rule->n_sources : 0;
    for (int i = 0; i < n; i++) {
        if (!mp->depth_seen[i])
            continue;
        if ((now - mp->depth_t[i]) > mp->timeout_ms)
            continue;
        if (isnan(best) || mp->depth[i] < best)
            best = mp->depth[i];
    }
    return nmea_dpt(s, mp->talker, best, offset);
}

int mapper_map(mapper_t *mp, const jsonl_msg_t *m, const arb_decision_t *d,
               uint64_t now_ms, map_out_t *out)
{
    out->n = 0;
    if (!d || d->result != ARB_ACCEPT)
        return 0;

    nmea_t s;
    const char *tk = mp->talker;

    switch (m->pgn) {

    case 129025:  /* Position, Rapid Update → GLL (sans heure) */
        emit(out, nmea_gll(&s, tk, getf(m, "Latitude"), getf(m, "Longitude"), -1, 0, 0));
        break;

    case 129026: { /* COG & SOG → VTG */
        double cog = getf(m, "COG");
        double sog = getf(m, "SOG");
        double kn  = isnan(sog) ? NAN : sog * MS_TO_KN;
        const char *ref = gets(m, "COG Reference");
        double cog_t = has_word(ref, "Magnetic") ? NAN : cog;
        double cog_m = has_word(ref, "Magnetic") ? cog : NAN;
        emit(out, nmea_vtg(&s, tk, cog_t, cog_m, kn));
        break;
    }

    case 126992: { /* System Time → ZDA */
        int y, mo, dd, h, mi; double se;
        if (parse_date(gets(m, "Date"), &y, &mo, &dd) &&
            parse_time(gets(m, "Time"), &h, &mi, &se))
            emit(out, nmea_zda(&s, tk, h, mi, se, dd, mo, y));
        break;
    }

    case 129029: { /* GNSS Position Data → GGA */
        int h, mi; double se;
        const char *method = gets(m, "Method");
        int quality = 1;
        if (!method || has_word(method, "No"))      quality = 0;
        else if (has_word(method, "DGNSS") || has_word(method, "SBAS")) quality = 2;
        if (parse_time(gets(m, "Time"), &h, &mi, &se))
            emit(out, nmea_gga(&s, tk, h, mi, se,
                               getf(m, "Latitude"), getf(m, "Longitude"),
                               quality, (int)getf(m, "Number of SVs"),
                               getf(m, "HDOP"), getf(m, "Altitude"),
                               getf(m, "Geoidal Separation")));
        break;
    }

    case 127250: { /* Vessel Heading → HDG+HDM (magnétique) ou HDT (vrai) */
        double head = getf(m, "Heading");
        const char *ref = gets(m, "Reference");
        if (has_word(ref, "True")) {
            emit(out, nmea_hdt(&s, tk, head));
        } else { /* Magnetic (défaut) */
            emit(out, nmea_hdg(&s, tk, head, getf(m, "Deviation"), getf(m, "Variation")));
            nmea_t s2;
            emit(out, nmea_hdm(&s2, tk, head));
        }
        break;
    }

    case 127251:  /* Rate of Turn → ROT (deg/s → deg/min) */
    {
        double r = getf(m, "Rate");
        emit(out, nmea_rot(&s, tk, isnan(r) ? NAN : r * 60.0));
        break;
    }

    case 127257:  /* Attitude → XDR (pitch + roll) */
        emit(out, nmea_xdr_attitude(&s, tk, getf(m, "Pitch"), getf(m, "Roll")));
        break;

    case 130306: { /* Wind Data → MWV(R) apparent, ou MWV(T)+MWD vrai */
        double wa = getf(m, "Wind Angle");
        double ws = getf(m, "Wind Speed");
        double kn = isnan(ws) ? NAN : ws * MS_TO_KN;
        const char *ref = gets(m, "Reference");
        if (has_word(ref, "True")) {
            emit(out, nmea_mwv(&s, tk, wa, 'T', kn, 'N'));
            nmea_t s2;
            emit(out, nmea_mwd(&s2, tk, wa, kn));
        } else { /* Apparent */
            emit(out, nmea_mwv(&s, tk, wa, 'R', kn, 'N'));
        }
        break;
    }

    case 127245:  /* Rudder → RSA */
        emit(out, nmea_rsa(&s, tk, getf(m, "Position"), NMEA_NA));
        break;

    case 128259: { /* Speed → VHW (vitesse surface) */
        double stw = getf(m, "Speed Water Referenced");
        emit(out, nmea_vhw(&s, tk, NMEA_NA, NMEA_NA, isnan(stw) ? NAN : stw * MS_TO_KN));
        break;
    }

    case 130312: { /* Temperature → MTW (eau) ou MDA (air) selon discriminant */
        double t = getf(m, "Actual Temperature");
        if (has_word(d->discriminant, "Sea"))
            emit(out, nmea_mtw(&s, tk, t));
        else if (has_word(d->discriminant, "Outside"))
            emit(out, nmea_mda(&s, tk, NMEA_NA, t));
        break;
    }

    case 130314:  /* Actual Pressure → MDA (pression) */
        emit(out, nmea_mda(&s, tk, getf(m, "Pressure"), NMEA_NA));
        break;

    case 128267:  /* Water Depth → DPT (minimum des DST) */
        emit(out, map_depth_min(mp, m, d, now_ms, &s));
        break;

    default:
        break;
    }

    return out->n;
}
