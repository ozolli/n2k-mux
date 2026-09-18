/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Olivier Zolli */

/*
 * sha256.h — SHA-256 (FIPS 180-4) et HMAC-SHA256 (RFC 2104), sans dépendance.
 *
 * Sert au cookie de session de l'interface web : jeton = HMAC(secret serveur,
 * "user:pass"). n2k-mux-web n'a aucune bibliothèque externe ; une implémentation
 * compacte, vérifiée contre les vecteurs officiels (test_sha256 : FIPS 180-2 et
 * RFC 4231), évite d'en ajouter une pour une seule fonction.
 */

#ifndef N2KMUX_SHA256_H
#define N2KMUX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32

typedef struct {
    uint32_t h[8];
    uint64_t bits;       /* longueur totale traitée, en bits */
    uint8_t  buf[64];
    size_t   n;          /* octets en attente dans buf */
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[SHA256_LEN]);

/* Empreinte d'un bloc en une fois. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_LEN]);

/* HMAC-SHA256(key, msg). */
void hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen,
                 uint8_t out[SHA256_LEN]);

/* Écrit les 32 octets en hexadécimal minuscule (65 octets avec le NUL). */
void sha256_hex(const uint8_t in[SHA256_LEN], char out[2 * SHA256_LEN + 1]);

#endif /* N2KMUX_SHA256_H */
