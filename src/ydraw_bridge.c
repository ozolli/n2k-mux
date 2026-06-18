/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * ydraw_bridge.c — Pont de TEST canboat/actisense → YDRAW → TCP.
 *
 * Lit le format texte de `actisense-serial` sur stdin :
 *     <timestamp>,prio,pgn,src,dst,len,b0,b1,...   (octets en hex)
 * convertit les messages en lignes YDRAW (Yacht Devices RAW text) et les diffuse
 * en TCP (module netout). Permet de valider la réception NMEA 2000 de qtVlm
 * (≥ 5.12.27, source NMEA TCP, auto-détection YDRAW) AVANT le passage à
 * socketcan/PEAK, en tapant la sortie d'actisense-serial.
 *
 * Le single-frame (len ≤ 8 : position 129025, COG/SOG 129026, cap 127250…) passe
 * en une trame. Les PGN fast-packet (len > 8 : AIS 129038/39/40/794/809/810…,
 * GNSS 129029…) sont RE-FRAGMENTÉS en trames CAN fast-packet (le message qu'on
 * lit ici est déjà réassemblé par actisense ; qtVlm le réassemble à nouveau). Le
 * vrai chemin socketcan, lui, recevra les trames brutes sans re-fragmenter.
 * Aucun arbitrage ici : c'est un test de format/transport, pas le daemon.
 *
 * Câblage typique (tee la sortie actisense-serial sans perturber le pipeline) :
 *   actisense-serial -s 230400 /dev/ttyNGX1 < tx.fifo \
 *     | tee >(ydraw-bridge --port 2600) | analyzer -json | n2k-mux ...
 * puis dans qtVlm : source NMEA TCP → <hôte o3nav>:2600.
 */

#include "ydraw.h"
#include "netout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* Taille max d'un message N2K fast-packet réassemblé (octets de données). */
#define N2K_MAX_DATA 223

/* Parse une ligne canboat/actisense. Retourne 1 si exploitable, 0 sinon (ligne
 * malformée, en-tête…). `data` doit pouvoir contenir N2K_MAX_DATA octets. */
static int parse_actisense(const char *line, int *prio, int *pgn, int *src,
                           int *dst, uint8_t *data, int *len)
{
    /* champ 0 = timestamp (éventuellement vide) → sauter jusqu'à la 1re virgule */
    const char *p = strchr(line, ',');
    if (!p)
        return 0;
    p++;

    long v[5];   /* prio, pgn, src, dst, len */
    for (int i = 0; i < 5; i++) {
        char *end;
        v[i] = strtol(p, &end, 10);
        if (end == p)
            return 0;
        if (i < 4) {
            if (*end != ',')
                return 0;
            p = end + 1;
        } else {
            p = end;
        }
    }
    int n = (int)v[4];
    if (n < 0 || n > N2K_MAX_DATA)
        return 0;

    for (int i = 0; i < n; i++) {
        if (*p != ',')
            return 0;
        p++;
        char *end;
        long b = strtol(p, &end, 16);
        if (end == p)
            return 0;
        data[i] = (uint8_t)b;
        p = end;
    }

    *prio = (int)v[0]; *pgn = (int)v[1]; *src = (int)v[2];
    *dst  = (int)v[3]; *len = n;
    return 1;
}

int main(int argc, char **argv)
{
    int port = 2600;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage : %s [--port N]  (lit le format actisense sur stdin)\n", argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    netout_t no;
    if (netout_open(&no, port) != 0) {
        fprintf(stderr, "ydraw-bridge : impossible d'ouvrir le port %d\n", port);
        return 1;
    }
    fprintf(stderr, "ydraw-bridge : YDRAW sur TCP port %d (single-frame + fast-packet)\n", no.port);

    char line[4096];
    unsigned long n_in = 0, n_frames = 0;
    int seq = 0;                         /* compteur de séquence fast-packet (0..7) */
    while (fgets(line, sizeof line, stdin)) {
        netout_accept(&no);
        int prio, pgn, src, dst, len;
        uint8_t data[N2K_MAX_DATA];
        if (!parse_actisense(line, &prio, &pgn, &src, &dst, data, &len))
            continue;
        n_in++;
        uint32_t id = ydraw_encode_id(prio, pgn, src, dst);
        char out[YDRAW_MAX_LEN];
        uint32_t tod = ydraw_tod_ms_now();

        if (len <= 8) {                  /* single-frame : une trame directe */
            int m = ydraw_format(out, sizeof out, id, data, len, tod);
            if (m > 0 && netout_broadcast(&no, out, (size_t)m) > 0)
                n_frames++;
        } else {                         /* fast-packet : re-fragmentation */
            int frames = ydraw_fastpacket_frames(len);
            for (int idx = 0; idx < frames; idx++) {
                int m = ydraw_format_fp(out, sizeof out, id, data, len, idx, seq, tod);
                if (m > 0 && netout_broadcast(&no, out, (size_t)m) > 0)
                    n_frames++;
            }
            seq = (seq + 1) & 0x7;
        }
    }

    netout_close(&no);
    fprintf(stderr, "ydraw-bridge : %lu messages lus, %lu trames YDRAW diffusées.\n", n_in, n_frames);
    return 0;
}
