/* Kernel crypto exports (Xc*): SHA-1, HMAC-SHA1 and RC4, as titles use them to sign and check save data.
 * SHA-1 state lives in the caller's context buffer (XcSHAInit's context is 0x74 bytes; we use 0x5C). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox.h"

typedef struct {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
} Sha1;

static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
        uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s) {
    static const uint32_t h0[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, h0, sizeof h0);
    s->bytes = 0;
}

static void sha1_update(Sha1 *s, const uint8_t *p, size_t n) {
    while (n) {
        size_t used = s->bytes & 63, take = 64 - used < n ? 64 - used : n;
        memcpy(s->block + used, p, take);
        s->bytes += take;
        p += take;
        n -= take;
        if (!(s->bytes & 63)) sha1_block(s, s->block);
    }
}

static void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha1_update(s, &pad, 1);
    while ((s->bytes & 63) != 56) sha1_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++)
        out[4 * i] = (uint8_t)(s->h[i] >> 24), out[4 * i + 1] = (uint8_t)(s->h[i] >> 16),
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8), out[4 * i + 3] = (uint8_t)s->h[i];
}

static uint32_t k_XcSHAInit(X86Ctx *c, const uint32_t *a) {
    sha1_init((Sha1 *)(g_mem + a[0]));
    return 0;
}
static uint32_t k_XcSHAUpdate(X86Ctx *c, const uint32_t *a) {
    sha1_update((Sha1 *)(g_mem + a[0]), g_mem + a[1], a[2]); /* (Context, Input, InputLength) */
    return 0;
}
static uint32_t k_XcSHAFinal(X86Ctx *c, const uint32_t *a) {
    sha1_final((Sha1 *)(g_mem + a[0]), g_mem + a[1]); /* (Context, Digest[20]) */
    return 0;
}

/* XcHMAC(Key, KeyLength, Data, DataLength, Data2, Data2Length, Digest[20]): HMAC-SHA1 over Data || Data2 */
static uint32_t k_XcHMAC(X86Ctx *c, const uint32_t *a) {
    uint8_t key[64] = {0}, pad[64], inner[20];
    if (a[1] > 64) {
        Sha1 s;
        sha1_init(&s);
        sha1_update(&s, g_mem + a[0], a[1]);
        sha1_final(&s, key);
    } else {
        memcpy(key, g_mem + a[0], a[1]);
    }
    Sha1 s;
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    if (a[2]) sha1_update(&s, g_mem + a[2], a[3]);
    if (a[4]) sha1_update(&s, g_mem + a[4], a[5]);
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5C;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, g_mem + a[6]);
    return 0;
}

/* RC4: XcRC4Key(State[258], KeyLength, Key), XcRC4Crypt(State, Length, Data) */
static uint32_t k_XcRC4Key(X86Ctx *c, const uint32_t *a) {
    uint8_t *st = g_mem + a[0], *key = g_mem + a[2];
    for (int i = 0; i < 256; i++) st[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + st[i] + key[i % a[1]]) & 255;
        uint8_t t = st[i]; st[i] = st[j]; st[j] = t;
    }
    st[256] = st[257] = 0;
    return 0;
}
static uint32_t k_XcRC4Crypt(X86Ctx *c, const uint32_t *a) {
    uint8_t *st = g_mem + a[0], *p = g_mem + a[2], i = st[256], j = st[257];
    for (uint32_t n = 0; n < a[1]; n++) {
        i++;
        j += st[i];
        uint8_t t = st[i]; st[i] = st[j]; st[j] = t;
        p[n] ^= st[(uint8_t)(st[i] + st[j])];
    }
    st[256] = i;
    st[257] = j;
    return 0;
}

/* XcDESKeyParity(Key, KeyLength): give every key byte odd parity (bit 0 is the parity bit) */
static uint32_t k_XcDESKeyParity(X86Ctx *c, const uint32_t *a) {
    for (uint32_t i = 0; i < a[1]; i++) {
        uint8_t b = M8(a[0] + i) & 0xFE, ones = 0;
        for (int k = 1; k < 8; k++) ones += (b >> k) & 1;
        M8(a[0] + i) = b | !(ones & 1);
    }
    return 0;
}

/* shortcut: DES/3DES only protects Xbox Live traffic, and there is no network here; XcKeyTable keeps the raw key
 * and XcBlockCryptCBC passes data through unchanged. Implement real (3)DES if networking is ever wanted. */
static uint32_t k_XcKeyTable(X86Ctx *c, const uint32_t *a) {
    /* (CipherSelect, KeyTable, Key) */
    memcpy(g_mem + a[1], g_mem + a[2], a[0] ? 24 : 8);
    return 0;
}
static uint32_t k_XcBlockCryptCBC(X86Ctx *c, const uint32_t *a) {
    /* (CipherSelect, InputLength, Input, Output, KeyTable, Operation, Feedback) */
    if (a[2] != a[3]) memmove(g_mem + a[3], g_mem + a[2], a[1]);
    return 0;
}

/* known-answer check (FIPS 180 "abc"), run once at startup */
void crypto_selftest(void) {
    static const uint8_t want[20] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E,
                                     0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};
    Sha1 s;
    uint8_t got[20];
    sha1_init(&s);
    sha1_update(&s, (const uint8_t *)"abc", 3);
    sha1_final(&s, got);
    if (memcmp(got, want, 20)) {
        fprintf(stderr, "crypto: SHA-1 self-test failed\n");
        exit(8);
    }
}

#define K(name) {#name, k_##name}
const KImpl g_crypto_impls[] = {
    K(XcSHAInit), K(XcSHAUpdate), K(XcSHAFinal), K(XcHMAC), K(XcRC4Key), K(XcRC4Crypt), K(XcDESKeyParity),
    K(XcKeyTable), K(XcBlockCryptCBC),
    {NULL, NULL},
};
