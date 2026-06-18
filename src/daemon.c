/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * daemon.c — n2k-mux : multiplexeur NMEA 2000 → NMEA 0183.
 *
 * Filtre Unix : lit la sortie JSON-lines de `analyzer -json` sur stdin, et
 * écrit des phrases NMEA 0183 sur stdout (vers kplex). Pour chaque ligne :
 *
 *   jsonl_parse → registry_observe → arbiter_decide → mapper_map → stdout
 *
 * Chaîne de production :
 *   actisense-serial -r -s 230400 /dev/ttyNGX1 | analyzer -json | n2k-mux conf.ini | kplex
 *
 * Identités (registre) : les PGN 60928/126996 ne circulent pas en passif. Avec
 * --tx <chemin>, le daemon écrit périodiquement des ISO Request (PGN 59904) sur
 * ce chemin (typiquement un FIFO relié au stdin d'un actisense-serial en mode
 * bidirectionnel, c.-à-d. SANS -r), pour faire répondre les devices. Sans --tx,
 * le daemon reste un simple filtre (le registre se peuple si les identités
 * arrivent par un autre moyen).
 *
 * Usage : n2k-mux [config.ini] [--tx CHEMIN] [--tx-interval SEC] [--no-0183] [-v]
 *
 * Le flux 0183 sort sur stdout (vers kplex). --no-0183 désactive toute génération
 * 0183 : l'arbitrage tourne encore (registre, sources, stats), prélude au futur
 * flux N2K arbité (qtVlm lit le N2K natif) qui réutilisera le module netout.
 */

#include "jsonl.h"
#include "registry.h"
#include "config.h"
#include "arbiter.h"
#include "mapper.h"
#include "aisdedup.h"
#include "sources.h"
#include "stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

/* Requêtes ISO (PGN 59904) demandant 60928 puis 126996 à tous (dst=255).
 * Format actisense-serial : ,prio,pgn,src,dst,bytes,b0,b1,... (src ignoré). */
static const char *ISO_REQUESTS[] = {
    ",6,59904,0,255,3,00,ee,00\n",   /* 60928  ISO Address Claim */
    ",6,59904,0,255,3,14,f0,01\n",   /* 126996 Product Information */
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Reload de la config sur SIGHUP : le handler ne fait que lever un drapeau, le
 * rechargement réel se fait dans la boucle (entre deux lignes). Avec signal(),
 * fgets n'est PAS interrompu (SA_RESTART) → la nouvelle config prend effet à la
 * ligne suivante ; sans objet pour un flux N2K vivant (jamais inactif). */
static volatile sig_atomic_t g_reload = 0;
static void on_hup(int sig) { (void)sig; g_reload = 1; }

/* Recharge `path` dans une config temporaire ; ne remplace `*cfg` QUE si le
 * parsing réussit (sinon l'ancienne config reste active). Renvoie true si rechargé. */
static bool config_reload(config_t *cfg, const char *path)
{
    if (!path) {
        fprintf(stderr, "n2k-mux : SIGHUP ignoré (aucun fichier de config)\n");
        return false;
    }
    config_t tmp;
    config_init(&tmp);
    if (!config_load(&tmp, path)) {
        fprintf(stderr, "n2k-mux : reload échoué (%s : %s ligne %d) — "
                "ancienne config conservée\n", path, tmp.err, tmp.err_line);
        return false;
    }
    *cfg = tmp;
    fprintf(stderr, "n2k-mux : config rechargée (%s)\n", path);
    return true;
}

static void emit_iso_requests(int txfd)
{
    if (txfd < 0)
        return;
    for (size_t i = 0; i < sizeof ISO_REQUESTS / sizeof *ISO_REQUESTS; i++) {
        ssize_t n = write(txfd, ISO_REQUESTS[i], strlen(ISO_REQUESTS[i]));
        (void)n;   /* best-effort : pas de lecteur / FIFO pleine → on ignore */
    }
}

/* --- Throttle de la sortie 0183 (limite de débit par type de phrase) --- */
#define THR_MAX 32
typedef struct {
    char     type[4];   /* type de phrase, ex. "GSV" */
    uint64_t last;      /* dernière émission (ms) */
} thr_entry_t;
typedef struct { thr_entry_t e[THR_MAX]; int n; } throttle_t;

/* Type d'une phrase "$ttTTT,..." → les 3 caractères suivant le talker (2). */
static void sentence_type(const char *s, char out[4])
{
    out[0] = '\0';
    if (s[0] != '$' && s[0] != '!')
        return;
    /* champ d'adresse = jusqu'à la virgule ; type = 3 derniers caractères. */
    const char *comma = strchr(s, ',');
    size_t addr = comma ? (size_t)(comma - (s + 1)) : strlen(s + 1);
    if (addr < 3)
        return;
    const char *t = s + 1 + addr - 3;
    out[0] = t[0]; out[1] = t[1]; out[2] = t[2]; out[3] = '\0';
}

/* Pointeur vers l'horodatage de dernière émission d'un type (créé au besoin). */
static uint64_t *thr_last(throttle_t *t, const char *type)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->e[i].type, type) == 0)
            return &t->e[i].last;
    if (t->n >= THR_MAX)
        return NULL;
    thr_entry_t *e = &t->e[t->n++];
    snprintf(e->type, sizeof e->type, "%s", type);
    e->last = 0;
    return &e->last;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage : %s [config.ini] [--tx CHEMIN] [--tx-interval SEC] [--ais-json] [-v]\n"
        "  config.ini       sources nommées, priorités, exclusions\n"
        "  --tx CHEMIN      émet des ISO Request sur CHEMIN (FIFO vers actisense-serial)\n"
        "  --tx-interval N  période d'émission en secondes (défaut 30)\n"
        "  --ais-json       mode filtre AIS : JSON->JSON dédupliqué par MMSI,\n"
        "                   à brancher devant n2kd (cf. fusion des sources AIS)\n"
        "  --sources CHEMIN publie les sources vues en JSON (pour la GUI)\n"
        "  --sources-interval N  période de publication en secondes (défaut 5)\n"
        "  --stats CHEMIN   publie le débit/PGN et la charge de bus estimée en JSON\n"
        "  --stats-interval N    période de publication des stats (défaut 5)\n"
        "  --no-0183        n'émet aucune phrase NMEA 0183 (arbitrage seul)\n"
        "  -v, --verbose    journalise les décisions + un résumé stats sur stderr\n",
        prog);
}

/*
 * Mode --ais-json : lit le JSON de l'analyzer, ne réémet que la ligne d'en-tête
 * et les messages AIS de la source RETENUE par MMSI (fusion em-trak > DataHub).
 * Le reste est filtré. Sortie destinée au stdin de n2kd (qui encode le VDM).
 */
static int run_ais_json(config_t *cfg, const char *cfg_path, int verbose)
{
    registry_t reg;
    registry_init(&reg);
    aisdedup_t dd;
    aisdedup_init(&dd, cfg, &reg);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, on_hup);

    char line[8192];
    unsigned long n_ais = 0, n_fwd = 0;

    while (fgets(line, sizeof line, stdin)) {
        uint64_t now = now_ms();

        if (g_reload) {
            g_reload = 0;
            if (config_reload(cfg, cfg_path))
                aisdedup_init(&dd, cfg, &reg);   /* repart sur les nouvelles règles */
        }
        jsonl_msg_t m;
        if (!jsonl_parse(line, &m))
            continue;
        registry_observe(&reg, &m);   /* capte les identités (60928/126996) */

        if (m.is_header) {            /* n2kd exige l'en-tête analyzer */
            fputs(line, stdout);
            if (fflush(stdout) != 0) break;
            continue;
        }
        if (!m.has_pgn || !aisdedup_is_ais(m.pgn))
            continue;                 /* non-AIS : filtré (n2k-mux gère le reste) */

        n_ais++;
        uint32_t mmsi = 0;
        const char *s; double v;
        if (jsonl_get_str(&m, "User ID", &s))      mmsi = (uint32_t)strtoul(s, NULL, 10);
        else if (jsonl_get_num(&m, "User ID", &v)) mmsi = (uint32_t)v;
        int src = m.has_src ? m.src : -1;

        bool fwd = (mmsi == 0 || src < 0)
                 ? true                /* indéterminé : on laisse passer */
                 : aisdedup_accept(&dd, m.pgn, mmsi, src, now);

        if (verbose && mmsi)
            fprintf(stderr, "AIS mmsi=%u src=%d pgn=%d %s\n",
                    mmsi, src, m.pgn, fwd ? "FWD" : "drop");

        if (fwd) {
            fputs(line, stdout);
            if (fflush(stdout) != 0) break;
            n_fwd++;
        }
    }

    fprintf(stderr, "n2k-mux --ais-json : %lu messages AIS, %lu transmis.\n", n_ais, n_fwd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cfg_path = NULL;
    const char *tx_path  = NULL;
    const char *sources_path = NULL;
    const char *stats_path = NULL;
    unsigned    tx_interval = 30;
    unsigned    sources_interval = 5;
    unsigned    stats_interval = 5;
    int         verbose = 0;
    int         ais_json = 0;
    int         gen_0183 = 1;    /* 0 avec --no-0183 : arbitrage sans sortie 0183 */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ais-json") == 0) {
            ais_json = 1;
        } else if (strcmp(argv[i], "--sources") == 0 && i + 1 < argc) {
            sources_path = argv[++i];
        } else if (strcmp(argv[i], "--sources-interval") == 0 && i + 1 < argc) {
            sources_interval = (unsigned)strtoul(argv[++i], NULL, 10);
            if (sources_interval == 0) sources_interval = 5;
        } else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) {
            stats_path = argv[++i];
        } else if (strcmp(argv[i], "--stats-interval") == 0 && i + 1 < argc) {
            stats_interval = (unsigned)strtoul(argv[++i], NULL, 10);
            if (stats_interval == 0) stats_interval = 5;
        } else if (strcmp(argv[i], "--tx") == 0 && i + 1 < argc) {
            tx_path = argv[++i];
        } else if (strcmp(argv[i], "--tx-interval") == 0 && i + 1 < argc) {
            tx_interval = (unsigned)strtoul(argv[++i], NULL, 10);
            if (tx_interval == 0) tx_interval = 30;
        } else if (strcmp(argv[i], "--no-0183") == 0) {
            gen_0183 = 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            cfg_path = argv[i];
        } else {
            fprintf(stderr, "option inconnue : %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    config_t cfg;
    config_init(&cfg);
    if (cfg_path) {
        if (!config_load(&cfg, cfg_path)) {
            fprintf(stderr, "config : %s (ligne %d)\n", cfg.err, cfg.err_line);
            return 1;
        }
    } else {
        fprintf(stderr, "n2k-mux : aucune config fournie — aucune règle, sortie vide.\n");
    }

    if (ais_json)
        return run_ais_json(&cfg, cfg_path, verbose);

    registry_t reg;
    registry_init(&reg);
    arbiter_t arb;
    arbiter_init(&arb, &cfg, &reg);
    mapper_t mp;
    mapper_init(&mp, cfg.talker);
    throttle_t thr = {0};   /* limite de débit 0183 par type ([rate]) */
    stats_t st;
    stats_init(&st, now_ms());   /* trafic N2K / charge de bus estimée */

    /* kplex peut fermer le tube : on gère l'erreur d'écriture nous-mêmes. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, on_hup);   /* reload config à chaud */

    /* Canal d'émission optionnel. O_RDWR sur un FIFO : pas d'interblocage de
     * rendez-vous, pas de blocage si aucun lecteur, écriture bufferisée. */
    int txfd = -1;
    uint64_t last_tx = 0;
    uint64_t last_sources = 0;
    uint64_t last_stats = now_ms();
    if (tx_path) {
        txfd = open(tx_path, O_RDWR | O_NONBLOCK);
        if (txfd < 0)
            fprintf(stderr, "n2k-mux : --tx %s : %s\n", tx_path, strerror(errno));
        else {
            emit_iso_requests(txfd);   /* requête initiale */
            last_tx = now_ms();
        }
    }

    char line[8192];
    unsigned long n_lines = 0, n_accept = 0, n_sent = 0;

    while (fgets(line, sizeof line, stdin)) {
        uint64_t now = now_ms();

        if (g_reload) {
            g_reload = 0;
            if (config_reload(&cfg, cfg_path)) {
                /* ré-init des modules dépendants ; reg/thr/st = état runtime conservé */
                arbiter_init(&arb, &cfg, &reg);
                mapper_init(&mp, cfg.talker);
            }
        }

        jsonl_msg_t m;
        if (jsonl_parse(line, &m)) {
            n_lines++;
            registry_observe(&reg, &m);
            if (m.has_pgn)
                stats_observe(&st, m.pgn);   /* tout le trafic vu (avant filtres) */

            arb_decision_t d = arbiter_decide(&arb, &m, now);
            if (d.result == ARB_ACCEPT) n_accept++;

            if (verbose && m.has_pgn)
                fprintf(stderr, "src=%-3d pgn=%-6d %-18s %s%s\n",
                        m.has_src ? m.src : -1, m.pgn, arb_result_str(d.result),
                        d.source_name ? d.source_name : "",
                        d.discriminant[0] ? d.discriminant : "");

            map_out_t out;
            if (gen_0183 && mapper_map(&mp, &m, &d, now, &out) > 0) {
                /* Throttle par type. Une rafale d'un même type produite dans le
                 * même mapper_map (ex. les pages GSV) passe en entier dès que le
                 * gate s'ouvre — sinon la pagination serait cassée. */
                char opened[MAP_MAX_SENT][4];
                int  n_opened = 0;
                int  wrote = 0;
                for (int i = 0; i < out.n; i++) {
                    char ty[4];
                    sentence_type(out.s[i], ty);
                    int iv = ty[0] ? config_rate_ms(&cfg, ty) : 0;
                    bool pass = true;
                    if (iv > 0) {
                        bool already = false;
                        for (int j = 0; j < n_opened; j++)
                            if (strcmp(opened[j], ty) == 0) { already = true; break; }
                        if (!already) {
                            uint64_t *last = thr_last(&thr, ty);
                            if (last && now - *last < (uint64_t)iv) {
                                pass = false;          /* gate fermé : on jette */
                            } else {
                                if (last) *last = now;
                                if (n_opened < MAP_MAX_SENT)
                                    snprintf(opened[n_opened++], 4, "%s", ty);
                            }
                        }
                    }
                    if (pass) {
                        fputs(out.s[i], stdout);
                        wrote++;
                        stats_observe_out(&st, ty, strlen(out.s[i]));
                    }
                }
                n_sent += wrote;
                if (wrote && fflush(stdout) != 0) {
                    fprintf(stderr, "n2k-mux : sortie fermée (%s)\n", strerror(errno));
                    break;
                }
            }
        }

        /* émission périodique des ISO Request (best-effort) */
        if (txfd >= 0 && now - last_tx >= (uint64_t)tx_interval * 1000u) {
            emit_iso_requests(txfd);
            last_tx = now;
        }

        /* publication périodique des sources vues (pour la GUI) */
        if (sources_path && now - last_sources >= (uint64_t)sources_interval * 1000u) {
            sources_write(&reg, sources_path);
            last_sources = now;
        }

        /* statistiques de trafic / charge de bus estimée */
        if ((stats_path || verbose) && now - last_stats >= (uint64_t)stats_interval * 1000u) {
            if (verbose) {
                double mps, fps, load, osps, obps, oload;
                stats_summary(&st, now, &mps, &fps, &load);
                stats_summary_out(&st, now, &osps, &obps, &oload);
                fprintf(stderr, "n2k-mux stats : N2K %.1f msg/s ~%.1f trames/s charge ~%.1f%% | "
                        "0183 %.1f phr/s %.0f o/s charge ~%.1f%%\n",
                        mps, fps, load, osps, obps, oload);
            }
            if (stats_path) stats_write(&st, stats_path, now);
            else            stats_reset(&st, now);
            last_stats = now;
        }
    }

    if (sources_path)
        sources_write(&reg, sources_path);   /* dernière publication à l'arrêt */
    if (txfd >= 0) close(txfd);
    fprintf(stderr, "n2k-mux : %lu lignes, %lu retenues, %lu phrases émises.\n",
            n_lines, n_accept, n_sent);
    return 0;
}
