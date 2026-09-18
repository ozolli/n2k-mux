/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * test_sha256.c — Vérifie SHA-256 et HMAC-SHA256 contre les vecteurs officiels
 * (FIPS 180-2 annexe B, RFC 4231). Sort non nul au premier écart.
 */

#include "sha256.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *what, const uint8_t d[SHA256_LEN], const char *want)
{
    char hex[2 * SHA256_LEN + 1];
    sha256_hex(d, hex);
    if (strcmp(hex, want) != 0) {
        fprintf(stderr, "FAIL %s\n  attendu : %s\n  obtenu  : %s\n", what, want, hex);
        failures++;
    }
}

int main(void)
{
    uint8_t d[SHA256_LEN];

    sha256("abc", 3, d);
    check("SHA256(abc)", d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("", 0, d);
    check("SHA256(vide)", d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   /* 448 bits */
    sha256(m2, strlen(m2), d);
    check("SHA256(448 bits)", d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* Un million de 'a', par morceaux inégaux : exerce la mise en tampon. */
    {
        sha256_t s;
        char a[997];
        memset(a, 'a', sizeof a);
        sha256_init(&s);
        size_t left = 1000000;
        while (left) {
            size_t n = left < sizeof a ? left : sizeof a;
            sha256_update(&s, a, n);
            left -= n;
        }
        sha256_final(&s, d);
        check("SHA256(1e6 x a)", d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /* RFC 4231 — cas 1 : clé 20 × 0x0b, « Hi There ». */
    {
        uint8_t k[20];
        memset(k, 0x0b, sizeof k);
        hmac_sha256(k, sizeof k, "Hi There", 8, d);
        check("HMAC RFC 4231 cas 1", d, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }
    /* cas 2 : clé « Jefe ». */
    {
        const char *msg = "what do ya want for nothing?";
        hmac_sha256("Jefe", 4, msg, strlen(msg), d);
        check("HMAC RFC 4231 cas 2", d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    /* cas 6 : clé de 131 octets (> bloc), réduite à son empreinte. */
    {
        uint8_t k[131];
        memset(k, 0xaa, sizeof k);
        const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
        hmac_sha256(k, sizeof k, msg, strlen(msg), d);
        check("HMAC RFC 4231 cas 6", d, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }

    fprintf(stderr, "\n--- Récapitulatif ---\néchecs : %d\n", failures);
    return failures ? 1 : 0;
}
