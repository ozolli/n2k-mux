/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * stats.c — Implémentation des statistiques de trafic / charge de bus estimée.
 */

#include "stats.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <libgen.h>
#include <unistd.h>

/* Table compacte des PGN fast-packet (octets utiles) attendus sur un bus de
 * navigation + AIS. Longueurs tirées de canboat ; les PGN à longueur variable
 * reçoivent une estimation raisonnable. Tout PGN absent = single-frame (1). */
static const struct { int pgn; int bytes; } FASTPACKET[] = {
    { 126208, 223 }, { 126464, 100 }, { 126996, 134 }, { 126998, 100 },
    { 127237,  21 }, { 127489,  26 }, { 128275,  14 }, { 129029,  43 },
    { 129038,  28 }, { 129039,  27 }, { 129040,  54 }, { 129041,  60 },
    { 129044,  20 }, { 129284,  34 }, { 129285, 233 }, { 129540,  90 },
    { 129793,  24 }, { 129794,  75 }, { 129798,  27 }, { 129801,  40 },
    { 129802,  50 }, { 129809,  27 }, { 129810,  35 },
};

int stats_frames_for(int pgn)
{
    for (size_t i = 0; i < sizeof FASTPACKET / sizeof *FASTPACKET; i++) {
        if (FASTPACKET[i].pgn == pgn) {
            int b = FASTPACKET[i].bytes;
            if (b <= 8)
                return 1;
            /* fast-packet : 6 octets de données dans la 1re trame, 7 ensuite. */
            return 1 + (b - 6 + 6) / 7;   /* = 1 + ceil((b-6)/7) */
        }
    }
    return 1;   /* single-frame par défaut */
}

void stats_init(stats_t *s, uint64_t now)
{
    memset(s, 0, sizeof *s);
    s->window_start = now;
}

void stats_reset(stats_t *s, uint64_t now)
{
    s->window_start = now;
    s->msgs = 0;
    s->frames = 0;
    for (int i = 0; i < s->n_pgns; i++)
        s->pgns[i].count = 0;
    s->out_sent = 0;
    s->out_bytes = 0;
    for (int i = 0; i < s->n_types; i++)
        s->types[i].count = 0;
}

void stats_observe(stats_t *s, int pgn)
{
    s->msgs++;
    s->frames += (unsigned long)stats_frames_for(pgn);

    for (int i = 0; i < s->n_pgns; i++)
        if (s->pgns[i].pgn == pgn) {
            s->pgns[i].count++;
            s->pgns[i].total++;
            return;
        }
    if (s->n_pgns < STATS_MAX_PGNS) {
        s->pgns[s->n_pgns].pgn   = pgn;
        s->pgns[s->n_pgns].count = 1;
        s->pgns[s->n_pgns].total = 1;
        s->n_pgns++;
    }
}

void stats_observe_out(stats_t *s, const char *type, size_t bytes)
{
    s->out_sent++;
    s->out_bytes += (unsigned long)bytes;
    if (!type || !type[0])
        return;
    for (int i = 0; i < s->n_types; i++)
        if (strcmp(s->types[i].type, type) == 0) {
            s->types[i].count++;
            s->types[i].total++;
            return;
        }
    if (s->n_types < STATS_MAX_TYPES) {
        snprintf(s->types[s->n_types].type, sizeof s->types[s->n_types].type, "%s", type);
        s->types[s->n_types].count = 1;
        s->types[s->n_types].total = 1;
        s->n_types++;
    }
}

void stats_summary_out(const stats_t *s, uint64_t now,
                       double *sent_per_s, double *bytes_per_s, double *load_pct)
{
    double dt = (now > s->window_start) ? (double)(now - s->window_start) / 1000.0 : 0.0;
    double sps = dt > 0 ? (double)s->out_sent / dt : 0.0;
    double bps = dt > 0 ? (double)s->out_bytes / dt : 0.0;
    /* 0183 8N1 : ~10 bits/octet ; charge = octets/s ÷ (baud/10) */
    double load = bps / (STATS_0183_REF_BAUD / 10.0) * 100.0;
    if (sent_per_s)  *sent_per_s  = sps;
    if (bytes_per_s) *bytes_per_s = bps;
    if (load_pct)    *load_pct    = load;
}

void stats_summary(const stats_t *s, uint64_t now,
                   double *msg_per_s, double *frames_per_s, double *load_pct)
{
    double dt = (now > s->window_start) ? (double)(now - s->window_start) / 1000.0 : 0.0;
    double mps = dt > 0 ? (double)s->msgs / dt : 0.0;
    double fps = dt > 0 ? (double)s->frames / dt : 0.0;
    double load = fps * STATS_BITS_PER_FRAME / STATS_BUS_BPS * 100.0;
    if (msg_per_s)    *msg_per_s    = mps;
    if (frames_per_s) *frames_per_s = fps;
    if (load_pct)     *load_pct     = load;
}

int stats_to_json(const stats_t *s, char *buf, size_t sz, uint64_t now)
{
    double dt = (now > s->window_start) ? (double)(now - s->window_start) / 1000.0 : 0.0;
    double mps, fps, load;
    stats_summary(s, now, &mps, &fps, &load);

    double osps, obps, oload;
    stats_summary_out(s, now, &osps, &obps, &oload);

    size_t n = 0;
    int w = snprintf(buf, sz,
                     "{\"window_s\":%.1f,\"msg_per_s\":%.1f,\"frames_per_s\":%.1f,"
                     "\"bus_load_pct\":%.1f,"
                     "\"out_sent_per_s\":%.1f,\"out_bytes_per_s\":%.1f,\"out_load_pct\":%.1f,"
                     "\"pgns\":[\n", dt, mps, fps, load, osps, obps, oload);
    if (w < 0 || (size_t)w >= sz) return -1;
    n += (size_t)w;

    for (int i = 0; i < s->n_pgns; i++) {
        double hz = dt > 0 ? (double)s->pgns[i].count / dt : 0.0;
        w = snprintf(buf + n, sz - n,
                     "%s{\"pgn\":%d,\"hz\":%.2f,\"total\":%lu}\n",
                     i ? "," : "", s->pgns[i].pgn, hz, s->pgns[i].total);
        if (w < 0 || (size_t)w >= sz - n) return -1;
        n += (size_t)w;
    }
    w = snprintf(buf + n, sz - n, "],\"out_types\":[\n");
    if (w < 0 || (size_t)w >= sz - n) return -1;
    n += (size_t)w;

    for (int i = 0; i < s->n_types; i++) {
        double hz = dt > 0 ? (double)s->types[i].count / dt : 0.0;
        w = snprintf(buf + n, sz - n,
                     "%s{\"type\":\"%s\",\"hz\":%.2f,\"total\":%lu}\n",
                     i ? "," : "", s->types[i].type, hz, s->types[i].total);
        if (w < 0 || (size_t)w >= sz - n) return -1;
        n += (size_t)w;
    }
    w = snprintf(buf + n, sz - n, "]}\n");
    if (w < 0 || (size_t)w >= sz - n) return -1;
    n += (size_t)w;
    return (int)n;
}

int stats_write(stats_t *s, const char *path, uint64_t now)
{
    char buf[STATS_MAX_PGNS * 48 + STATS_MAX_TYPES * 48 + 256];
    int len = stats_to_json(s, buf, sizeof buf, now);
    if (len < 0)
        return -1;

    char dircopy[512];
    snprintf(dircopy, sizeof dircopy, "%s", path);
    char *dir = dirname(dircopy);
    if (dir && strcmp(dir, ".") != 0)
        mkdir(dir, 0755);   /* best-effort */

    char tmp[576];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    size_t wr = fwrite(buf, 1, (size_t)len, f);
    if (fclose(f) != 0 || wr != (size_t)len) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

    stats_reset(s, now);
    return 0;
}
