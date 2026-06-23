/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * canfilter.c — Filtre N2K→N2K par frame-passthrough (socketcan).
 *
 * Lit les trames CAN brutes d'une interface d'entrée (déf. can0, le bus réel via
 * le PEAK PCAN-USB) et réémet sur une interface de sortie (déf. vcan0, CAN
 * virtuel) UNIQUEMENT les trames retenues par l'arbitrage. Zéro ré-encodage : on
 * recopie la struct can_frame telle quelle (pas de décodage→ré-encodage canboat).
 *
 * La DÉCISION (qui gagne pour chaque PGN) est calculée ailleurs (n2k-mux, couche
 * canboat), qui publie la liste des PERDANTS dans un fichier — voir incrément 2.
 * Ici, incrément 1 : pur forwarder (miroir) pour valider le chemin can0→vcan0→qtVlm.
 *
 * PGN et adresse source se lisent dans l'ID CAN 29 bits de CHAQUE trame (même les
 * fragments fast-packet), donc le filtrage est trame par trame, sans réassemblage :
 * tous les fragments d'un message portent le même (pgn, src) dans leur ID, qtVlm
 * réassemble en aval.
 *
 * Zéro allocation dynamique. Sockets bloquants (un thread, un flux).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* Ouvre un socket CAN RAW lié à l'interface `ifname`. Retourne le fd ou -1. */
static int can_open(const char *ifname)
{
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        fprintf(stderr, "canfilter : socket(PF_CAN) : %s\n", strerror(errno));
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "canfilter : interface '%s' introuvable : %s\n",
                ifname, strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof addr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "canfilter : bind '%s' : %s\n", ifname, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* PGN extrait de l'ID CAN 29 bits (J1939/N2K). */
static int pgn_of(canid_t id)
{
    unsigned e  = id & CAN_EFF_MASK;       /* 29 bits */
    unsigned dp = (e >> 24) & 1;           /* Data Page */
    unsigned pf = (e >> 16) & 0xff;        /* PDU Format */
    unsigned ps = (e >> 8) & 0xff;         /* PDU Specific */
    return (pf < 240) ? (int)((dp << 16) | (pf << 8))        /* PDU1 (dirigé) */
                      : (int)((dp << 16) | (pf << 8) | ps);  /* PDU2 (diffusion) */
}
static int src_of(canid_t id) { return (int)(id & 0xff); }

/* --- Liste des perdants à jeter (publiée par n2k-mux --losers) ------------- */
#define FDROP_MAX 128
typedef struct { int pgn, src; } fdrop_t;

/* (Re)charge le fichier « pgn src » (lignes # ignorées). Retourne le nombre lu. */
static int drop_load(const char *path, fdrop_t *set, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0;
    char line[128];
    while (n < max && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        int pgn, src;
        if (sscanf(line, "%d %d", &pgn, &src) == 2) {
            set[n].pgn = pgn; set[n].src = src; n++;
        }
    }
    fclose(f);
    return n;
}

static int drop_has(const fdrop_t *set, int n, int pgn, int src)
{
    for (int i = 0; i < n; i++)
        if (set[i].pgn == pgn && set[i].src == src) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *in_if = "can0", *out_if = "vcan0", *drop_path = NULL;
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc)         in_if = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out_if = argv[++i];
        else if (!strcmp(argv[i], "--drop") && i + 1 < argc)  drop_path = argv[++i];
        else if (!strcmp(argv[i], "-v"))                      verbose = 1;
        else {
            fprintf(stderr, "usage : %s [--in IFACE] [--out IFACE] [--drop FICHIER] [-v]\n"
                            "  défauts : --in can0 --out vcan0\n"
                            "  --drop : fichier des perdants (pgn src) publié par n2k-mux --losers\n",
                    argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    int rx = can_open(in_if);
    if (rx < 0) return 1;
    int tx = can_open(out_if);
    if (tx < 0) { close(rx); return 1; }

    /* Liste des perdants (rechargée à chaud quand le fichier change). */
    fdrop_t dset[FDROP_MAX];
    int dn = 0;
    time_t dmtime = 0;
    if (drop_path) {
        dn = drop_load(drop_path, dset, FDROP_MAX);
        struct stat sb;
        if (stat(drop_path, &sb) == 0) dmtime = sb.st_mtime;
    }

    fprintf(stderr, "canfilter : %s → %s%s, Ctrl-C pour arrêter.\n",
            in_if, out_if,
            drop_path ? " (arbitré : drop des perdants)" : " (miroir)");

    unsigned long n_in = 0, n_out = 0, n_drop = 0;
    while (!g_stop) {
        struct can_frame f;
        ssize_t r = read(rx, &f, sizeof f);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "canfilter : read %s : %s\n", in_if, strerror(errno));
            break;
        }
        if (r != (ssize_t)sizeof f) continue;   /* trame CAN FD ignorée ici */
        n_in++;

        /* Recharge la liste des perdants si le fichier a changé (~tous les 256). */
        if (drop_path && (n_in & 0xff) == 0) {
            struct stat sb;
            if (stat(drop_path, &sb) == 0 && sb.st_mtime != dmtime) {
                dmtime = sb.st_mtime;
                dn = drop_load(drop_path, dset, FDROP_MAX);
            }
        }

        /* Arbitrage : on jette les (pgn, src) perdants ; tout le reste passe. */
        if (drop_path && drop_has(dset, dn, pgn_of(f.can_id), src_of(f.can_id))) {
            n_drop++;
            continue;
        }

        ssize_t w = write(tx, &f, sizeof f);
        if (w == (ssize_t)sizeof f) n_out++;

        if (verbose && (n_in % 500) == 0)
            fprintf(stderr, "  %lu lues / %lu réémises / %lu jetées\n",
                    n_in, n_out, n_drop);
    }

    fprintf(stderr, "canfilter : %lu trames lues, %lu réémises, %lu jetées.\n",
            n_in, n_out, n_drop);
    close(rx);
    close(tx);
    return 0;
}
