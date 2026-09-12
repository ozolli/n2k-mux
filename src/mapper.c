/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

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
#define M_TO_NM   (1.0/1852.0)  /* mètres → milles nautiques */

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

/* Sous-chaîne, SANS tenir compte de la casse : les libellés canboat mélangent
 * les casses (« no GNSS » en minuscule), ce qui faisait échouer les tests. */
static bool has_word(const char *s, const char *w)
{
    return s && strcasestr(s, w) != NULL;
}

/* Lecture entière tolérante : `dflt` si le champ est absent. Un cast direct de
 * NaN vers int est un comportement INDÉFINI (selon l'optimisation : 0 ou
 * INT_MIN, qui sortait tel quel dans la phrase). */
static int geti(const jsonl_msg_t *m, const char *k, int dflt)
{
    double v;
    return jsonl_get_num(m, k, &v) ? (int)v : dflt;
}

/* Qualité de fix GGA depuis le champ "Method" du PGN 129029 (table GNS_METHOD
 * de canboat). Champ absent ou « no GNSS » → 0 (position INVALIDE) : c'est le
 * cas critique, un consommateur doit cesser de faire confiance à la position. */
static int gga_quality(const char *method)
{
    if (!method || !*method)            return 0;
    if (has_word(method, "no GNSS"))    return 0;
    if (has_word(method, "DGNSS") ||
        has_word(method, "SBAS"))       return 2;
    if (has_word(method, "Precise"))    return 3;
    if (has_word(method, "RTK Fixed"))  return 4;
    if (has_word(method, "RTK float"))  return 5;
    if (has_word(method, "Estimated"))  return 6;
    if (has_word(method, "Manual"))     return 7;
    if (has_word(method, "Simulate"))   return 8;
    return 1;                           /* "GNSS fix" et inconnus : fix simple */
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
    int    best_slot = -1;
    int    n = d->rule ? d->rule->n_sources : 0;
    for (int i = 0; i < n; i++) {
        if (!mp->depth_seen[i])
            continue;
        if ((now - mp->depth_t[i]) > mp->timeout_ms)
            continue;
        if (isnan(best) || mp->depth[i] < best) {
            best = mp->depth[i];
            best_slot = i;
        }
    }
    /* Le mode min accepte TOUTES les sources vivantes : sans ce garde-fou,
     * chacune émettait la même DPT (phrases identiques en double). Seule la
     * source qui porte le minimum émet ; en cas d'égalité, la 1re de la règle. */
    if (best_slot != slot)
        return NULL;
    return nmea_dpt(s, mp->talker, best, offset);
}

/* PGN 128275 : loch retenu = max des sources vivantes. Un DST810 sorti de
 * l'eau cesse de compter (sous-estime) → on garde le capteur le plus avancé.
 * Log et Trip Log proviennent du MÊME capteur (le max sur le Log) pour rester
 * cohérents. */
static const char *map_log_max(mapper_t *mp, const jsonl_msg_t *m,
                               const arb_decision_t *d, uint64_t now, nmea_t *s)
{
    double total = getf(m, "Log");
    double trip  = getf(m, "Trip Log");

    int slot = -1;
    if (d->rule && d->source_name)
        for (int i = 0; i < d->rule->n_sources; i++)
            if (strcmp(d->rule->sources[i], d->source_name) == 0) { slot = i; break; }
    if (slot < 0 || isnan(total))
        return NULL;

    mp->log_total[slot] = total;
    mp->log_trip[slot]  = trip;
    mp->log_t[slot]     = now;
    mp->log_seen[slot]  = true;

    /* capteur au Log maximal parmi les sources vues récemment */
    double best_total = NAN, best_trip = NAN;
    int    best_slot = -1;
    int    n = d->rule ? d->rule->n_sources : 0;
    for (int i = 0; i < n; i++) {
        if (!mp->log_seen[i])
            continue;
        if ((now - mp->log_t[i]) > mp->timeout_ms)
            continue;
        if (isnan(best_total) || mp->log_total[i] > best_total) {
            best_total = mp->log_total[i];
            best_trip  = mp->log_trip[i];
            best_slot  = i;
        }
    }
    /* Comme pour la profondeur : seule la source retenue émet, sinon la même
     * VLW sortait une fois par capteur vivant. */
    if (best_slot != slot)
        return NULL;
    return nmea_vlw(s, mp->talker,
                    isnan(best_total) ? NAN : best_total * M_TO_NM,
                    isnan(best_trip)  ? NAN : best_trip  * M_TO_NM);
}

/* MDA : la pression (130314) et la température d'air (130316/Outside) sont
 * portées par deux PGN, mais la table de conversion prévoit UNE seule phrase.
 * Chaque PGN rafraîchit sa valeur ; la phrase émise porte les deux si elles
 * sont fraîches. Sans cela, chaque PGN émettait sa MDA en laissant vide le
 * champ de l'autre, et le consommateur effaçait la valeur précédente.
 *
 * `is_press` dit lequel des deux PGN déclenche l'appel. Pour ne pas émettre
 * deux phrases identiques par cycle, c'est la pression qui cadence la MDA ; la
 * température ne la déclenche que si la pression manque (appareil absent du
 * bus ou silencieux), auquel cas elle cadence seule. */
static const char *map_mda(mapper_t *mp, uint64_t now, nmea_t *s,
                           double press_bar, double air_c, bool is_press)
{
    if (!isnan(press_bar)) {
        mp->mda_press = press_bar; mp->mda_press_t = now; mp->mda_press_seen = true;
    }
    if (!isnan(air_c)) {
        mp->mda_air = air_c; mp->mda_air_t = now; mp->mda_air_seen = true;
    }
    double p = (mp->mda_press_seen && now - mp->mda_press_t <= MAP_MDA_FRESH_MS)
             ? mp->mda_press : NAN;
    double t = (mp->mda_air_seen && now - mp->mda_air_t <= MAP_MDA_FRESH_MS)
             ? mp->mda_air : NAN;
    if (isnan(p) && isnan(t))
        return NULL;
    if (!is_press && !isnan(p))
        return NULL;              /* la pression cadence : pas de phrase en double */
    return nmea_mda(s, mp->talker, p, t);
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
        int quality = gga_quality(gets(m, "Method"));
        if (parse_time(gets(m, "Time"), &h, &mi, &se))
            emit(out, nmea_gga(&s, tk, h, mi, se,
                               getf(m, "Latitude"), getf(m, "Longitude"),
                               quality, geti(m, "Number of SVs", 0),
                               getf(m, "HDOP"), getf(m, "Altitude"),
                               getf(m, "Geoidal Separation")));
        break;
    }

    case 129539: { /* GNSS DOPs → GSA */
        char mode = has_word(gets(m, "Desired Mode"), "Auto") ? 'A' : 'M';
        const char *am = gets(m, "Actual Mode");
        int fix = 1;                               /* défaut : pas de fix */
        if (has_word(am, "3D"))      fix = 3;
        else if (has_word(am, "2D")) fix = 2;
        double h = getf(m, "HDOP"), v = getf(m, "VDOP");
        /* 129539 ne porte pas le PDOP : relation PDOP² = HDOP² + VDOP². */
        double p = (isnan(h) || isnan(v)) ? NAN : sqrt(h * h + v * v);
        emit(out, nmea_gsa(&s, tk, mode, fix, p, h, v));
        break;
    }

    case 129540: { /* GNSS Sats in View → GSV (4 satellites par phrase) */
        int n = jsonl_list_count(m);
        if (n <= 0)
            break;
        /* Le compte annoncé doit correspondre aux satellites RÉELLEMENT émis :
         * si la liste a été tronquée (JSONL_MAX_LIST), annoncer le chiffre du
         * PGN laisserait le consommateur attendre des satellites absents. */
        int in_view = geti(m, "Sats in View", n);
        if (in_view <= 0 || in_view > n)
            in_view = n;
        int total = (n + 3) / 4;          /* nb de phrases GSV (4 sats/phrase) */
        int idx = 0;
        for (int page = 0; page < total; page++) {
            int    prn[4];
            double el[4], az[4], sn[4];
            int k = 0;
            for (; k < 4 && idx < n; k++, idx++) {
                double v;
                prn[k] = jsonl_list_get_num(m, idx, "PRN", &v) ? (int)v : 0;
                el[k]  = jsonl_list_get_num(m, idx, "Elevation", &v) ? v : NAN;
                az[k]  = jsonl_list_get_num(m, idx, "Azimuth",   &v) ? v : NAN;
                sn[k]  = jsonl_list_get_num(m, idx, "SNR",       &v) ? v : NAN;
            }
            nmea_t sg;
            emit(out, nmea_gsv(&sg, tk, total, page + 1, in_view, prn, el, az, sn, k));
        }
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

    case 129291: { /* Set & Drift → VDR (courant) */
        double set   = getf(m, "Set");
        double drift = getf(m, "Drift");
        double kn    = isnan(drift) ? NAN : drift * MS_TO_KN;
        const char *ref = gets(m, "Set Reference");
        double dir_t = has_word(ref, "Magnetic") ? NAN : set;
        double dir_m = has_word(ref, "Magnetic") ? set : NAN;
        emit(out, nmea_vdr(&s, tk, dir_t, dir_m, kn));
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

    case 128275:  /* Distance Log → VLW (max des DST : le plus avancé) */
        emit(out, map_log_max(mp, m, d, now_ms, &s));
        break;

    case 130312:   /* Temperature (DÉPRÉCIÉ) — air (SCX/Outside) → MDA */
    case 130316: { /* Temperature Extended Range — eau (DST/Sea) → MTW */
        /* 130316 porte la valeur en "Temperature" (24 bits, 0.001 K) ;
           130312, déprécié, en "Actual Temperature" (16 bits). */
        double t = getf(m, m->pgn == 130316 ? "Temperature" : "Actual Temperature");
        if (has_word(d->discriminant, "Sea"))
            emit(out, nmea_mtw(&s, tk, t));
        else if (has_word(d->discriminant, "Outside"))
            emit(out, map_mda(mp, now_ms, &s, NMEA_NA, t, false));
        break;
    }

    case 130314:  /* Actual Pressure → MDA (pression + temp air mémorisée) */
        emit(out, map_mda(mp, now_ms, &s, getf(m, "Pressure"), NMEA_NA, true));
        break;

    case 128267:  /* Water Depth → DPT (minimum des DST) */
        emit(out, map_depth_min(mp, m, d, now_ms, &s));
        break;

    default:
        break;
    }

    return out->n;
}
