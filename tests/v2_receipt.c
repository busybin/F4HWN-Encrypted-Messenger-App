#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"

static void set_local(const char *id, uint8_t suffix)
{
    memset(G.id.localId, 0, 8);
    for (uint8_t i = 0; i < ID_MAX && id[i]; i++) G.id.localId[i] = (uint8_t)id[i];
    G.id.localId[6] = 0;
    G.id.localId[7] = suffix;
    G.idLen = idLenOf(G.id.localId);
}

int main(void)
{
    app_api_t api = {0};
    A = &api;
    memset(&G, 0, sizeof(G));
    memcpy(G.pin, "12345678", 8);
    G.pinLen = PIN_LEN;
    set_local("ABC123", 0x34);
    kdfStart();
    while (G.kdfLeft) kdfStep();

    uint8_t counter[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    /* Long message: the ID/counter at p[8..23] must survive decryption, else
     * the echoed ACK refers to garbage and the sender never sees a receipt. */
    static const char body[] = "HELLO-WORLD-12345!";
    uint8_t blen = (uint8_t)(sizeof(body) - 1);
    memcpy(G.msg, body, blen);
    G.msgLen = blen;
    buildPacket(counter);
    uint8_t normal[72];
    memcpy(normal, pbytes(), sizeof(normal));

    uint64_t base_key[2] = {G.key[0], G.key[1]};
    uint8_t sender_id[8];
    memcpy(sender_id, G.id.localId, sizeof(sender_id));
    set_local("RECV99", 0x78);
    G.key[0] = base_key[0]; G.key[1] = base_key[1];
    clearPeer();
    memcpy(pbytes(), normal, sizeof(normal));
    if (parsePacket() != 1 || pbytes()[24] != 'H' || pbytes()[24 + blen - 1] != '!')
        return 1;
    if (memcmp(pbytes() + 8, sender_id, 6) != 0 || pbytes()[8 + 7] != 0x34)
        return 9;   /* sender ID must still be readable for the display */
    if (!buildAck())
        return 2;
    uint8_t receipt[72];
    memcpy(receipt, pbytes(), sizeof(receipt));
    if ((receipt[7] & PK_KIND_MASK) != PK_KIND_ACK || (receipt[7] & 0x3Fu) != 16u)
        return 3;

    set_local("ABC123", 0x34);
    G.key[0] = base_key[0]; G.key[1] = base_key[1];
    clearPeer();
    /* Mirror sendMessage: txNext is left holding the counter just transmitted. */
    memcpy(G.id.txNext, counter, sizeof(counter));
    G.ackPending = 1;
    G.ackT = 100;
    memcpy(pbytes(), receipt, sizeof(receipt));
    int rr = parsePacket();
    if (rr != 2)
        return 4;
    G.ackPending = 0;
    if (parsePacket() != 0)
        return 5;

    memset(&G.hist, 0, sizeof(G.hist));
    G.hn = 0;
    histNew(true);
    histSetId(sender_id);
    histApp("OK");
    if (G.hist[0].len != 9u || memcmp(G.hist[0].text, "ABC123:OK", 9) != 0)
        return 6;

    memset(&G.hist, 0, sizeof(G.hist));
    G.hn = 0;
    histNew(false);
    histApp("SENT");
    if (G.hist[0].text[0] != 'O') return 7;
    histAck('V');
    if (G.hist[0].text[0] != 'V') return 8;

    puts("SECMSG RECEIPT/ID PASS");
    return 0;
}
