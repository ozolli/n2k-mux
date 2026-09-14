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
 *     twd     = 225      ; direction du vent VRAI (D'OÙ il vient), degrés
 *     tws     = 15       ; vitesse du vent VRAI, NŒUDS
 *
 * Toute clé absente ou à « auto » garde le comportement automatique.
 *
 * SIX entrées, et tout le reste en DÉCOULE :
 *   - route et vitesse fond (COG, SOG) = vecteur surface + vecteur courant ;
 *   - angle du vent vrai à l'étrave (TWA) = TWD − HDG ;
 *   - vent apparent (AWA, AWS) = vent vrai − vecteur bateau sur le fond ;
 *   - vent vrai référencé eau = vent vrai − courant, ramené à l'étrave ;
 *   - taux de giration = dérivée du CAP (nul si le cap est imposé) ;
 *   - la position s'intègre le long du COG ainsi obtenu.
 *
 * L'état déduit est publié en clair avec --state (même format), ce que
 * l'interface web affiche à côté des réglages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

#include "polar.h"

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
    double twa_w;      /* vent vrai RÉFÉRENCÉ EAU : angle / étrave (deg) */
    double tws_w;      /* vent vrai référencé eau : vitesse (m/s) */
    double tws_base;   /* vent vrai de base, avant l'aléa (m/s) */
    double twd_base;   /* direction de base, avant l'aléa (deg) */
    int    stw_from_polar;  /* 1 si la vitesse surface vient de la polaire */
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
    double twd, tws;                    /* vent VRAI (TWA/AWA/AWS en découlent) */
    /* vent aléatoire (cf. wseq_t) */
    int    wind_random;                 /* 1 = aléa appliqué autour de twd/tws */
    double tws_var;                     /* amplitude TOTALE de la force, en % */
    double twd_var;                     /* amplitude TOTALE de la direction, en ° */
    double wind_period;                 /* durée typique d'une séquence, minutes */
    unsigned long seed;                 /* 0 = graine tirée de l'horloge */
    /* polaire */
    int    stw_polar;                   /* 1 = stw calculée par la polaire */
    char   polar[512];                  /* chemin complet du .pol/.csv */
    /* Centres des entrées en « auto » : « twd = auto 300 » fait varier le vent
     * AUTOUR de 300°. C'est la dernière valeur réglée, que l'interface écrit à
     * côté du mot auto pour qu'elle survive à un redémarrage. NAN = aucun. */
    double c_hdg, c_stw, c_set, c_drift, c_twd, c_tws;
} simctl_t;

/* Valeurs par défaut de l'aléa : 10 % de force (plage 0-20 %), 20° de direction, séquences
 * d'environ 10 minutes, comme on l'observe sur l'eau. */
#define CTL_INIT { 1, NAN, NAN, NAN, NAN, NAN, NAN, 0, 10.0, 20.0, 10.0, 0, 0, "", \
                   NAN, NAN, NAN, NAN, NAN, NAN }

/* Base du vent quand l'aléa est actif et twd/tws laissés à « auto ». */
#define WIND_BASE_TWD   225.0
#define WIND_BASE_TWS_KN 15.0

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
    char line[640];
    while (fgets(line, sizeof line, f)) {
        for (char *p = line; *p; p++)
            if (*p == ';' || *p == '#') { *p = '\0'; break; }
        /* clé = valeur, la valeur allant jusqu'au bout de la ligne : un chemin
         * de polaire peut contenir des espaces (« Oceanis 46.csv »). */
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char key[32] = "", val[600] = "";
        if (sscanf(line, " %31[A-Za-z_]", key) != 1)
            continue;
        {
            char *b = eq + 1, *e = b + strlen(b);
            while (*b == ' ' || *b == '\t') b++;
            while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
                e--;
            snprintf(val, sizeof val, "%.*s", (int)(e - b), b);
        }
        if (!val[0])
            continue;
        for (char *p = key; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p += 32;
        /* « auto » ou « auto N » : valeur automatique, centrée sur N si donné */
        double v, vc = NAN;
        if (strncasecmp(val, "auto", 4) == 0) {
            v = NAN;
            char *end = NULL;
            double cv = strtod(val + 4, &end);
            if (end && end != val + 4)
                vc = cv;
        } else {
            v = atof(val);
        }
        if (strcmp(key, "hdg") == 0)   c.c_hdg   = vc;
        if (strcmp(key, "stw") == 0)   c.c_stw   = isnan(vc) ? vc : vc * KN_TO_MS;
        if (strcmp(key, "set") == 0)   c.c_set   = vc;
        if (strcmp(key, "drift") == 0) c.c_drift = isnan(vc) ? vc : vc * KN_TO_MS;
        if (strcmp(key, "twd") == 0)   c.c_twd   = vc;
        if (strcmp(key, "tws") == 0)   c.c_tws   = isnan(vc) ? vc : vc * KN_TO_MS;
        if      (strcmp(key, "enabled") == 0)
            c.enabled = (strcmp(val, "0") && strcmp(val, "false") &&
                         strcmp(val, "off") && strcmp(val, "no"));
        else if (strcmp(key, "hdg") == 0)   c.hdg = v;
        else if (strcmp(key, "stw") == 0)   c.stw = isnan(v) ? v : v * KN_TO_MS;
        else if (strcmp(key, "set") == 0)   c.set = v;
        else if (strcmp(key, "drift") == 0) c.drift = isnan(v) ? v : v * KN_TO_MS;
        else if (strcmp(key, "twd") == 0)   c.twd = v;
        else if (strcmp(key, "tws") == 0)   c.tws = isnan(v) ? v : v * KN_TO_MS;
        else if (strcmp(key, "wind_random") == 0) c.wind_random = !isnan(v) && v != 0;
        else if (strcmp(key, "tws_var") == 0 && !isnan(v))     /* 0 à 20 % */
            c.tws_var = v < 0 ? 0 : (v > 20 ? 20 : v);
        else if (strcmp(key, "twd_var") == 0 && !isnan(v))     c.twd_var = v < 0 ? 0 : v;
        else if (strcmp(key, "wind_period") == 0 && !isnan(v)) c.wind_period = v < 0.5 ? 0.5 : v;
        else if (strcmp(key, "seed") == 0 && !isnan(v))        c.seed = (unsigned long)v;
        else if (strcmp(key, "stw_polar") == 0) c.stw_polar = !isnan(v) && v != 0;
        else if (strcmp(key, "polar") == 0 && strcmp(val, "auto") != 0)
            snprintf(c.polar, sizeof c.polar, "%s", val);
    }
    fclose(f);
    g_ctl = c;
}

/* Relit le fichier de contrôle si sa date ou sa taille a changé, au plus une
 * fois par tour de boucle. Retourne 1 si l'état a été rechargé. */
static int ctl_refresh(void)
{
    /* inode + date à la nanoseconde + taille. La date à la seconde ne suffisait
     * PAS : deux réglages de même longueur écrits dans la même seconde (curseur
     * déplacé vite, « hdg = 45.00 » puis « hdg = 46.00 ») passaient inaperçus.
     * L'interface écrit par rename, donc chaque version a un nouvel inode. */
    static ino_t last_ino; static struct timespec last_mt; static long last_sz = -1;
    if (!g_ctl_path)
        return 0;
    struct stat sb;
    if (stat(g_ctl_path, &sb) != 0)
        return 0;
    if (sb.st_ino == last_ino && sb.st_mtim.tv_sec == last_mt.tv_sec &&
        sb.st_mtim.tv_nsec == last_mt.tv_nsec && (long)sb.st_size == last_sz)
        return 0;
    last_ino = sb.st_ino;
    last_mt  = sb.st_mtim;
    last_sz  = (long)sb.st_size;
    ctl_load(g_ctl_path);
    return 1;
}

/* --- Hasard reproductible ----------------------------------------------------
 * splitmix64 : minuscule, sans état global caché, et surtout REPRODUCTIBLE avec
 * une graine donnée (seed), ce qui permet de tester l'aléa. */
static uint64_t g_rng;
static int      g_rng_seeded;

static double rnd01(void)
{
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (double)(z >> 11) / 9007199254740992.0;   /* [0, 1) */
}

static double rnd_between(double a, double b) { return a + (b - a) * rnd01(); }

/* --- Vent aléatoire par SÉQUENCES --------------------------------------------
 * Sur l'eau, le vent tient un régime puis bascule vers un autre, par séquences
 * d'une dizaine de minutes. Chaque séquence tire au hasard :
 *   - sa cible, uniformément dans l'amplitude TOTALE (±amplitude/2) ;
 *   - sa durée, entre 0,5 et 1,5 fois la période ;
 *   - la vitesse de sa transition : la bascule occupe entre 15 % et 85 % de la
 *     séquence, puis le vent tient la cible jusqu'à la séquence suivante.
 * La transition est adoucie en cosinus : ni à-coup au départ ni à l'arrivée.
 * Force et direction ont chacune leur propre suite de séquences, indépendante. */
typedef struct {
    double from, to;   /* décalage au début et à la fin de la transition */
    double t0;         /* début de la séquence (s) */
    double dur;        /* durée totale de la séquence (s) */
    double ramp;       /* durée de la transition (s) */
    int    init;
} wseq_t;

static wseq_t g_seq_tws, g_seq_twd;

/* Décalage courant d'une suite de séquences, dans [-amp/2, +amp/2]. */
static double wseq_value(wseq_t *q, double t, double amp, double period_s)
{
    double half = amp / 2.0;
    if (!q->init) {
        q->from = 0.0;            /* on part de la base, sans saut */
        q->to   = rnd_between(-half, half);
        q->t0   = t;
        q->dur  = period_s * rnd_between(0.5, 1.5);
        q->ramp = q->dur * rnd_between(0.15, 0.85);
        q->init = 1;
    }
    while (t >= q->t0 + q->dur) {  /* séquence(s) écoulée(s) : on enchaîne */
        q->from = q->to;
        q->to   = rnd_between(-half, half);
        q->t0  += q->dur;
        q->dur  = period_s * rnd_between(0.5, 1.5);
        q->ramp = q->dur * rnd_between(0.15, 0.85);
    }
    double x = (t - q->t0) / q->ramp;
    double v = (x >= 1.0) ? q->to
             : q->from + (q->to - q->from) * (1.0 - cos(M_PI * x)) / 2.0;
    /* amplitude réduite en cours de route : on reste dans la nouvelle borne */
    if (v >  half) v =  half;
    if (v < -half) v = -half;
    return v;
}

/* --- Entrées en « auto » : on repart de la dernière valeur réglée ------------
 * Avant, « auto » reprenait un centre codé en dur (225° pour le vent, 90° pour
 * le cap…) : régler le vent au 300 puis recocher « auto » le renvoyait au 225,
 * et le cap pouvait sauter de 55°. Désormais :
 *   - tant qu'une entrée est réglée, on retient sa valeur ;
 *   - au passage en auto, la sinusoïde est CENTRÉE sur cette valeur et CALÉE
 *     pour valoir exactement ce centre à l'instant de la bascule : pas de saut ;
 *   - au démarrage, faute de valeur réglée connue, le centre vient du fichier
 *     (« auto N ») puis, à défaut, de la valeur historique par défaut. */
static double norm360(double a);   /* défini plus bas */

typedef struct {
    double last;       /* dernière valeur réglée */
    int    has_last;
    double center;     /* centre de la variation automatique */
    double t0;         /* instant de la bascule en auto (phase nulle) */
    int    in_auto;
} autoin_t;

static autoin_t g_ai_hdg, g_ai_stw, g_ai_set, g_ai_drift, g_ai_twd, g_ai_tws;

/* Valeur d'une entrée. `manual` NAN = auto. Amplitude `amp`, période `period`
 * (s) ; `deriv` reçoit la dérivée (unités/s) si non NULL. */
static double input_value(autoin_t *a, double manual, double file_center,
                          double def_center, double t, double amp, double period,
                          int wrap, double *deriv)
{
    if (!isnan(manual)) {
        a->last = manual;
        a->has_last = 1;
        a->in_auto = 0;
        if (deriv) *deriv = 0.0;
        return wrap ? norm360(manual) : manual;
    }
    if (!a->in_auto) {
        a->center = a->has_last ? a->last
                  : (!isnan(file_center) ? file_center : def_center);
        a->t0 = t;
        a->in_auto = 1;
    }
    double x = (t - a->t0) / period;
    if (deriv) *deriv = amp / period * cos(x);
    double v = a->center + amp * sin(x);
    return wrap ? norm360(v) : v;
}

/* --- Polaire (rechargée quand le chemin change) --- */
static polar_t g_polar;
static char    g_polar_loaded[512];
static int     g_polar_ok;

static void polar_refresh(void)
{
    if (strcmp(g_ctl.polar, g_polar_loaded) == 0)
        return;
    snprintf(g_polar_loaded, sizeof g_polar_loaded, "%s", g_ctl.polar);
    g_polar_ok = g_ctl.polar[0] && polar_load(&g_polar, g_ctl.polar);
    if (g_ctl.polar[0] && !g_polar_ok)
        fprintf(stderr, "n2k-sim : polaire refusée (%s) : %s\n",
                g_ctl.polar, g_polar.err);
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

    /* --- ENTRÉES : cap et courant. Chacun vient du fichier de contrôle s'il y
     * est fixé, sinon d'une sinusoïde. --- */
    /* cap : vire en S (±55°) ; la giration est la dérivée du cap */
    boat.hdg = input_value(&g_ai_hdg, g_ctl.hdg, g_ctl.c_hdg, 90.0,
                           t, 55.0, 70.0, 1, &boat.rot);
    boat.set = input_value(&g_ai_set, g_ctl.set, g_ctl.c_set, 120.0,
                           t, 10.0, 40.0, 1, NULL);
    boat.drift = input_value(&g_ai_drift, g_ctl.drift, g_ctl.c_drift, 0.5,
                             t, 0.2, 25.0, 0, NULL);
    if (boat.drift < 0) boat.drift = 0;

    /* --- VENT VRAI : entrée (direction + vitesse). Avec l'aléa, twd/tws sont
     * la BASE autour de laquelle le vent évolue par séquences ; laissés à
     * « auto », la base est fixe (225°, 15 nds) plutôt que sinusoïdale. --- */
    if (g_ctl.wind_random) {
        if (!g_rng_seeded) {
            g_rng = g_ctl.seed ? (uint64_t)g_ctl.seed : (uint64_t)now_ms();
            g_rng_seeded = 1;
        }
        /* base = valeur réglée, ou centre de l'auto (amplitude nulle : l'aléa
         * remplace la sinusoïde) ; plus de retour au 225 en recochant auto */
        boat.twd_base = input_value(&g_ai_twd, g_ctl.twd, g_ctl.c_twd, WIND_BASE_TWD,
                                    t, 0.0, 60.0, 1, NULL);
        boat.tws_base = input_value(&g_ai_tws, g_ctl.tws, g_ctl.c_tws,
                                    WIND_BASE_TWS_KN * KN_TO_MS, t, 0.0, 8.0, 0, NULL);
        double period = g_ctl.wind_period * 60.0;
        double pct = wseq_value(&g_seq_tws, t, g_ctl.tws_var, period);
        double deg = wseq_value(&g_seq_twd, t, g_ctl.twd_var, period);
        boat.tws = boat.tws_base * (1.0 + pct / 100.0);
        boat.twd = norm360(boat.twd_base + deg);
    } else {
        /* aléa coupé : on repartira d'une séquence neuve à la réactivation */
        g_seq_tws.init = g_seq_twd.init = 0;
        boat.twd = input_value(&g_ai_twd, g_ctl.twd, g_ctl.c_twd, 225.0,
                               t, 15.0, 60.0, 1, NULL);
        boat.tws = input_value(&g_ai_tws, g_ctl.tws, g_ctl.c_tws, 9.0,
                               t, 2.0, 8.0, 0, NULL);
        boat.twd_base = boat.twd;
        boat.tws_base = boat.tws;
    }
    if (boat.tws < 0) boat.tws = 0;
    boat.twa = norm360(boat.twd - boat.hdg);

    /* --- VENT VRAI RÉFÉRENCÉ EAU : vent vrai moins le courant, ramené à
     * l'étrave. C'est le vent dans lequel le bateau navigue, donc celui qui
     * indexe la polaire. Il ne dépend pas de la vitesse surface : pas de boucle
     * de calcul. --- */
    double cn, ce;
    vec_of(boat.set, boat.drift, &cn, &ce);
    {
        double an, ae, dir, mag;
        vec_of(norm360(boat.twd + 180.0), boat.tws, &an, &ae);
        dir_of(an - cn, ae - ce, &dir, &mag);
        boat.twa_w = norm360(dir + 180.0 - boat.hdg);
        boat.tws_w = mag;
    }

    /* --- VITESSE SURFACE : polaire si demandée et lisible, sinon l'entrée. --- */
    polar_refresh();
    boat.stw_from_polar = g_ctl.stw_polar && g_polar_ok;
    if (boat.stw_from_polar)
        boat.stw = polar_speed(&g_polar, boat.twa_w, boat.tws_w / KN_TO_MS) * KN_TO_MS;
    else
        boat.stw = input_value(&g_ai_stw, g_ctl.stw, g_ctl.c_stw, 4.5,
                               t, 1.0, 40.0, 0, NULL);
    if (boat.stw < 0) boat.stw = 0;

    /* --- CALCULÉ : route/vitesse FOND = vecteur surface + vecteur courant. --- */
    double wn, we, gn, ge;
    vec_of(boat.hdg, boat.stw, &wn, &we);
    gn = wn + cn;
    ge = we + ce;
    dir_of(gn, ge, &boat.cog, &boat.sog);

    /* --- VENT APPARENT = vent vrai (mouvement de l'air) − vecteur bateau/fond --- */
    {
        double an, ae, dir, mag;
        vec_of(norm360(boat.twd + 180.0), boat.tws, &an, &ae);
        dir_of(an - gn, ae - ge, &dir, &mag);
        boat.awa = norm360(dir + 180.0 - boat.hdg);
        boat.aws = mag;
    }

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
            "awa = %.1f\naws = %.2f\ntwa_w = %.1f\ntws_w = %.2f\n"
            "twd_base = %.1f\ntws_base = %.2f\nwind_random = %d\n"
            "stw_polar = %d\npolar_ok = %d\nlat = %.6f\nlon = %.6f\n",
            g_ctl.enabled, boat.hdg, boat.stw / KN_TO_MS, boat.cog,
            boat.sog / KN_TO_MS, boat.set, boat.drift / KN_TO_MS,
            boat.twd, boat.tws / KN_TO_MS, boat.twa, boat.awa,
            boat.aws / KN_TO_MS, boat.twa_w, boat.tws_w / KN_TO_MS,
            boat.twd_base, boat.tws_base / KN_TO_MS, g_ctl.wind_random,
            boat.stw_from_polar, g_polar_ok, boat.lat, boat.lon);
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
    *ta = boat.twa_w;              /* calculé une fois par pas (boat_update) */
    *ts = boat.tws_w;
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
    /* cap magnétique = cap vrai − variation − déviation. Déviation NULLE : le
     * compas simulé est compensé. Avant, une déviation de 1,5° était annoncée
     * sans être appliquée, si bien que HDG donnait un cap vrai de 260,5° quand
     * HDT disait 259° (variation 2°W = −2). */
    double hdg_mag = norm360(boat.hdg + 2.0);
    snprintf(f, sizeof f,
             "\"Heading\":%.1f,\"Deviation\":0.0,\"Variation\":-2.0,\"Reference\":\"Magnetic\"",
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
    /* pas de champ Yaw : non disponible, comme en trames (cf. a_attitude) */
    snprintf(f, sizeof f, "\"Pitch\":%.1f,\"Roll\":%.1f",
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

/* Flux des trames binaires (format texte actisense). stdout en mode
 * --actisense ; un FIFO distinct avec --actisense-out, pour servir le N2K
 * (ydraw-bridge → port 2700) EN MÊME TEMPS que le JSON, depuis le même état. */
static FILE *g_act_fp = NULL;

static void emit_frame(int prio, int src, int pgn, const uint8_t *d, int len)
{
    FILE *o = g_act_fp ? g_act_fp : stdout;
    char ts[40]; ts_now(ts, sizeof ts);
    /* une ligne = une écriture : deux trames ne s'entremêlent jamais */
    char line[1024];
    int n = snprintf(line, sizeof line, "%s,%d,%d,%d,255,%d", ts, prio, pgn, src, len);
    for (int i = 0; i < len && n > 0 && (size_t)n < sizeof line - 4; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, ",%02x", d[i]);
    if (n > 0 && (size_t)n < sizeof line - 1) {
        line[n++] = '\n';
        fwrite(line, 1, (size_t)n, o);
    }
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
    /* Lacet « non disponible » (0x7FFF), et SURTOUT pas 0 : un lacet à 0° est
     * une vraie valeur, qu'un récepteur peut prendre pour le cap. qtVlm voyait
     * alors le cap osciller entre 127250 (cap réel) et 0°, et en déduisait un
     * courant fictif. Le compas simulé ne publie pas de lacet. */
    p16(b, 1, 0x7FFF);
    p16(b, 3, (int)lround(DEG2RAD(3.0 * sin(t / 5.0)) / 1e-4));
    p16(b, 5, (int)lround(DEG2RAD(8.0 * sin(t / 7.0)) / 1e-4));
    emit_frame(2, SCX_SRC, 127257, b, 7);
}

/* Une trame 130306 : vitesse (m/s), angle (deg) et code de référence
 * (WIND_REFERENCE : 0 vrai/nord, 2 apparent, 4 vrai/eau). Les 5 bits hauts de
 * l'octet 5 sont réservés, donc à 1. */
static void a_wind1(double speed_ms, double angle_deg, int ref)
{
    uint8_t b[8] = { 0xff, 0, 0, 0, 0, 0, 0xff, 0xff };
    p16(b, 1, (int)lround(speed_ms / 0.01));
    p16(b, 3, (int)lround(DEG2RAD(angle_deg) / 1e-4));
    b[5] = (uint8_t)(0xf8 | (ref & 0x07));
    emit_frame(2, MAD_SRC, 130306, b, 8);
}

static void a_wind(double t)       /* 130306 Wind Data : apparent + vrai (eau) */
{
    (void)t;
    a_wind1(boat.aws,   boat.awa,   2);   /* apparent, angle / étrave */
    a_wind1(boat.tws_w, boat.twa_w, 4);   /* vrai référencé eau, angle / étrave */
    /* PAS de trame « True (ground referenced to North) » en N2K : son champ
     * Wind Angle porte une DIRECTION (43°) là où les deux autres portent un
     * angle à l'étrave (144°). Un récepteur qui ne distingue pas la référence
     * voit le vent vrai sauter de l'une à l'autre, quatre fois par seconde.
     * Un instrument réel publie apparent + vrai relatif ; le récepteur déduit la
     * direction du cap. Le flux JSON la garde : n2k-mux en tire MWD en 0183. */
}

static void a_setdrift(void)       /* 129291 Set & Drift, Rapid Update */
{
    uint8_t b[8] = { 0xff, 0xfc, 0, 0, 0, 0, 0xff, 0xff };  /* SID ; réf. vraie (0) */
    p16(b, 2, (int)lround(DEG2RAD(boat.set) / 1e-4));
    p16(b, 4, (int)lround(boat.drift / 0.01));
    emit_frame(3, MAD_SRC, 129291, b, 8);
}

static void a_stw(double t)        /* 128259 Speed (water referenced) */
{
    (void)t;
    uint8_t b[8] = { 0xff, 0, 0, 0xff, 0xff, 0, 0xff, 0xff };
    /* STW de l'état : réglée, sinusoïdale ou tirée de la polaire, comme en JSON */
    p16(b, 1, (int)lround(boat.stw / 0.01));
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

/* Ordonnancement des trames binaires, partagé par le mode --actisense et par
 * --actisense-out (JSON + trames en parallèle). id → émetteur, iv = période ms. */
static struct { int id; double iv, next; } ACT_SCHED[] = {
    {0,250,0},{1,250,0},{2,200,0},{3,500,0},{4,200,0},
    {5,250,0},{6,500,0},{7,500,0},{8,2000,0},{9,2000,0},{10,1000,0},
    {11,2000,0},{12,200,0},{13,1000,0},{14,5000,0},{15,2000,0},
    {16,2000,0},{17,3000,0},{18,1000,0},
};

/* Émet les trames dues à l'instant el (ms depuis le départ). */
static void act_tick(double el, double t, int once, int no_ais)
{
    int n = (int)(sizeof ACT_SCHED / sizeof *ACT_SCHED);
    for (int i = 0; i < n; i++) {
        if (!once && el < ACT_SCHED[i].next) continue;
        switch (ACT_SCHED[i].id) {
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
            case 18: a_setdrift(); break;
        }
        /* sans dérive : on avance l'échéance d'une période, pas « maintenant +
         * période » ; très en retard (reprise après désactivation), on recale */
        ACT_SCHED[i].next += ACT_SCHED[i].iv;
        if (ACT_SCHED[i].next < el)
            ACT_SCHED[i].next = el + ACT_SCHED[i].iv;
    }
    fflush(g_act_fp ? g_act_fp : stdout);
}

/* Attend la prochaine échéance ABSOLUE de la boucle (multiple de tick depuis
 * le départ), au lieu de dormir « tick » après le traitement. Dormir un pas
 * fixe laissait dériver la boucle (pas réel 100 ms + traitement), et les PGN à
 * 250 ms partaient en fait toutes les 300 ms : 3,3 Hz au lieu de 4 Hz. */
static void sleep_until_next_tick(uint64_t start, long tick_ms)
{
    uint64_t now = now_ms();
    uint64_t k = (now - start) / (uint64_t)tick_ms + 1;
    uint64_t due = start + k * (uint64_t)tick_ms;
    if (due > now)
        usleep((useconds_t)((due - now) * 1000));
}

static int run_actisense(double duration, long tick, int once, int no_ais)
{
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
            sleep_until_next_tick(start, tick);
            continue;
        }
        act_tick(el, t, once, no_ais);
        if (once) return 0;
        sleep_until_next_tick(start, tick);
    } while (!g_stop);
    return 0;
}

/* --- Ordonnanceur (mode JSON) --- */
typedef void (*emit_fn)(double);
typedef struct { emit_fn fn; double iv_ms; double next_ms; int is_ais; const char *name; } sched_t;

static sched_t SCHED[] = {
    { e_identity,    10000, 0, 0, "identité (60928/126996)" },
    { e_pos,           250, 0, 0, "129025 → GLL" },
    { e_cogsog,        250, 0, 0, "129026 → VTG" },   /* Rapid Update : 4 Hz */
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
        "  --tick MS       période de la boucle d'émission (défaut 50 ms)\n"
        "  --control FIC   pilotage à chaud, SIX entrées : enabled, hdg, stw, set,\n"
        "                  drift, twd, tws. Relu à chaque changement du fichier ;\n"
        "                  c'est ce que l'interface web écrit. Vitesses en nœuds,\n"
        "                  angles en degrés. TWA, AWA et AWS en découlent.\n"
        "  --state FIC     publie l'état DÉDUIT (cog, sog, twa, awa, aws…) dans ce\n"
        "                  fichier, même format, pour affichage par l'interface web\n"
        "  --actisense-out FIC  émet AUSSI les trames N2K binaires (format actisense)\n"
        "                  dans FIC (typiquement un FIFO lu par ydraw-bridge), en plus\n"
        "                  du JSON sur stdout, depuis le MÊME état bateau\n"
        "  --wind-trace S  déroule S secondes de temps SIMULÉ sans attendre et imprime\n"
        "                  « t;twd;tws;stw » toutes les 10 s (réglage de l'aléa et de\n"
        "                  la polaire, tests). Utilise --control ; puis s'arrête.\n"
        "\nÉmet du JSON façon `analyzer -json` pour tous les PGN compris par\n"
        "n2k-mux + l'identité. Exemple : %s | ./n2k-mux n2k-sim.ini -v\n",
        p, p);
}

int main(int argc, char **argv)
{
    int once = 0, no_ais = 0, actisense = 0;
    double wind_trace = 0;     /* --wind-trace : secondes de temps simulé */
    const char *act_out = NULL; /* --actisense-out : trames N2K en parallèle */
    double duration = 0;       /* secondes ; 0 = sans fin */
    long tick_ms = 50;   /* 200, 250 et 500 ms en sont des multiples exacts */

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--once") == 0)      once = 1;
        else if (strcmp(argv[i], "--no-ais") == 0)    no_ais = 1;
        else if (strcmp(argv[i], "--actisense") == 0) actisense = 1;
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) duration = atof(argv[++i]);
        else if (strcmp(argv[i], "--tick") == 0 && i + 1 < argc)     tick_ms = atol(argv[++i]);
        else if (strcmp(argv[i], "--control") == 0 && i + 1 < argc)  g_ctl_path = argv[++i];
        else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc)    g_state_path = argv[++i];
        else if (strcmp(argv[i], "--wind-trace") == 0 && i + 1 < argc) wind_trace = atof(argv[++i]);
        else if (strcmp(argv[i], "--actisense-out") == 0 && i + 1 < argc) act_out = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "option inconnue : %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (tick_ms < 10) tick_ms = 10;

    /* --wind-trace : pas de flux NMEA, juste la courbe du vent et de la vitesse
     * surface en temps simulé. Le vent par séquences dure des dizaines de
     * minutes : on ne va pas l'attendre en temps réel pour le régler. */
    if (wind_trace > 0) {
        ctl_refresh();
        printf("t_s;twd;tws_kn;stw_kn\n");
        for (double t = 0; t <= wind_trace; t += 10.0) {
            boat_update(t);
            printf("%.0f;%.1f;%.2f;%.2f\n", t, boat.twd, boat.tws / KN_TO_MS,
                   boat.stw / KN_TO_MS);
        }
        return 0;
    }

    signal(SIGINT, on_int);
    signal(SIGTERM, on_int);
    setvbuf(stdout, NULL, _IOLBF, 0);   /* sortie ligne par ligne (pipe) */

    /* Mode --actisense : trames N2K binaires (→ ydraw-bridge → qtVlm en N2K).
     * Pas d'en-tête JSON ici (format texte actisense). AIS inclus (fast-packet). */
    if (actisense)
        return run_actisense(duration, tick_ms, once, no_ais);

    /* --actisense-out : second flux, TRAMES N2K, depuis le même état que le JSON.
     * Sur un FIFO, l'ouverture attend le lecteur (ydraw-bridge), lancé avant.
     * Si le lecteur meurt, l'écriture lève SIGPIPE et le simulateur s'arrête :
     * la chaîne le voit et systemd relance tout, plutôt qu'un port 2700 muet. */
    if (act_out) {
        g_act_fp = fopen(act_out, "w");
        if (!g_act_fp) {
            fprintf(stderr, "n2k-sim : --actisense-out %s : ouverture impossible\n", act_out);
            return 1;
        }
        setvbuf(g_act_fp, NULL, _IOLBF, 0);
    }

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
        if (g_act_fp)
            act_tick(0.0, 0.0, 1, no_ais);
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
            sleep_until_next_tick(start, tick_ms);
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
                SCHED[i].next_ms += SCHED[i].iv_ms;      /* sans dérive */
                if (SCHED[i].next_ms < el)
                    SCHED[i].next_ms = el + SCHED[i].iv_ms;
            }
        }
        fflush(stdout);
        if (g_act_fp)
            act_tick(el, t, 0, no_ais);  /* même pas de temps, même état */
        sleep_until_next_tick(start, tick_ms);
    }
    return 0;
}
