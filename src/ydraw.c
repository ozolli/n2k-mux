/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * ydraw.c — Implémentation du formateur YDRAW + (dé)codage de l'ID CAN 29 bits.
 */

#include "ydraw.h"

#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#define CAN_EFF_MASK 0x1FFFFFFFu   /* 29 bits utiles de l'identifiant étendu */

uint32_t ydraw_encode_id(int prio, int pgn, int src, int dst)
{
    unsigned pf = (unsigned)(pgn >> 8) & 0xFF;          /* PDU Format */
    unsigned ps = (pf < 240) ? (unsigned)(dst & 0xFF)   /* PDU1 : destination */
                             : (unsigned)(pgn & 0xFF);  /* PDU2 : group extension */
    uint32_t id = ((uint32_t)(prio & 0x7)      << 26)   /* priorité (3 bits) */
                | ((uint32_t)((pgn >> 16) & 0x3) << 24) /* EDP + DP */
                | (pf << 16)
                | (ps << 8)
                | (uint32_t)(src & 0xFF);
    return id & CAN_EFF_MASK;
}

void ydraw_decode_id(uint32_t can_id, int *prio, int *pgn, int *src, int *dst)
{
    can_id &= CAN_EFF_MASK;
    unsigned pf = (can_id >> 16) & 0xFF;
    unsigned ps = (can_id >> 8) & 0xFF;
    unsigned dp = (can_id >> 24) & 0x3;            /* EDP + DP */
    int p = (int)((can_id >> 26) & 0x7);
    int s = (int)(can_id & 0xFF);
    int g, d;
    if (pf < 240) {                                 /* PDU1 : adressé */
        d = (int)ps;
        g = (int)((dp << 16) | (pf << 8));          /* PS hors PGN */
    } else {                                        /* PDU2 : broadcast */
        d = 255;
        g = (int)((dp << 16) | (pf << 8) | ps);
    }
    if (prio) *prio = p;
    if (pgn)  *pgn  = g;
    if (src)  *src  = s;
    if (dst)  *dst  = d;
}

int ydraw_format(char *buf, size_t sz, uint32_t can_id,
                 const uint8_t *data, int len, uint32_t tod_ms)
{
    if (len < 0)
        len = 0;
    unsigned ms = tod_ms % 1000u;
    unsigned s  = (tod_ms / 1000u) % 60u;
    unsigned m  = (tod_ms / 60000u) % 60u;
    unsigned h  = (tod_ms / 3600000u) % 24u;

    int n = snprintf(buf, sz, "%02u:%02u:%02u.%03u R %08X",
                     h, m, s, ms, (unsigned)(can_id & CAN_EFF_MASK));
    if (n < 0 || (size_t)n >= sz)
        return -1;
    size_t pos = (size_t)n;

    for (int i = 0; i < len; i++) {
        int w = snprintf(buf + pos, sz - pos, " %02X", data[i]);
        if (w < 0 || (size_t)w >= sz - pos)
            return -1;
        pos += (size_t)w;
    }

    int w = snprintf(buf + pos, sz - pos, "\r\n");
    if (w < 0 || (size_t)w >= sz - pos)
        return -1;
    pos += (size_t)w;
    return (int)pos;
}

uint32_t ydraw_tod_ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    return (uint32_t)((tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec) * 1000
                      + tv.tv_usec / 1000);
}

int ydraw_fastpacket_frames(int total_len)
{
    if (total_len <= 6)
        return 1;
    return 1 + (total_len - 6 + 6) / 7;   /* 1 + ceil((total_len − 6) / 7) */
}

int ydraw_format_fp(char *buf, size_t sz, uint32_t can_id,
                    const uint8_t *payload, int total_len,
                    int idx, int seq, uint32_t tod_ms)
{
    uint8_t f[8];
    for (int i = 0; i < 8; i++)
        f[i] = 0xFF;                       /* remplissage par défaut */

    if (idx == 0) {
        f[0] = (uint8_t)((seq & 0x7) << 5);            /* séquence | trame 0 */
        f[1] = (uint8_t)total_len;
        int n = total_len < 6 ? total_len : 6;
        for (int i = 0; i < n; i++)
            f[2 + i] = payload[i];
    } else {
        f[0] = (uint8_t)(((seq & 0x7) << 5) | (idx & 0x1F));
        int off = 6 + (idx - 1) * 7;
        for (int i = 0; i < 7 && off + i < total_len; i++)
            f[1 + i] = payload[off + i];
    }
    return ydraw_format(buf, sz, can_id, f, 8, tod_ms);
}
