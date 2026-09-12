/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * simulator.c — n2k-sim : générateur de flux NMEA 2000 simulé (JSON-lines).
 *
 * Émet sur stdout le JSON façon `analyzer -json -nv` de canboat (un objet par
 * ligne) pour TOUS les PGN que n2k-mux comprend, plus les PGN d'identité
 * (60928 ISO Address Claim + 126996 Product Information) nécessaires pour que
 * l'arbitrage résolve src→identité→nom. Permet de tester toute la chaîne sans
 * matériel ni bus réel :
 *
 *   ./n2k-sim | ./n2k-mux n2k-sim.ini -v            (instruments → phrases 0183)
 *   ./n2k-sim | ./n2k-mux --ais-json n2k-sim.ini    (AIS → dédup par MMSI)
 *   ./n2k-sim --once | ./n2k-mux n2k-sim.ini        (un de chaque PGN, puis fin)
 *
 * Les valeurs varient dans le temps (sinusoïdes) pour un flux « vivant ». Les
 * sources simulées correspondent à n2k-sim.ini (mêmes Model Serial Code). Pas
 * de dépendance au reste du projet : un seul fichier, sortie texte.
 *
 * Usage : n2k-sim [--once] [--duration SEC] [--no-ais] [--tick MS]
 *                  [--control FICHIER]
 *
 * --control : pilotage À CHAUD par un petit fichier « clé = valeur », relu dès
 * que sa date de modification change (c'est ce que l'interface web écrit) :
 *
 *     enabled = 1        ; 0 = le simulateur n'émet RIEN (chaîne silencieuse)
 *     hdg     = 45       ; cap vrai, degrés          (auto = sinusoïde)
 *     stw     = 6.2      ; vitesse SURFACE, NŒUDS
 *     set     = 120      ; direction du courant (VERS laquelle il porte), degrés
 *     drift   = 1.0      ; vitesse du courant, NŒUDS
 *
 * puis le vent, défini par UNE de ces trois paires (priorité dans cet ordre) :
 *     awa = 40, aws = 18 ; vent APPARENT : angle/étrave + vitesse, NŒUDS
 *     twa = 55, tws = 15 ; vent VRAI par son angle/étrave + vitesse
 *     twd = 225, tws = 15 ; vent VRAI par sa direction (D'OÙ il vient)
 *
 * Toute clé absente ou à « auto » garde le comportement automatique.
 *
 * POURQUOI une seule paire : le triangle des vitesses lie le vent vrai,
 * l'apparent et le vecteur bateau. Imposer les trois à la fois serait
 * contradictoire ; on impose donc une paire et le reste est CALCULÉ :
 *   - route et vitesse fond (COG, SOG) = vecteur surface + vecteur courant ;
 *   - taux de giration = dérivée du CAP (nul si le cap est imposé) ;
 *   - les deux autres expressions du vent, dont le vent vrai référencé eau ;
 *   - la position s'intègre le long du COG ainsi obtenu.
 *
 * L'état déduit est publié en clair avec --state (même format), ce que
 * l'interface web affiche à côté des réglages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Adresses N2K simulées (doivent rester cohérentes avec n2k-sim.ini). */
#define SCX_SRC    4
#define VER_SRC    2
#define MAD_SRC    5
#define DSTBB_SRC 20
#define DSTTB_SRC 21
#define AIS_SRC   30
#define DH_SRC    31

static volatile sig_atomic_t g_stop = 0;
static void on_int(int s) { (void)s; g_stop = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Horodatage ISO-ish pour le champ "timestamp" (cosmétique : le mapper lit les
 * champs Date/Time, pas celui-ci). */
static void ts_now(char *buf, size_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    gmtime_r(&ts.tv_sec, &tmv);
    /* bornage (modulo) : rassure -Wformat-truncation, sans effet pratique */
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             (tmv.tm_year + 1900) % 10000, (tmv.tm_mon + 1) % 100, tmv.tm_mday % 100,
             tmv.tm_hour % 100, tmv.tm_min % 100, tmv.tm_sec % 100,
             (int)(ts.tv_nsec / 1000000) % 1000);
}

/* Date "YYYY.MM.DD" et Time "HH:MM:SS.ss" (UTC) pour 126992 / 129029. */
static void date_now(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tmv; gmtime_r(&t, &tmv);
    snprintf(buf, n, "%04d.%02d.%02d",
             (tmv.tm_year + 1900) % 10000, (tmv.tm_mon + 1) % 100, tmv.tm_mday % 100);
}
static void time_now(char *buf, size_t n)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv; gmtime_r(&ts.tv_sec, &tmv);
    snprintf(buf, n, "%02d:%02d:%02d.%02d",
             tmv.tm_hour % 100, tmv.tm_min % 100, tmv.tm_sec % 100,
             (int)(ts.tv_nsec / 10000000) % 100);
}

/* Émet une ligne JSON complète (enveloppe + champs déjà formatés). */
static void emit(int prio, int src, int pgn, const char *desc, const char *fields)
{
    char ts[40];
    ts_now(ts, sizeof ts);
    printf("{\"timestamp\":\"%s\",\"prio\":%d,\"src\":%d,\"dst\":255,"
           "\"pgn\":%d,\"description\":\"%s\",\"fields\":{%s}}\n",
           ts, prio, src, pgn, desc, fields);
}

/* --- État bateau cohérent ---------------------------------------------------
 * Position, route (COG) et cap (HDG) doivent être cohérents, sinon qtVlm dessine
 * le bateau « à reculons » (cap opposé au déplacement GPS). On intègre donc la
 * position VERS L'AVANT le long du COG, avec un cap ≈ COG (+ petite dérive). */
static struct {
    double lat, lon;   /* position courante (degrés) */
    double cog;        /* route fond (deg), CALCULÉE */
    double sog;        /* vitesse fond (m/s), CALCULÉE */
    double stw;        /* vitesse surface (m/s), ENTRÉE */
    double hdg;        /* cap vrai (deg), ENTRÉE */
    double rot;        /* taux de giration (deg/s) = dHDG/dt */
    double set;        /* direction du courant, vers laquelle il porte (deg) */
    double drift;      /* vitesse du courant (m/s) */
    double twd;        /* direction du vent VRAI, d'où il vient (deg) */
    double tws;        /* vitesse du vent vrai (m/s) */
    double twa;        /* angle du vent vrai / étrave (deg), = twd − hdg */
    double awa;        /* angle du vent apparent / étrave (deg) */
    double aws;        /* vitesse du vent apparent (m/s) */
    double last_t;     /* horodatage du dernier pas (s) */
    int    init;
} boat;

/* --- Pilotage à chaud (--control, écrit par l'interface web) ---------------
 * Valeurs NAN = « auto » : le générateur sinusoïdal reprend la main. Les
 * vitesses sont en NŒUDS dans le fichier (unité de l'utilisateur) et converties
 * en m/s ici, comme le reste du simulateur. */
#define KN_TO_MS 0.514444

typedef struct {
    int    enabled;
    double hdg, stw, set, drift;        /* bateau et courant */
    double twd, tws, twa, awa, aws;     /* vent : une paire suffit */
} simctl_t;

#define CTL_INIT { 1, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN }

static simctl_t g_ctl = CTL_INIT;
static const char *g_ctl_path = NULL;
static const char *g_state_path = NULL;

/* Lit « clé = valeur » ; « auto » ou clé absente → NAN. Tolérant : une ligne
 * incomprise est ignorée, un fichier illisible laisse l'état inchangé. */
static void ctl_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    simctl_t c = CTL_INIT;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        for (char *p = line; *p; p++)
            if (*p == ';' || *p == '#') { *p = '\0'; break; }
        char key[32] = "", val[64] = "";
        if (sscanf(line, " %31[A-Za-z_] = %63s", key, val) != 2)
            continue;
        for (char *p = key; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p += 32;
        double v = (strcmp(val, "auto") == 0) ? NAN : atof(val);
        if      (strcmp(key, "enabled") == 0)
            c.enabled = (strcmp(val, "0") && strcmp(val, "false") &&
                         strcmp(val, "off") && strcmp(val, "no"));
        else if (strcmp(key, "hdg") == 0)   c.hdg = v;
        else if (strcmp(key, "stw") == 0)   c.stw = isnan(v) ? v : v * KN_TO_MS;
        else if (strcmp(key, "set") == 0)   c.set = v;
        else if (strcmp(key, "drift") == 0) c.drift = isnan(v) ? v : v * KN_TO_MS;
        else if (strcmp(key, "twd") == 0)   c.twd = v;
        else if (strcmp(key, "tws") == 0)   c.tws = isnan(v) ? v : v * KN_TO_MS;
        else if (strcmp(key, "twa") == 0)   c.twa = v;
        else if (strcmp(key, "awa") == 0)   c.awa = v;
        else if (strcmp(key, "aws") == 0)   c.aws = isnan(v) ? v : v * KN_TO_MS;
    }
    fclose(f);
    g_ctl = c;
}

/* Relit le fichier de contrôle si sa date ou sa taille a changé, au plus une
 * fois par tour de boucle. Retourne 1 si l'état a été rechargé. */
static int ctl_refresh(void)
{
    static time_t last_mt; static long last_sz = -1;
    if (!g_ctl_path)
        return 0;
    struct stat sb;
    if (stat(g_ctl_path, &sb) != 0)
        return 0;
    if (sb.st_mtime == last_mt && (long)sb.st_size == last_sz)
        return 0;
    last_mt = sb.st_mtime;
    last_sz = (long)sb.st_size;
    ctl_load(g_ctl_path);
    return 1;
}

/* Normalise un cap dans [0, 360). */
static double norm360(double a)
{
    while (a < 0.0)    a += 360.0;
    while (a >= 360.0) a -= 360.0;
    return a;
}

/* Composantes (nord, est) d'un vecteur donné par un cap et un module. */
static void vec_of(double dir_deg, double mag, double *n, double *e)
{
    double r = dir_deg * M_PI / 180.0;
    *n = mag * cos(r);
    *e = mag * sin(r);
}

/* Cap et module d'un vecteur (nord, est). */
static void dir_of(double n, double e, double *dir_deg, double *mag)
{
    *mag = sqrt(n * n + e * e);
    *dir_deg = (*mag < 1e-9) ? 0.0 : norm360(atan2(e, n) * 180.0 / M_PI);
}

static void boat_update(double t)
{
    if (!boat.init) { boat.lat = 47.5000; boat.lon = -3.0000; boat.last_t = t; boat.init = 1; }
    double dt = t - boat.last_t;
    boat.last_t = t;
    if (dt < 0) dt = 0;

    /* --- ENTRÉES : cap et vitesse SURFACE (ce que barre et lit l'équipage),
     * plus le courant. Chacune vient du fichier de contrôle si elle y est
     * fixée, sinon d'une sinusoïde. --- */
    if (isnan(g_ctl.hdg)) {
        boat.hdg = norm360(90.0 + 55.0 * sin(t / 70.0));   /* vire en S */
        boat.rot = (55.0 / 70.0) * cos(t / 70.0);          /* dérivée du CAP */
    } else {
        boat.hdg = norm360(g_ctl.hdg);
        boat.rot = 0.0;                                    /* cap imposé */
    }
    boat.stw   = isnan(g_ctl.stw)   ? 4.5 + 1.0 * sin(t / 40.0) : g_ctl.stw;
    boat.set   = isnan(g_ctl.set)   ? norm360(120.0 + 10.0 * sin(t / 40.0))
                                    : norm360(g_ctl.set);
    boat.drift = isnan(g_ctl.drift) ? 0.5 + 0.2 * sin(t / 25.0) : g_ctl.drift;
    if (boat.stw < 0)   boat.stw = 0;
    if (boat.drift < 0) boat.drift = 0;

    /* --- CALCULÉ : route/vitesse FOND = vecteur surface + vecteur courant.
     * C'est le triangle des vitesses, dans le sens où l'équipage le vit. --- */
    double wn, we, cn, ce, gn, ge;
    vec_of(boat.hdg, boat.stw,   &wn, &we);
    vec_of(boat.set, boat.drift, &cn, &ce);
    gn = wn + cn;
    ge = we + ce;
    dir_of(gn, ge, &boat.cog, &boat.sog);

    /* --- VENT : une seule paire imposée, les autres expressions calculées.
     * Priorité à l'apparent (la grandeur réellement mesurée à bord), puis à
     * l'angle vrai, puis à la direction vraie. --- */
    if (!isnan(g_ctl.awa) && !isnan(g_ctl.aws)) {
        boat.awa = norm360(g_ctl.awa);
        boat.aws = g_ctl.aws < 0 ? 0 : g_ctl.aws;
        /* on remonte au vent vrai : air vu du bateau + vecteur bateau/fond */
        double rn, re;
        vec_of(norm360(boat.hdg + boat.awa + 180.0), boat.aws, &rn, &re);
        double dir, mag;
        dir_of(rn + gn, re + ge, &dir, &mag);
        boat.twd = norm360(dir + 180.0);
        boat.tws = mag;
    } else {
        if (!isnan(g_ctl.twa))
            boat.twd = norm360(boat.hdg + g_ctl.twa);
        else
            boat.twd = isnan(g_ctl.twd) ? norm360(225.0 + 15.0 * sin(t / 60.0))
                                        : norm360(g_ctl.twd);
        boat.tws = isnan(g_ctl.tws) ? 9.0 + 2.0 * sin(t / 8.0) : g_ctl.tws;
        if (boat.tws < 0) boat.tws = 0;
        /* apparent = vent vrai (mouvement de l'air) − vecteur bateau/fond */
        double an, ae;
        vec_of(norm360(boat.twd + 180.0), boat.tws, &an, &ae);
        double dir, mag;
        dir_of(an - gn, ae - ge, &dir, &mag);
        boat.awa = norm360(dir + 180.0 - boat.hdg);
        boat.aws = mag;
    }
    boat.twa = norm360(boat.twd - boat.hdg);

    /* avance le long du COG (1° lat ≈ 111320 m) */
    boat.lat += (gn * dt) / 111320.0;
    boat.lon += (ge * dt) / (111320.0 * cos(boat.lat * M_PI / 180.0));
}

/* Publie l'état DÉDUIT (--state) : même format « clé = valeur », unités de
 * l'utilisateur. L'interface web l'affiche à côté des réglages, pour qu'on voie
 * ce que le triangle donne sans lire le flux NMEA. Écriture atomique. */
static void state_write(void)
{
    if (!g_state_path)
        return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_state_path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f,
            "# n2k-sim : état déduit (lecture seule)\n"
            "enabled = %d\nhdg = %.1f\nstw = %.2f\ncog = %.1f\nsog = %.2f\n"
            "set = %.1f\ndrift = %.2f\ntwd = %.1f\ntws = %.2f\ntwa = %.1f\n"
            "awa = %.1f\naws = %.2f\nlat = %.6f\nlon = %.6f\n",
            g_ctl.enabled, boat.hdg, boat.stw / KN_TO_MS, boat.cog,
            boat.sog / KN_TO_MS, boat.set, boat.drift / KN_TO_MS,
            boat.twd, boat.tws / KN_TO_MS, boat.twa, boat.awa,
            boat.aws / KN_TO_MS, boat.lat, boat.lon);
    fclose(f);
    if (rename(tmp, g_state_path) != 0)
        unlink(tmp);
}

/* --- Vent : le vrai (twd/tws) est l'entrée, l'apparent et le « vrai eau » en
 * découlent. Convention : twd = direction D'OÙ vient le vent. --- */

/* Vent vrai RÉFÉRENCÉ EAU : vent vrai moins le courant, ramené à l'étrave.
 * C'est ce que calcule une centrale à partir de l'apparent et du loch. */
static void wind_true_water(double *ta, double *ts)
{
    double wn, we, cn, ce;
    vec_of(norm360(boat.twd + 180.0), boat.tws, &wn, &we);
    vec_of(boat.set, boat.drift, &cn, &ce);
    double dir, mag;
    dir_of(wn - cn, we - ce, &dir, &mag);
    *ta = norm360(dir + 180.0 - boat.hdg);
    *ts = mag;
}

/* --- Identité : 60928 (Unique Number) + 126996 (Model Serial Code) --- */
static void identity(int src, unsigned uniq, const char *mfg,
                     const char *model, const char *serial)
{
    char f[256];
    snprintf(f, sizeof f,
             "\"Unique Number\":%u,\"Manufacturer Code\":\"%s\","
             "\"Device Function\":130,\"Device Class\":\"%s\"",
             uniq, mfg, "Navigation");
    emit(6, src, 60928, "ISO Address Claim", f);
    snprintf(f, sizeof f,
             "\"NMEA 2000 Version\":2100,\"Model ID\":\"%s\","
             "\"Model Serial Code\":\"%s\",\"Model Version\":\"1.0\"",
             model, serial);
    emit(6, src, 126996, "Product Information", f);
}

static void e_identity(double t)
{
    (void)t;
    identity(SCX_SRC,   2000004, "Furuno",      "SCX-20",      "SCX20-SIM");
    identity(VER_SRC,    917661, "Veratron",    "Veratron GO", "917661");
    identity(MAD_SRC,   2000005, "Madintec",    "MADBrain",    "MAD-SIM");
    identity(DSTBB_SRC, 2000020, "Airmar",      "DST810",      "DSTBB-SIM");
    identity(DSTTB_SRC, 2000021, "Airmar",      "DST810",      "DSTTB-SIM");
    identity(AIS_SRC,   2000030, "em-trak",     "B953",        "EMTRAK-SIM");
    identity(DH_SRC,    2000031, "PredictWind", "DataHub",     "DH-SIM");
}

/* --- Instruments (un PGN par fonction, valeurs fonction du temps t) --- */

static void e_pos(double t)   /* 129025 → GLL */
{
    (void)t;
    char f[160];
    snprintf(f, sizeof f, "\"Latitude\":%.7f,\"Longitude\":%.7f", boat.lat, boat.lon);
    emit(2, SCX_SRC, 129025, "Position, Rapid Update", f);
}

static void e_cogsog(double t)   /* 129026 → VTG */
{
    (void)t;
    char f[160];
    snprintf(f, sizeof f,
             "\"COG Reference\":\"True\",\"COG\":%.1f,\"SOG\":%.2f", boat.cog, boat.sog);
    emit(2, SCX_SRC, 129026, "COG & SOG, Rapid Update", f);
}

static void e_systime(double t)   /* 126992 → ZDA */
{
    (void)t;
    char f[96], d[16], tm[20];
    date_now(d, sizeof d); time_now(tm, sizeof tm);
    snprintf(f, sizeof f, "\"Source\":\"GPS\",\"Date\":\"%s\",\"Time\":\"%s\"", d, tm);
    emit(3, SCX_SRC, 126992, "System Time", f);
}

static void e_gnss(double t)   /* 129029 → GGA */
{
    (void)t;
    char f[256], d[16], tm[20];
    date_now(d, sizeof d); time_now(tm, sizeof tm);
    snprintf(f, sizeof f,
             "\"Date\":\"%s\",\"Time\":\"%s\",\"Latitude\":%.7f,\"Longitude\":%.7f,"
             "\"Method\":\"GNSS fix\",\"Number of SVs\":9,\"HDOP\":0.8,"
             "\"Altitude\":12.3,\"Geoidal Separation\":47.0",
             d, tm, boat.lat, boat.lon);
    emit(3, SCX_SRC, 129029, "GNSS Position Data", f);
}

static void e_dops(double t)   /* 129539 → GSA */
{
    (void)t;
    emit(6, SCX_SRC, 129539, "GNSS DOPs",
         "\"Desired Mode\":\"Auto\",\"Actual Mode\":\"3D\",\"HDOP\":0.8,\"VDOP\":1.2");
}

static void e_gsv(double t)   /* 129540 → GSV (8 sats, paginé 4/phrase) */
{
    char list[640]; size_t p = 0;
    p += snprintf(list + p, sizeof list - p, "[");
    for (int i = 0; i < 8; i++) {
        int prn = 1 + i;
        double el = 20 + 60.0 * fabs(sin((t + i * 7) / 40.0));
        double az = fmod(i * 45 + t * 2, 360.0);
        double sn = 30 + 12.0 * fabs(sin((t + i * 3) / 20.0));
        p += snprintf(list + p, sizeof list - p,
                      "%s{\"PRN\":%d,\"Elevation\":%.0f,\"Azimuth\":%.0f,\"SNR\":%.0f}",
                      i ? "," : "", prn, el, az, sn);
    }
    snprintf(list + p, sizeof list - p, "]");
    char f[768];
    snprintf(f, sizeof f, "\"Sats in View\":8,\"list\":%s", list);
    emit(6, SCX_SRC, 129540, "GNSS Sats in View", f);
}

static void e_heading(double t)   /* 127250 → HDG/HDM (mag) + HDT (vrai) */
{
    (void)t;
    char f[160];
    /* cap magnétique = cap vrai − variation (variation 2°W = −2) */
    double hdg_mag = norm360(boat.hdg + 2.0);
    snprintf(f, sizeof f,
             "\"Heading\":%.1f,\"Deviation\":1.5,\"Variation\":-2.0,\"Reference\":\"Magnetic\"",
             hdg_mag);
    emit(2, SCX_SRC, 127250, "Vessel Heading", f);
    snprintf(f, sizeof f, "\"Heading\":%.1f,\"Reference\":\"True\"", boat.hdg);
    emit(2, SCX_SRC, 127250, "Vessel Heading", f);
}

static void e_rot(double t)   /* 127251 → ROT (deg/s) */
{
    (void)t;
    char f[96];
    snprintf(f, sizeof f, "\"Rate\":%.3f", boat.rot);   /* deg/s, = dCOG/dt */
    emit(2, SCX_SRC, 127251, "Rate of Turn", f);
}

static void e_attitude(double t)   /* 127257 → XDR (pitch/roll) */
{
    char f[128];
    snprintf(f, sizeof f, "\"Yaw\":0.0,\"Pitch\":%.1f,\"Roll\":%.1f",
             3.0 * sin(t / 5.0), 8.0 * sin(t / 7.0));
    emit(2, SCX_SRC, 127257, "Attitude", f);
}

static void e_wind(double t)   /* 130306 → MWV(R), MWV(T), MWD */
{
    (void)t;
    char f[160];
    double ta, ts;
    wind_true_water(&ta, &ts);
    /* apparent : angle relatif à l'étrave (état déduit ou imposé) */
    snprintf(f, sizeof f,
             "\"Reference\":\"Apparent\",\"Wind Speed\":%.2f,\"Wind Angle\":%.1f",
             boat.aws, boat.awa);
    emit(2, MAD_SRC, 130306, "Wind Data", f);
    /* vrai référencé eau : angle relatif à l'étrave aussi */
    snprintf(f, sizeof f,
             "\"Reference\":\"True (water referenced)\",\"Wind Speed\":%.2f,\"Wind Angle\":%.1f",
             ts, ta);
    emit(2, MAD_SRC, 130306, "Wind Data", f);
    /* vrai référencé nord : le champ porte la DIRECTION (→ MWD côté 0183) */
    snprintf(f, sizeof f,
             "\"Reference\":\"True (ground referenced to North)\","
             "\"Wind Speed\":%.2f,\"Wind Angle\":%.1f", boat.tws, boat.twd);
    emit(2, MAD_SRC, 130306, "Wind Data", f);
}

static void e_setdrift(double t)   /* 129291 → VDR (courant) */
{
    (void)t;
    char f[160];
    snprintf(f, sizeof f,
             "\"Set Reference\":\"True\",\"Set\":%.1f,\"Drift\":%.2f",
             boat.set, boat.drift);
    emit(4, MAD_SRC, 129291, "Set & Drift, Rapid Update", f);
}

static void e_rudder(double t)   /* 127245 → RSA */
{
    char f[96];
    snprintf(f, sizeof f, "\"Position\":%.1f", 5.0 * sin(t / 8.0));
    emit(2, MAD_SRC, 127245, "Rudder", f);
}

static void e_stw(double t)   /* 128259 → VHW (vitesse surface) */
{
    (void)t;
    char f[96];
    /* CALCULÉE : module du vecteur fond − courant (cf. boat_update). */
    snprintf(f, sizeof f, "\"Speed Water Referenced\":%.2f", boat.stw);
    emit(2, MAD_SRC, 128259, "Speed", f);
}

static void e_log(double t)    /* 128275 → VLW (distance dans l'eau, DST810) */
{
    char f[96];
    /* loch : cumul (odomètre) + trajet depuis reset, en mètres. Les deux DST810
     * émettent ; l'arbitre garde le MAX (le capteur le plus avancé). Le tribord
     * traîne ~2 NM (simule un capteur qui décroche/sort de l'eau et sous-compte). */
    double trip  = 4.6 * t;                 /* ~9 nœuds moyens depuis le départ */
    snprintf(f, sizeof f, "\"Log\":%.0f,\"Trip Log\":%.0f", 1234567.0 + trip, trip);
    emit(3, DSTBB_SRC, 128275, "Distance Log", f);
    snprintf(f, sizeof f, "\"Log\":%.0f,\"Trip Log\":%.0f", 1230860.0 + trip, trip - 3707.0);
    emit(3, DSTTB_SRC, 128275, "Distance Log", f);
}

static void e_temp(double t)   /* 130316 → MTW (eau, DST) + MDA (air, SCX) */
{
    char f[160];
    /* 130316 (Temperature Extended Range) : champ valeur "Temperature".
       DST et SCX émettent tous deux en 130316 (130312 déprécié). */
    snprintf(f, sizeof f,
             "\"Source\":\"Sea Temperature\",\"Temperature Source\":\"Sea Temperature\","
             "\"Temperature\":%.3f", 18.0 + 0.5 * sin(t / 50.0));
    emit(5, DSTBB_SRC, 130316, "Temperature, Extended Range", f);
    snprintf(f, sizeof f,
             "\"Source\":\"Outside Temperature\",\"Temperature Source\":\"Outside Temperature\","
             "\"Temperature\":%.3f", 22.0 + 1.0 * sin(t / 60.0));
    emit(5, SCX_SRC, 130316, "Temperature, Extended Range", f);
}

static void e_press(double t)   /* 130314 → MDA (pression) */
{
    char f[96];
    snprintf(f, sizeof f, "\"Source\":\"Atmospheric\",\"Pressure\":%.4f",
             1.0132 + 0.0010 * sin(t / 120.0));   /* bar */
    emit(5, SCX_SRC, 130314, "Actual Pressure", f);
}

static void e_depth(double t)   /* 128267 → DPT (minimum DST bâbord/tribord) */
{
    char f[96];
    snprintf(f, sizeof f, "\"Depth\":%.2f,\"Offset\":-0.30", 10.0 + 2.0 * sin(t / 20.0));
    emit(3, DSTBB_SRC, 128267, "Water Depth", f);
    snprintf(f, sizeof f, "\"Depth\":%.2f,\"Offset\":-0.30", 11.0 + 2.0 * sin(t / 20.0 + 1.0));
    emit(3, DSTTB_SRC, 128267, "Water Depth", f);
}

/* --- AIS (dédup par MMSI + encodage VDM par n2kd) ---
 *
 * Champs en forme `analyzer -nv` ({"value":N,"name":"S"}, User ID en
 * {"value":"MMSI","key":true}) et jeu COMPLET : n2kd exige tous les champs du
 * PGN pour reconstruire le train de bits VDM (un champ manquant → message ignoré
 * en silence). Modelé sur des trames réelles em-trak/DataHub. */

static void ais_class_b(int src, unsigned mmsi, double lat, double lon,
                        double cog, double sog)
{
    char f[1024];
    snprintf(f, sizeof f,
        "\"Message ID\":{\"value\":18,\"name\":\"Standard Class B position report\"},"
        "\"Repeat Indicator\":{\"value\":0,\"name\":\"Initial\"},"
        "\"User ID\":{\"value\":\"%u\",\"key\":true},"
        "\"Longitude\":%.6f,\"Latitude\":%.6f,"
        "\"Position Accuracy\":{\"value\":0,\"name\":\"Low\"},"
        "\"RAIM\":{\"value\":0,\"name\":\"not in use\"},"
        "\"Time Stamp\":{\"value\":30},"
        "\"COG\":%.1f,\"SOG\":%.2f,"
        "\"Communication State\":\"FF FF 07\","
        "\"AIS Transceiver information\":{\"value\":1,\"name\":\"Channel B VDL reception\"},"
        "\"Heading\":%.1f,\"Regional Application\":\"FF\","
        "\"Unit type\":{\"value\":0,\"name\":\"SOTDMA\"},"
        "\"Integrated Display\":{\"value\":0,\"name\":\"No\"},"
        "\"DSC\":{\"value\":0,\"name\":\"No\"},"
        "\"Band\":{\"value\":0,\"name\":\"Top 525 kHz of marine band\"},"
        "\"Can handle Msg 22\":{\"value\":0,\"name\":\"No\"},"
        "\"AIS mode\":{\"value\":0,\"name\":\"Autonomous\"},"
        "\"AIS communication state\":{\"value\":0,\"name\":\"SOTDMA\"}",
        mmsi, lon, lat, cog, sog, cog);
    emit(4, src, 129039, "AIS Class B Position Report", f);
}

static void ais_class_a(int src, unsigned mmsi, double lat, double lon,
                        double cog, double sog)
{
    char f[1024];
    snprintf(f, sizeof f,
        "\"Message ID\":{\"value\":1,\"name\":\"Scheduled Class A position report\"},"
        "\"Repeat Indicator\":{\"value\":0,\"name\":\"Initial\"},"
        "\"User ID\":{\"value\":\"%u\",\"key\":true},"
        "\"Longitude\":%.6f,\"Latitude\":%.6f,"
        "\"Position Accuracy\":{\"value\":0,\"name\":\"Low\"},"
        "\"RAIM\":{\"value\":0,\"name\":\"not in use\"},"
        "\"Time Stamp\":{\"value\":5},"
        "\"COG\":%.1f,\"SOG\":%.2f,"
        "\"Communication State\":\"FF FF 07\","
        "\"AIS Transceiver information\":{\"value\":0,\"name\":\"Channel A VDL reception\"},"
        "\"Heading\":%.1f,\"Rate of Turn\":0.000,"
        "\"Nav Status\":{\"value\":0,\"name\":\"Under way using engine\"},"
        "\"Special Maneuver Indicator\":{\"value\":0,\"name\":\"Not available\"},"
        "\"Spare\":\"04\"",
        mmsi, lon, lat, cog, sog, cog);
    emit(4, src, 129038, "AIS Class A Position Report", f);
}

/* 129809 : Class B static msg 24 partie A (nom). */
static void ais_static_a(int src, unsigned mmsi, const char *name)
{
    char f[256];
    snprintf(f, sizeof f,
        "\"Message ID\":{\"value\":24,\"name\":\"Static data report\"},"
        "\"Repeat Indicator\":{\"value\":0,\"name\":\"Initial\"},"
        "\"User ID\":{\"value\":\"%u\",\"key\":true},\"Name\":\"%s\","
        "\"AIS Transceiver information\":{\"value\":1,\"name\":\"Channel B VDL reception\"}",
        mmsi, name);
    emit(6, src, 129809, "AIS Class B static data (msg 24 Part A)", f);
}

/* 129810 : Class B static msg 24 partie B (type/dimensions). */
static void ais_static_b(int src, unsigned mmsi)
{
    char f[640];
    snprintf(f, sizeof f,
        "\"Message ID\":{\"value\":24,\"name\":\"Static data report\"},"
        "\"Repeat Indicator\":{\"value\":0,\"name\":\"Initial\"},"
        "\"User ID\":{\"value\":\"%u\",\"key\":true},"
        "\"Type of ship\":{\"value\":36,\"name\":\"Sailing\"},"
        "\"Vendor ID\":\"SIM\",\"Callsign\":\"SIM1\","
        "\"Length\":12.0,\"Beam\":4.0,"
        "\"Position reference from Starboard\":0.0,"
        "\"Position reference from Bow\":0.0,"
        "\"Mothership User ID\":{\"value\":\"000000000\",\"key\":true},"
        "\"GNSS type\":{\"value\":0,\"name\":\"Default: undefined\"},"
        "\"AIS Transceiver information\":{\"value\":1,\"name\":\"Channel B VDL reception\"}",
        mmsi);
    emit(6, src, 129810, "AIS Class B static data (msg 24 Part B)", f);
}

static void e_ais(double t)
{
    double dl = 0.0010 * sin(t / 30.0);
    /* 227000002 vue par les DEUX sources → dédup (em-trak gagne) */
    ais_class_b(AIS_SRC, 227000002, 47.512 + dl, -3.020, 120.0, 3.0);
    ais_class_b(DH_SRC,  227000002, 47.512 + dl, -3.020, 120.0, 3.0);
    /* 227000001 em-trak seule ; 227000003 DataHub seule (union, classe A) */
    ais_class_b(AIS_SRC, 227000001, 47.505 + dl, -3.010,  90.0, 4.0);
    ais_class_a(DH_SRC,  227000003, 47.490 + dl, -2.995, 200.0, 6.0);
    /* statiques (noms/types) pour la cible 227000002 */
    ais_static_a(AIS_SRC, 227000002, "SIM CLASSB");
    ais_static_b(AIS_SRC, 227000002);
}

/* ===== Mode --actisense : trames N2K binaires single-frame (pour ydraw-bridge) =====
 *
 * Encode quelques PGN single-frame au format texte actisense
 *   <timestamp>,prio,pgn,src,dst,len,b0,b1,...
 * (octets en hex), à piper dans ./ydraw-bridge → YDRAW/TCP → qtVlm en N2K.
 * ATTENTION : le binaire N2K est en unités SI (radians, m/s, kelvin, pascals),
 * indépendamment de l'affichage « lisible » de l'analyzer. Les facteurs sont
 * validés par aller-retour dans `analyzer`. */

#define DEG2RAD(d) ((d) * M_PI / 180.0)

static void p16(uint8_t *b, int off, int v)
{
    b[off] = (uint8_t)(v & 0xff);
    b[off + 1] = (uint8_t)((v >> 8) & 0xff);
}
static void p24(uint8_t *b, int off, long v)
{
    b[off]     = (uint8_t)(v & 0xff);
    b[off + 1] = (uint8_t)((v >> 8) & 0xff);
    b[off + 2] = (uint8_t)((v >> 16) & 0xff);
}
static void p32(uint8_t *b, int off, long v)
{
    b[off]     = (uint8_t)(v & 0xff);
    b[off + 1] = (uint8_t)((v >> 8) & 0xff);
    b[off + 2] = (uint8_t)((v >> 16) & 0xff);
    b[off + 3] = (uint8_t)((v >> 24) & 0xff);
}
static void p64(uint8_t *b, int off, long long v)
{
    for (int i = 0; i < 8; i++) { b[off + i] = (uint8_t)(v & 0xff); v >>= 8; }
}

static void emit_frame(int prio, int src, int pgn, const uint8_t *d, int len)
{
    char ts[40]; ts_now(ts, sizeof ts);
    printf("%s,%d,%d,%d,255,%d", ts, prio, pgn, src, len);
    for (int i = 0; i < len; i++)
        printf(",%02x", d[i]);
    printf("\n");
}

static void a_pos(void)            /* 129025 Position Rapid Update */
{
    uint8_t b[8];
    p32(b, 0, lround(boat.lat / 1e-7));
    p32(b, 4, lround(boat.lon / 1e-7));
    emit_frame(2, SCX_SRC, 129025, b, 8);
}

static void a_cogsog(void)         /* 129026 COG & SOG Rapid Update */
{
    uint8_t b[8] = { 0xff, 0xfc, 0, 0, 0, 0, 0xff, 0xff };  /* SID ; ref=True */
    p16(b, 2, (int)lround(DEG2RAD(boat.cog) / 1e-4));
    p16(b, 4, (int)lround(boat.sog / 0.01));
    emit_frame(2, SCX_SRC, 129026, b, 8);
}

static void a_heading(void)        /* 127250 Vessel Heading (True) */
{
    uint8_t b[8] = { 0xff, 0, 0, 0, 0, 0, 0, 0xfc };       /* ref=True (byte7) */
    p16(b, 1, (int)lround(DEG2RAD(boat.hdg) / 1e-4));
    p16(b, 3, 0);                                           /* déviation 0 */
    p16(b, 5, (int)lround(DEG2RAD(-2.0) / 1e-4));           /* variation 2°W */
    emit_frame(2, SCX_SRC, 127250, b, 8);
}

static void a_rot(void)            /* 127251 Rate of Turn */
{
    uint8_t b[8] = { 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff };
    p32(b, 1, lround(DEG2RAD(boat.rot) / 3.125e-08));       /* res 3.125e-8 rad/s */
    emit_frame(2, SCX_SRC, 127251, b, 8);
}

static void a_attitude(double t)   /* 127257 Attitude (yaw/pitch/roll) */
{
    uint8_t b[7] = { 0xff, 0, 0, 0, 0, 0, 0 };
    p16(b, 1, 0);                                           /* yaw */
    p16(b, 3, (int)lround(DEG2RAD(3.0 * sin(t / 5.0)) / 1e-4));
    p16(b, 5, (int)lround(DEG2RAD(8.0 * sin(t / 7.0)) / 1e-4));
    emit_frame(2, SCX_SRC, 127257, b, 7);
}

static void a_wind(double t)       /* 130306 Wind Data (apparent) */
{
    uint8_t b[8] = { 0xff, 0, 0, 0, 0, 0xfa, 0xff, 0xff };  /* ref=Apparent(2) */
    double as = 8.0 + 2.0 * sin(t / 8.0);
    double aa = 45.0 + 20.0 * sin(t / 10.0);
    p16(b, 1, (int)lround(as / 0.01));
    p16(b, 3, (int)lround(DEG2RAD(aa) / 1e-4));
    emit_frame(2, MAD_SRC, 130306, b, 8);
}

static void a_stw(double t)        /* 128259 Speed (water referenced) */
{
    uint8_t b[8] = { 0xff, 0, 0, 0xff, 0xff, 0, 0xff, 0xff };
    p16(b, 1, (int)lround((boat.sog - 0.3 + 0.2 * sin(t / 12.0)) / 0.01));
    emit_frame(2, MAD_SRC, 128259, b, 8);
}

static void a_log(double t)        /* 128275 Distance Log (fast-packet, 14 o.) */
{
    uint8_t b[14];
    memset(b, 0, sizeof b);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    p16(b, 0, (int)(ts.tv_sec / 86400));                 /* Date (jours) */
    p32(b, 2, (ts.tv_sec % 86400) * 10000L);             /* Time (×0.0001 s) */
    double trip  = 4.6 * t;
    p32(b, 6,  lround(1234567.0 + trip));                /* Log (m) */
    p32(b, 10, lround(trip));                            /* Trip Log (m) */
    emit_frame(3, DSTBB_SRC, 128275, b, 14);
}

static void a_depth(double t)      /* 128267 Water Depth */
{
    uint8_t b[8] = { 0xff, 0, 0, 0, 0, 0, 0, 0xff };
    p32(b, 1, lround((10.0 + 2.0 * sin(t / 20.0)) / 0.01));
    p16(b, 5, (int)lround(-0.30 / 0.001));                  /* offset −0.30 m */
    emit_frame(3, DSTBB_SRC, 128267, b, 8);
}

static void a_temp(double t)       /* 130316 Temperature Extended Range (Sea) */
{
    /* SID, Instance, Source(Sea=0), Temperature(24b, 0.001K), Set Temp(16b) */
    uint8_t b[8] = { 0xff, 0, 0, 0, 0, 0, 0xff, 0xff };
    double k = (18.0 + 0.5 * sin(t / 50.0)) + 273.15;
    p24(b, 3, lround(k / 0.001));
    emit_frame(5, DSTBB_SRC, 130316, b, 8);
}

static void a_press(double t)      /* 130314 Actual Pressure (atmosphérique) */
{
    uint8_t b[8] = { 0xff, 0, 0, 0, 0, 0, 0, 0xff };        /* instance 0, source 0 */
    double pa = (1.0132 + 0.0010 * sin(t / 120.0)) * 1e5;   /* bar → Pa */
    p32(b, 3, lround(pa / 0.1));                            /* res 0.1 Pa */
    emit_frame(5, SCX_SRC, 130314, b, 8);
}

static void a_rudder(double t)     /* 127245 Rudder */
{
    uint8_t b[8] = { 0, 0xff, 0xff, 0x7f, 0, 0, 0xff, 0xff };  /* instance 0 ; ordre n/a */
    p16(b, 4, (int)lround(DEG2RAD(5.0 * sin(t / 8.0)) / 1e-4));
    emit_frame(2, MAD_SRC, 127245, b, 8);
}

static void a_envparams(double t)  /* 130311 Environmental Parameters (baromètre) */
{
    uint8_t b[8] = { 0xff, 0x01, 0, 0, 0, 0, 0, 0 };   /* Temp Source = Outside(1) */
    double k = (22.0 + 1.0 * sin(t / 60.0)) + 273.15;  /* temp air, K */
    p16(b, 2, (int)lround(k / 0.01));
    p16(b, 4, (int)lround(60.0 / 0.004));              /* humidité 60 % */
    double pa = (1.0132 + 0.0010 * sin(t / 120.0)) * 1e5;
    p16(b, 6, (int)lround(pa / 100.0));                /* pression, res 100 Pa (hPa) */
    emit_frame(5, SCX_SRC, 130311, b, 8);
}

static void a_systime(void)        /* 126992 System Time */
{
    uint8_t b[8] = { 0xff, 0xf0, 0, 0, 0, 0, 0, 0 };        /* source GPS */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    long days = ts.tv_sec / 86400;
    long secday = ts.tv_sec % 86400;
    p16(b, 2, (int)days);
    p32(b, 4, secday * 10000L + ts.tv_nsec / 100000);       /* res 1e-4 s */
    emit_frame(3, SCX_SRC, 126992, b, 8);
}

/* --- Fast-packet : encodés en un message complet (>8 octets), ydraw-bridge
 *     les re-fragmente en trames CAN fast-packet. --- */

static void a_gnss(void)           /* 129029 GNSS Position Data (fast-packet) */
{
    uint8_t b[43];
    memset(b, 0, sizeof b);
    b[0] = 0xff;                                        /* SID */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    long days = ts.tv_sec / 86400, secday = ts.tv_sec % 86400;
    p16(b, 1, (int)days);
    p32(b, 3, secday * 10000L + ts.tv_nsec / 100000);
    p64(b, 7,  llround(boat.lat / 1e-16));              /* Latitude  (1e-16 deg) */
    p64(b, 15, llround(boat.lon / 1e-16));             /* Longitude (1e-16 deg) */
    p64(b, 23, llround(12.3 / 1e-6));                  /* Altitude  (1e-6 m) */
    b[31] = (1 << 4) | 0;                              /* Method=GNSS fix ; type=GPS */
    b[32] = 0xfc;                                      /* Integrity 0 + réservé */
    b[33] = 9;                                         /* Number of SVs */
    p16(b, 34, (int)lround(0.8 / 0.01));              /* HDOP */
    p16(b, 36, (int)lround(1.5 / 0.01));              /* PDOP */
    p32(b, 38, lround(47.0 / 0.01));                  /* Geoidal Separation */
    b[42] = 0;                                         /* Reference Stations */
    emit_frame(3, SCX_SRC, 129029, b, 43);
}

static void a_dops(void)           /* 129539 GNSS DOPs (mode de fix + DOP) */
{
    uint8_t b[8];
    memset(b, 0, sizeof b);
    b[0] = 0xff;
    b[1] = (uint8_t)(3 | (2 << 3) | (3 << 6));        /* Desired=Auto(3), Actual=3D(2) */
    p16(b, 2, (int)lround(0.8 / 0.01));               /* HDOP */
    p16(b, 4, (int)lround(1.2 / 0.01));               /* VDOP */
    p16(b, 6, 0x7fff);                                 /* TDOP n/a */
    emit_frame(6, SCX_SRC, 129539, b, 8);
}

static void a_gsv(double t)        /* 129540 GNSS Sats in View (fast-packet) */
{
    int nsat = 8;
    uint8_t b[3 + 12 * 8];
    memset(b, 0, sizeof b);
    b[0] = 0xff;                                       /* SID */
    b[1] = 0xfc;                                       /* Range residual mode + réservé */
    b[2] = (uint8_t)nsat;                              /* Sats in View */
    int off = 3;
    for (int i = 0; i < nsat; i++) {
        double el = 20 + 60.0 * fabs(sin((t + i * 7) / 40.0));
        double az = fmod(i * 45 + t * 2, 360.0);
        double sn = 30 + 12.0 * fabs(sin((t + i * 3) / 20.0));
        b[off]      = (uint8_t)(i + 1);                /* PRN */
        p16(b, off + 1, (int)lround(DEG2RAD(el) / 1e-4));  /* Elevation */
        p16(b, off + 3, (int)lround(DEG2RAD(az) / 1e-4));  /* Azimuth */
        p16(b, off + 5, (int)lround(sn / 0.01));           /* SNR */
        p32(b, off + 7, 0x7fffffff);                       /* Range residuals n/a */
        b[off + 11] = 0xf2;                                /* Status=Used + réservé */
        off += 12;
    }
    emit_frame(6, SCX_SRC, 129540, b, off);
}

/* --- AIS binaire (bit-packé, ordre N2K : LSB d'abord) ---
 * Les PGN AIS ne sont PAS alignés octet → packer au bit près. Le buffer doit
 * être pré-mis à 0. Tous fast-packet : emit_frame émet le message complet, le
 * ydraw-bridge re-fragmente. Layouts/échelles validés par aller-retour analyzer.
 * Les deux sources (em-trak + DataHub) émettent la même cible 227000002 : en N2K
 * direct, c'est qtVlm qui dédoublonne par MMSI (pas d'arbitrage n2k-mux ici). */
static void putbits(uint8_t *b, int *pos, uint64_t val, int bits)
{
    for (int i = 0; i < bits; i++)
        if ((val >> i) & 1ULL) {
            int p = *pos + i;
            b[p >> 3] |= (uint8_t)(1u << (p & 7));
        }
    *pos += bits;
}

/* STRING_FIX (champ byte-aligné) : ASCII puis bourrage 0xff (terminateur canboat). */
static void putstr_fix(uint8_t *b, int *pos, const char *s, int nbytes)
{
    int byte = *pos >> 3, i = 0;
    for (; s && s[i] && i < nbytes; i++) b[byte + i] = (uint8_t)s[i];
    for (; i < nbytes; i++) b[byte + i] = 0xff;
    *pos += nbytes * 8;
}

static uint64_t b32(double deg) { return (uint64_t)(int64_t)lround(deg / 1e-7); }

static void a_ais_class_b(int src, unsigned mmsi, double lat, double lon,
                          double cog, double sog, double hdg)
{
    uint8_t b[27]; memset(b, 0, sizeof b); int p = 0;
    putbits(b,&p, 18, 6);                                   /* Message ID */
    putbits(b,&p, 0, 2);                                    /* Repeat Indicator */
    putbits(b,&p, mmsi, 32);                                /* User ID (MMSI) */
    putbits(b,&p, b32(lon), 32);                            /* Longitude */
    putbits(b,&p, b32(lat), 32);                            /* Latitude */
    putbits(b,&p, 0, 1);                                    /* Position Accuracy */
    putbits(b,&p, 0, 1);                                    /* RAIM */
    putbits(b,&p, 30, 6);                                   /* Time Stamp */
    putbits(b,&p, (uint64_t)lround(DEG2RAD(cog)/1e-4), 16); /* COG */
    putbits(b,&p, (uint64_t)lround(sog/0.01), 16);          /* SOG */
    putbits(b,&p, 0x7ffff, 19);                             /* Communication State */
    putbits(b,&p, 1, 5);                                    /* Transceiver (Channel B) */
    putbits(b,&p, (uint64_t)lround(DEG2RAD(hdg)/1e-4), 16); /* Heading */
    putbits(b,&p, 0xff, 8);                                 /* Regional Application */
    putbits(b,&p, 0x3, 2);                                  /* Regional Application B */
    putbits(b,&p, 0, 1);                                    /* Unit type */
    putbits(b,&p, 0, 1);                                    /* Integrated Display */
    putbits(b,&p, 0, 1);                                    /* DSC */
    putbits(b,&p, 0, 1);                                    /* Band */
    putbits(b,&p, 0, 1);                                    /* Can handle Msg 22 */
    putbits(b,&p, 0, 1);                                    /* AIS mode */
    putbits(b,&p, 0, 1);                                    /* AIS communication state */
    putbits(b,&p, 0x7fff, 15);                              /* Reserved */
    emit_frame(4, src, 129039, b, 27);
}

static void a_ais_class_a(int src, unsigned mmsi, double lat, double lon,
                          double cog, double sog, double hdg)
{
    uint8_t b[28]; memset(b, 0, sizeof b); int p = 0;
    putbits(b,&p, 1, 6);                                    /* Message ID (sched A) */
    putbits(b,&p, 0, 2);
    putbits(b,&p, mmsi, 32);
    putbits(b,&p, b32(lon), 32);
    putbits(b,&p, b32(lat), 32);
    putbits(b,&p, 0, 1);                                    /* Position Accuracy */
    putbits(b,&p, 0, 1);                                    /* RAIM */
    putbits(b,&p, 5, 6);                                    /* Time Stamp */
    putbits(b,&p, (uint64_t)lround(DEG2RAD(cog)/1e-4), 16); /* COG */
    putbits(b,&p, (uint64_t)lround(sog/0.01), 16);          /* SOG */
    putbits(b,&p, 0x7ffff, 19);                             /* Communication State */
    putbits(b,&p, 0, 5);                                    /* Transceiver (Channel A) */
    putbits(b,&p, (uint64_t)lround(DEG2RAD(hdg)/1e-4), 16); /* Heading */
    putbits(b,&p, 0, 16);                                   /* Rate of Turn = 0 */
    putbits(b,&p, 0, 4);                                    /* Nav Status (under way) */
    putbits(b,&p, 0, 2);                                    /* Special Maneuver */
    putbits(b,&p, 0x3, 2);                                  /* Reserved */
    putbits(b,&p, 0x7, 3);                                  /* Spare */
    putbits(b,&p, 0x1f, 5);                                 /* Reserved */
    putbits(b,&p, 0xff, 8);                                 /* Sequence ID */
    emit_frame(4, src, 129038, b, 28);
}

static void a_ais_static_a(int src, unsigned mmsi, const char *name)
{
    uint8_t b[27]; memset(b, 0, sizeof b); int p = 0;
    putbits(b,&p, 24, 6);                                   /* Message ID (static) */
    putbits(b,&p, 0, 2);
    putbits(b,&p, mmsi, 32);
    putstr_fix(b,&p, name, 20);                             /* Name (160 bits) */
    putbits(b,&p, 1, 5);                                    /* Transceiver (Channel B) */
    putbits(b,&p, 0x7, 3);                                  /* Reserved */
    putbits(b,&p, 0xff, 8);                                 /* Sequence ID */
    emit_frame(6, src, 129809, b, 27);
}

static void a_ais_static_b(int src, unsigned mmsi)
{
    uint8_t b[35]; memset(b, 0, sizeof b); int p = 0;
    putbits(b,&p, 24, 6);
    putbits(b,&p, 0, 2);
    putbits(b,&p, mmsi, 32);
    putbits(b,&p, 36, 8);                                   /* Type of ship = Sailing */
    putstr_fix(b,&p, "SIM", 7);                             /* Vendor ID (56 bits) */
    putstr_fix(b,&p, "SIM1", 7);                            /* Callsign (56 bits) */
    putbits(b,&p, (uint64_t)lround(12.0/0.1), 16);          /* Length */
    putbits(b,&p, (uint64_t)lround(4.0/0.1), 16);           /* Beam */
    putbits(b,&p, 0, 16);                                   /* Pos ref Starboard */
    putbits(b,&p, 0, 16);                                   /* Pos ref Bow */
    putbits(b,&p, 0, 32);                                   /* Mothership User ID */
    putbits(b,&p, 0x3, 2);                                  /* Reserved */
    putbits(b,&p, 0x3, 2);                                  /* Spare */
    putbits(b,&p, 0, 4);                                    /* GNSS type */
    putbits(b,&p, 1, 5);                                    /* Transceiver (Channel B) */
    putbits(b,&p, 0x7, 3);                                  /* Reserved */
    putbits(b,&p, 0xff, 8);                                 /* Sequence ID */
    emit_frame(6, src, 129810, b, 35);
}

static void a_ais(double t)
{
    double dl = 0.0010 * sin(t / 30.0);
    a_ais_class_b(AIS_SRC, 227000002, 47.512 + dl, -3.020, 120.0, 3.0, 120.0);
    a_ais_class_b(DH_SRC,  227000002, 47.512 + dl, -3.020, 120.0, 3.0, 120.0);
    a_ais_class_b(AIS_SRC, 227000001, 47.505 + dl, -3.010,  90.0, 4.0,  90.0);
    a_ais_class_a(DH_SRC,  227000003, 47.490 + dl, -2.995, 200.0, 6.0, 200.0);
    a_ais_static_a(AIS_SRC, 227000002, "SIM CLASSB");
    a_ais_static_b(AIS_SRC, 227000002);
}

static int run_actisense(double duration, long tick, int once, int no_ais)
{
    /* (fn0 sans arg) et (fn1 avec t) regroupées via un switch indexé. */
    struct { int id; double iv, next; } S[] = {
        {0,250,0},{1,1000,0},{2,200,0},{3,500,0},{4,200,0},
        {5,250,0},{6,500,0},{7,500,0},{8,2000,0},{9,2000,0},{10,1000,0},
        {11,2000,0},{12,200,0},{13,1000,0},{14,5000,0},{15,2000,0},
        {16,2000,0},{17,3000,0},
    };
    int n = (int)(sizeof S / sizeof *S);
    uint64_t start = now_ms();
    double last_state_ms = -1e9;
    do {
        uint64_t now = now_ms();
        double el = (double)(now - start);
        if (!once && duration > 0 && el >= duration * 1000.0) break;
        double t = el / 1000.0;
        ctl_refresh();                 /* --control : mêmes réglages qu'en JSON */
        boat_update(t);
        if (el - last_state_ms >= 500.0) { state_write(); last_state_ms = el; }
        if (!g_ctl.enabled) {          /* désactivé : aucune trame émise */
            usleep((useconds_t)(tick * 1000));
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (!once && el < S[i].next) continue;
            switch (S[i].id) {
                case 0: a_pos(); break;
                case 1: a_cogsog(); break;
                case 2: a_heading(); break;
                case 3: a_rot(); break;
                case 4: a_attitude(t); break;
                case 5: a_wind(t); break;
                case 6: a_stw(t); break;
                case 7: a_depth(t); break;
                case 8: a_temp(t); break;
                case 9: a_press(t); break;
                case 10: a_systime(); break;
                case 11: a_envparams(t); break;
                case 12: a_rudder(t); break;
                case 13: a_gnss(); break;
                case 14: a_gsv(t); break;
                case 15: a_dops(); break;
                case 16: a_log(t); break;
                case 17: if (!no_ais) a_ais(t); break;
            }
            S[i].next = el + S[i].iv;
        }
        fflush(stdout);
        if (once) return 0;
        usleep((useconds_t)(tick * 1000));
    } while (!g_stop);
    return 0;
}

/* --- Ordonnanceur (mode JSON) --- */
typedef void (*emit_fn)(double);
typedef struct { emit_fn fn; double iv_ms; double next_ms; int is_ais; const char *name; } sched_t;

static sched_t SCHED[] = {
    { e_identity,    10000, 0, 0, "identité (60928/126996)" },
    { e_pos,           250, 0, 0, "129025 → GLL" },
    { e_cogsog,       1000, 0, 0, "129026 → VTG" },
    { e_systime,      1000, 0, 0, "126992 → ZDA" },
    { e_gnss,         1000, 0, 0, "129029 → GGA" },
    { e_dops,         2000, 0, 0, "129539 → GSA" },
    { e_gsv,          5000, 0, 0, "129540 → GSV" },
    { e_heading,       200, 0, 0, "127250 → HDG/HDM/HDT" },
    { e_rot,           500, 0, 0, "127251 → ROT" },
    { e_attitude,      200, 0, 0, "127257 → XDR" },
    { e_wind,          250, 0, 0, "130306 → MWV/MWD" },
    { e_setdrift,     1000, 0, 0, "129291 → VDR" },
    { e_rudder,        200, 0, 0, "127245 → RSA" },
    { e_stw,           500, 0, 0, "128259 → VHW" },
    { e_log,          2000, 0, 0, "128275 → VLW" },
    { e_temp,         2000, 0, 0, "130316 → MTW/MDA" },
    { e_press,        2000, 0, 0, "130314 → MDA" },
    { e_depth,         500, 0, 0, "128267 → DPT" },
    { e_ais,          3000, 0, 1, "AIS 129039/794/809" },
};
static const int N_SCHED = (int)(sizeof SCHED / sizeof *SCHED);

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage : %s [--once] [--duration SEC] [--no-ais] [--tick MS]\n"
        "  --once          émet un exemplaire de chaque PGN puis s'arrête\n"
        "  --duration SEC  s'arrête après SEC secondes (0 = sans fin, défaut)\n"
        "  --no-ais        n'émet pas les PGN AIS\n"
        "  --actisense     émet des TRAMES N2K binaires (format actisense, single-frame\n"
        "                  + fast-packet GNSS/loch/AIS) à piper dans ./ydraw-bridge\n"
        "                  → YDRAW/TCP → qtVlm en N2K\n"
        "  --tick MS       période de la boucle d'émission (défaut 100 ms)\n"
        "  --control FIC   pilotage à chaud : enabled, hdg, stw, set, drift, et le\n"
        "                  vent par UNE paire (awa+aws, twa+tws ou twd+tws). Relu à\n"
        "                  chaque changement du fichier ; c'est ce que l'interface\n"
        "                  web écrit. Vitesses en nœuds, angles en degrés.\n"
        "  --state FIC     publie l'état DÉDUIT (cog, sog, twa, awa, aws…) dans ce\n"
        "                  fichier, même format, pour affichage par l'interface web\n"
        "\nÉmet du JSON façon `analyzer -json` pour tous les PGN compris par\n"
        "n2k-mux + l'identité. Exemple : %s | ./n2k-mux n2k-sim.ini -v\n",
        p, p);
}

int main(int argc, char **argv)
{
    int once = 0, no_ais = 0, actisense = 0;
    double duration = 0;       /* secondes ; 0 = sans fin */
    long tick_ms = 100;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--once") == 0)      once = 1;
        else if (strcmp(argv[i], "--no-ais") == 0)    no_ais = 1;
        else if (strcmp(argv[i], "--actisense") == 0) actisense = 1;
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) duration = atof(argv[++i]);
        else if (strcmp(argv[i], "--tick") == 0 && i + 1 < argc)     tick_ms = atol(argv[++i]);
        else if (strcmp(argv[i], "--control") == 0 && i + 1 < argc)  g_ctl_path = argv[++i];
        else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc)    g_state_path = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "option inconnue : %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (tick_ms < 10) tick_ms = 10;

    signal(SIGINT, on_int);
    signal(SIGTERM, on_int);
    setvbuf(stdout, NULL, _IOLBF, 0);   /* sortie ligne par ligne (pipe) */

    /* Mode --actisense : trames N2K binaires (→ ydraw-bridge → qtVlm en N2K).
     * Pas d'en-tête JSON ici (format texte actisense). AIS inclus (fast-packet). */
    if (actisense)
        return run_actisense(duration, tick_ms, once, no_ais);

    /* En-tête analyzer (exigé par n2kd ; le parser le marque is_header). */
    printf("{\"version\":\"n2k-sim 1.0\",\"showLookupValues\":true}\n");

    if (once) {
        /* Le pilotage vaut AUSSI pour --once : sans cette relecture, le mode
         * « un de chaque PGN » sortait toujours les valeurs automatiques. */
        ctl_refresh();
        boat_update(0.0);
        state_write();
        if (!g_ctl.enabled) {
            fflush(stdout);
            return 0;              /* désactivé : rien que l'en-tête */
        }
        /* identité d'abord (sinon les 1ers instruments sont rejetés), puis un
         * exemplaire de chaque PGN. */
        for (int i = 0; i < N_SCHED; i++) {
            if (SCHED[i].is_ais && no_ais) continue;
            SCHED[i].fn(0.0);
        }
        fflush(stdout);
        return 0;
    }

    uint64_t start = now_ms();
    int was_enabled = 1;
    double last_state_ms = -1e9;
    while (!g_stop) {
        uint64_t now = now_ms();
        double el = (double)(now - start);          /* ms écoulées */
        if (duration > 0 && el >= duration * 1000.0) break;
        double t = el / 1000.0;                      /* secondes (phase) */

        ctl_refresh();                               /* --control : relecture */
        boat_update(t);                              /* avance la position/cap */

        /* état déduit publié ~2 fois par seconde (affichage web) */
        if (el - last_state_ms >= 500.0) { state_write(); last_state_ms = el; }

        /* Porte « enabled » : désactivé, le simulateur n'émet RIEN. La chaîne
         * reste debout (kplex, n2kd, web) et le daemon publie un âge de dernier
         * message qui grandit — l'interface affiche FLUX MORT, ce qui est la
         * bonne lecture. À la réactivation on redonne l'en-tête analyzer, dont
         * n2kd a besoin s'il a démarré entre-temps. */
        if (!g_ctl.enabled) {
            was_enabled = 0;
            usleep((useconds_t)(tick_ms * 1000));
            continue;
        }
        if (!was_enabled) {
            printf("{\"version\":\"n2k-sim 1.0\",\"showLookupValues\":true}\n");
            was_enabled = 1;
        }

        for (int i = 0; i < N_SCHED; i++) {
            if (SCHED[i].is_ais && no_ais) continue;
            if (el >= SCHED[i].next_ms) {
                SCHED[i].fn(t);
                SCHED[i].next_ms = el + SCHED[i].iv_ms;
            }
        }
        fflush(stdout);
        usleep((useconds_t)(tick_ms * 1000));
    }
    return 0;
}
