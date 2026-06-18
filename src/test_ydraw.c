/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_ydraw.c — Vérifie l'encodage/décodage de l'ID CAN 29 bits et le format
 * de ligne YDRAW (Yacht Devices RAW text).
 */

#include "ydraw.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void ok(const char *what, int cond)
{
    if (!cond) { fprintf(stderr, "FAIL %s\n", what); failures++; }
}

int main(void)
{
    /* --- PGN PDU2 (broadcast) : 129025 Position Rapid Update --- */
    /* PF=0xF8=248 ≥240 → PDU2 ; prio 2, src 3 → ID attendu 0x09F80103 */
    uint32_t id = ydraw_encode_id(2, 129025, 3, 255);
    ok("129025 → ID 0x09F80103", id == 0x09F80103u);
    int prio, pgn, src, dst;
    ydraw_decode_id(id, &prio, &pgn, &src, &dst);
    ok("PDU2 round-trip pgn", pgn == 129025);
    ok("PDU2 round-trip src", src == 3);
    ok("PDU2 round-trip prio", prio == 2);
    ok("PDU2 dst broadcast", dst == 255);

    /* --- PGN PDU1 (adressé) : 59904 ISO Request --- */
    /* PF=0xEA=234 <240 → PDU1 ; prio 6, src 0, dst 255 → ID 0x18EAFF00 */
    id = ydraw_encode_id(6, 59904, 0, 255);
    ok("59904 → ID 0x18EAFF00", id == 0x18EAFF00u);
    ydraw_decode_id(id, &prio, &pgn, &src, &dst);
    ok("PDU1 round-trip pgn", pgn == 59904);
    ok("PDU1 round-trip src", src == 0);
    ok("PDU1 round-trip dst", dst == 255);

    /* --- PGN PDU1 adressé à une destination précise (dst=37) --- */
    id = ydraw_encode_id(6, 59904, 12, 37);
    ydraw_decode_id(id, &prio, &pgn, &src, &dst);
    ok("PDU1 dst spécifique", dst == 37 && src == 12 && pgn == 59904);

    /* --- format d'une ligne YDRAW (horodatage déterministe) --- */
    /* tod = 12:34:56.789 → (12*3600+34*60+56)*1000+789 = 45296789 ms */
    uint8_t data[8] = { 0x00, 0xFF, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0xFF };
    char buf[YDRAW_MAX_LEN];
    int n = ydraw_format(buf, sizeof buf, 0x09F80103u, data, 8, 45296789u);
    ok("format len > 0", n > 0);
    ok("ligne YDRAW exacte",
       strcmp(buf, "12:34:56.789 R 09F80103 00 FF FF 7F FF 7F FF FF\r\n") == 0);
    if (n > 0)
        ok("longueur retournée cohérente", (size_t)n == strlen(buf));

    /* --- trame courte (single-frame de 3 octets) --- */
    uint8_t d3[3] = { 0x01, 0x02, 0x03 };
    n = ydraw_format(buf, sizeof buf, 0x09F80103u, d3, 3, 0u);
    ok("trame 3 octets",
       n > 0 && strcmp(buf, "00:00:00.000 R 09F80103 01 02 03\r\n") == 0);

    /* --- dépassement de buffer signalé --- */
    char small[16];
    ok("dépassement → -1", ydraw_format(small, sizeof small, 0x09F80103u, data, 8, 0u) == -1);

    /* --- pointeurs NULL acceptés au décodage --- */
    ydraw_decode_id(0x09F80103u, NULL, &pgn, NULL, NULL);
    ok("décodage avec NULL", pgn == 129025);

    /* --- fast-packet : nombre de trames --- */
    ok("fp frames(6) = 1",   ydraw_fastpacket_frames(6) == 1);
    ok("fp frames(8) = 2",   ydraw_fastpacket_frames(8) == 2);   /* 6 + 2 */
    ok("fp frames(13) = 2",  ydraw_fastpacket_frames(13) == 2);  /* 6 + 7 */
    ok("fp frames(14) = 3",  ydraw_fastpacket_frames(14) == 3);  /* 6 + 7 + 1 */
    ok("fp frames(28) = 5",  ydraw_fastpacket_frames(28) == 5);  /* AIS 129038 : 6+7+7+7+1 */
    ok("fp frames(223) = 32", ydraw_fastpacket_frames(223) == 32);

    /* --- fast-packet : contenu des trames (message de 10 octets, seq=1) --- */
    /* payload 01..0A → trame 0 : [20][0A][01..06] ; trame 1 : [21][07..0A][FF FF FF] */
    uint8_t fp[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    n = ydraw_format_fp(buf, sizeof buf, 0x09F80103u, fp, 10, 0, 1, 0u);
    ok("fp trame 0",
       n > 0 && strcmp(buf, "00:00:00.000 R 09F80103 20 0A 01 02 03 04 05 06\r\n") == 0);
    n = ydraw_format_fp(buf, sizeof buf, 0x09F80103u, fp, 10, 1, 1, 0u);
    ok("fp trame 1 (complétée 0xFF)",
       n > 0 && strcmp(buf, "00:00:00.000 R 09F80103 21 07 08 09 0A FF FF FF\r\n") == 0);

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
