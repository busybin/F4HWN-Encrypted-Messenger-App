#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"
#include "host_api.h"

/* Slim-build session semantics (no newSession rotation):
 * - PIN change re-derives the key, counters continue, peers keep working.
 * - ID edit only rewrites the 6 user bytes; ACK matching still works. */
static void set_pin(const char *pin)
{
    memcpy(G.pin, pin, 8);
    G.pinLen = PIN_LEN;
    G.pinDirty = true;
}

static void save_menu(void)
{
    /* Mirror closeMenu() without resident services. */
    if (G.pinLen != PIN_LEN) { puts("bad pin"); return; }
    G.menu = 0;
    if (G.pinDirty) { kdfStart(); G.pinDirty = false; }
}

int main(void)
{
    app_api_t api = {0};
    A = &api; HOST_API_STUBS(api);
    memset(&G, 0, sizeof(G));
    memcpy(G.id.localId, "ALICE", 5);
    G.id.localId[5] = 0;
    G.id.localId[6] = 0;
    G.id.localId[7] = 0x42;
    G.idLen = 5;
    memset(G.id.txNext, 0, 8);
    set_pin("12345678");
    save_menu();
    while (G.kdfLeft) kdfStep();
    uint64_t k1[2] = {G.key[0], G.key[1]};
    uint8_t tx1[8];
    copy8(tx1, G.id.txNext);

    /* PIN change: key changes, identity bytes and counters continue. */
    set_pin("87654321");
    save_menu();
    while (G.kdfLeft) kdfStep();
    if (G.key[0] == k1[0] && G.key[1] == k1[1])
        return 1;
    if (!same8(tx1, G.id.txNext))
        return 2;
    if (memcmp(G.id.localId, "ALICE", 5) != 0 || G.id.localId[7] != 0x42)
        return 3;

    /* ID edit path via the shared multitap helper. */
    G.menu = 2;
    memset(G.id.localId, 0, ID_MAX);
    G.idLen = 0;
    handleTap(APP_KEY_2);
    G.tapMs = TAP_TICKS;
    handleTap(APP_KEY_2);
    if (G.idLen != 1 || G.id.localId[0] != 'B')
        return 4;
    G.menu = 0;
    if (idLenOf(G.id.localId) != 1)
        return 5;

    /* A message built after PIN+ID changes still verifies under the new key. */
    memcpy(G.msg, "HI", 2);
    G.msgLen = 2;
    uint8_t ctr[8];
    copy8(ctr, G.id.txNext);
    if (!inc8(ctr))
        return 6;
    buildPacket(ctr);
    uint8_t wire[72];
    memcpy(wire, pbytes(), sizeof(wire));
    memcpy(pbytes(), wire, sizeof(wire));
    if (parsePacket() != 1 || memcmp(pbytes() + 24, "HI", 2) != 0)
        return 7;

    puts("SECMSG SESSION PASS");
    return 0;
}
