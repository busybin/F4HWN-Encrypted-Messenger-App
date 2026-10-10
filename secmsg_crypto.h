/* SECMSG crypto core — shared by the firmware app and the host tests.
 * Speck-128/128 (block=128, key=128, 32 rounds, alpha=8, beta=3) encrypt-only:
 * CTR mode and CBC-MAC both need encryption alone, so no decrypt routine
 * (saves code + 256 B of round-key RAM).
 * Endianness: byte strings map to u64 words big-endian, words in order;
 * the first round key is the LAST key word (K = (l0, k0) per the spec).
 * Verified against the official NSA KAT (see tests/crypto_test.c).
 */
#ifndef SECMSG_CRYPTO_H
#define SECMSG_CRYPTO_H

#include <stdint.h>

#define SPECK_ROUNDS 32u
#define KDF_V1_ROUNDS 16384u

static inline uint64_t ror64(uint64_t x, unsigned r){ return (x >> r) | (x << (64u - r)); }
static inline uint64_t rol64(uint64_t x, unsigned r){ return (x << r) | (x >> (64u - r)); }

static inline uint64_t ld64be(const uint8_t *b){
    uint64_t v = 0;
    for(uint8_t i = 0; i < 8; i++){ v <<= 8; v |= b[i]; }
    return v;
}
static inline void st64be(uint8_t *b, uint64_t v){
    for(int8_t i = 7; i >= 0; i--){ b[i] = (uint8_t)v; v >>= 8; }
}

/* One Speck-128/128 block. k[2] = key words, in[2]/out[2] = block words. */
static void speck128_enc(const uint64_t k[2], const uint64_t in[2], uint64_t out[2]){
    uint64_t y = in[0], x = in[1], b = k[1], a = k[0];
    for(uint32_t i = 0; i < SPECK_ROUNDS; i++){
        x = (ror64(x, 8u) + y) ^ b;
        y = (rol64(y, 3u) ^ x);
        uint64_t an = (ror64(a, 8u) + b) ^ i;
        b = (rol64(b, 3u) ^ an);
        a = an;
    }
    out[0] = y; out[1] = x;
}

/* Legacy v1 helper retained for host-vector tests only. SECMSG v2 uses the
 * application KDF below and never calls this function. */
__attribute__((unused))
static void kdf_pin(const uint8_t *pin, uint8_t pinLen, uint64_t key[2]){
    if(!pin || !pinLen || pinLen > 16u){ key[0] = key[1] = 0; return; }
    uint8_t buf[16];
    for(uint8_t i = 0; i < 16; i++)
        buf[i] = (uint8_t)(pin[i % pinLen] * 31u + i * 0x9Eu + 0x51u);
    key[0] = ld64be(buf); key[1] = ld64be(buf + 8);
    uint64_t st[2];
    for(uint32_t i = 0; i < KDF_V1_ROUNDS; i++){
        speck128_enc(key, key, st);
        key[0] = st[0]; key[1] = st[1];
    }
}

/* CTR keystream block i: E_k(nonce || ctr). nonce[16], ctr = block index. */
__attribute__((unused))
static void ctr_block(const uint64_t k[2], const uint8_t nonce[16], uint32_t ctr, uint8_t out[16]){
    uint8_t inb[16];
    for(uint8_t i = 0; i < 16; i++) inb[i] = nonce[i];
    inb[0] ^= (uint8_t)ctr; inb[1] ^= (uint8_t)(ctr >> 8);
    inb[2] ^= (uint8_t)(ctr >> 16); inb[3] ^= (uint8_t)(ctr >> 24);
    uint64_t in[2], o[2];
    in[0] = ld64be(inb); in[1] = ld64be(inb + 8);
    speck128_enc(k, in, o);
    st64be(out, o[0]); st64be(out + 8, o[1]);
}

/* CBC-MAC (zero IV) over data, first 12 bytes out (SECMSG v2 tag). */
static void cbcmac(const uint64_t k[2], const uint8_t *data, uint8_t len, uint8_t mac[12]){
    if(!data || !mac || !len || len > 240u || (len & 15u)){
        for(uint8_t i = 0; i < 12; i++) mac[i] = 0;
        return;
    }
    uint64_t st[2] = {0, 0}, blk[2], o[2];
    for(uint8_t off = 0; off < len; off += 16){
        blk[0] = st[0] ^ ld64be(data + off);
        blk[1] = st[1] ^ ld64be(data + off + 8);
        speck128_enc(k, blk, o);
        st[0] = o[0]; st[1] = o[1];
    }
    uint8_t tmp[16];
    st64be(tmp, st[0]); st64be(tmp + 8, st[1]);
    for(uint8_t i = 0; i < 12; i++) mac[i] = tmp[i];
}

#endif /* SECMSG_CRYPTO_H */
