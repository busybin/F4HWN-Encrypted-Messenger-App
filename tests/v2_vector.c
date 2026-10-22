#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"
#include "host_api.h"

static int hexeq(const uint8_t *p, const char *s, int n){
    for(int i=0;i<n;i++){ unsigned v; if(sscanf(s+2*i,"%2x",&v)!=1 || p[i]!=(uint8_t)v) return 0; }
    return 1;
}
int main(void){
    app_api_t api={0}; A=&api; memset(&G,0,sizeof(G));
    memcpy(G.pin,"12345678",8); G.pinLen=8;
    const uint8_t id[8]={1,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
    memcpy(G.id.localId,id,8); kdfStart(); while(G.kdfLeft) kdfStep();
    uint8_t out[32]; st64be(out,G.key[0]); st64be(out+8,G.key[1]);
    if(!hexeq(out,"1ecc210831c3ca3bfafa3ca6285f734c",16)) return 1;
    uint64_t enc[2], mac[2]; deriveSession(id,enc,mac);
    st64be(out,enc[0]); st64be(out+8,enc[1]);
    if(!hexeq(out,"fe7f857b11bed10af04160481602c88c",16)) return 2;
    st64be(out,mac[0]); st64be(out+8,mac[1]);
    if(!hexeq(out,"f910bd7be96679213aefc586e840e3ae",16)) return 3;
    memset(G.id.txNext,0,8); G.id.txNext[7]=1; memcpy(G.msg,"HELLO-2",8); G.msgLen=7;
    buildPacket(G.id.txNext);
    uint8_t wire[72]; memcpy(wire,pbytes(),72);
    /* First MSG_MAX payload bytes are CTR-encrypted; the keystream is
     * independent of MSG_MAX, so bytes 0..17 match the original vector. */
    if(!hexeq(wire+24,"c106719ffbce9cceabb742dbfa6c52564266",18)) return 4;
    for(uint8_t i = MSG_MAX; i < 32u; i++) if(wire[24 + i] != 0) return 5; /* tail stays clear */
    if(!hexeq(wire+56,"0972656b722e38c057a9a844",12)) return 6;
    if(wire[68]!=0x62 || wire[69]!=0x6D) return 11;
    const uint8_t other[8]={0xDE,0xAD,0xBE,0xEF,0,0,0,1}; memcpy(G.id.localId,other,8);
    if(parsePacket()!=1 || strcmp((char *)pbytes()+24,"HELLO-2")!=0) return 7;
    memcpy(pbytes(),wire,72); if(parsePacket()!=0) return 8; /* exact replay */
    memcpy(G.id.localId,id,8);
    G.id.txNext[7]=2; buildPacket(G.id.txNext); if(parsePacket()!=1) return 9;
    memcpy(pbytes(),wire,72); if(parsePacket()!=0) return 10; /* old counter */
    puts("SECMSG V2 VECTOR PASS");
    return 0;
}
