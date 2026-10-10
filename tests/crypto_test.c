// Host verification for secmsg_crypto.h:
// 1) NSA Speck-128/128 KAT  2) CTR roundtrip  3) MAC verify/tamper
// 4) KDF determinism + PIN sensitivity + avalanche sample
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../secmsg_crypto.h"

static int hex(const char *s, uint8_t *b, int n){
    for(int i = 0; i < n; i++){
        unsigned v; if(sscanf(s + 2*i, "%2x", &v) != 1) return -1;
        b[i] = (uint8_t)v;
    }
    return 0;
}
static int fails = 0;
#define CHECK(c, msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

int main(void){
    uint8_t kb[16], pb[16], ct[16], back[40], ks[16];
    uint64_t k[2], in[2], out[2];

    // 1) NSA KAT
    hex("0f0e0d0c0b0a09080706050403020100", kb, 16);
    hex("7469206564616d206c61766975716520", pb, 16);
    k[0] = ld64be(kb); k[1] = ld64be(kb + 8);
    in[0] = ld64be(pb); in[1] = ld64be(pb + 8);
    speck128_enc(k, in, out);
    st64be(ct, out[0]); st64be(ct + 8, out[1]);
    uint8_t want[16];
    hex("7860fedf5c570d18a65d985179783265", want, 16);
    CHECK(memcmp(ct, want, 16) == 0, "NSA KAT mismatch");
    printf("KAT ct = ");
    for(int i = 0; i < 16; i++) printf("%02x", ct[i]);
    printf(" (want 7860fedf5c570d18a65d985179783265)\n");

    // 2) CTR roundtrip over 40 bytes
    uint8_t nonce[16], msg[40], enc[40];
    for(int i = 0; i < 16; i++) nonce[i] = (uint8_t)(i * 17 + 3);
    for(int i = 0; i < 40; i++) msg[i] = (uint8_t)('A' + (i % 26));
    for(int b = 0; b < 3; b++){
        ctr_block(k, nonce, (uint32_t)b, ks);
        for(int i = 0; i < 16 && b * 16 + i < 40; i++)
            enc[b * 16 + i] = msg[b * 16 + i] ^ ks[i];
    }
    CHECK(memcmp(enc, msg, 40) != 0, "CTR produced identity (suspicious)");
    for(int b = 0; b < 3; b++){  // decrypt = same operation
        ctr_block(k, nonce, (uint32_t)b, ks);
        for(int i = 0; i < 16 && b * 16 + i < 40; i++)
            back[b * 16 + i] = enc[b * 16 + i] ^ ks[i];
    }
    CHECK(memcmp(back, msg, 40) == 0, "CTR roundtrip failed");

    // 3) MAC: verify ok, tamper fails
    uint8_t mac[12], mac2[12];
    cbcmac(k, enc, 32, mac);   // MAC over 32-byte (2-block) region
    cbcmac(k, enc, 32, mac2);
    CHECK(memcmp(mac, mac2, 12) == 0, "MAC not deterministic");
    enc[5] ^= 1;               // tamper one bit
    cbcmac(k, enc, 32, mac2);
    CHECK(memcmp(mac, mac2, 12) != 0, "MAC missed tampering");

    // 4) KDF: deterministic, PIN-sensitive, distinct PINs differ
    uint64_t k1[2], k2[2], k3[2];
    kdf_pin((const uint8_t *)"12345678", 8, k1);
    kdf_pin((const uint8_t *)"12345678", 8, k2);
    kdf_pin((const uint8_t *)"12345679", 8, k3);
    CHECK(k1[0] == k2[0] && k1[1] == k2[1], "KDF not deterministic");
    CHECK(k1[0] != k3[0] || k1[1] != k3[1], "KDF insensitive to PIN change");
    // avalanche sample: bits differing between k1/k3
    uint64_t d = (k1[0] ^ k3[0]) | (k1[1] ^ k3[1]);
    int bits = 0; while(d){ bits += (int)(d & 1u); d >>= 1; }
    printf("legacy KDF avalanche bits (1-digit PIN change): %d/128\n", bits);
    CHECK(bits > 40, "KDF avalanche too weak");

    if(!fails) printf("ALL CRYPTO TESTS PASS\n");
    return fails != 0;
}
