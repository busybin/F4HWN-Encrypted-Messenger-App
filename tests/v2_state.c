#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"
#include "host_api.h"

int main(void)
{
    app_api_t api = {0};
    A = &api; HOST_API_STUBS(api);
    memset(&G, 0, sizeof(G));

    /* Keypad/multitap and length-delimited edit buffer. */
    handleTap(APP_KEY_2);
    G.tapMs = TAP_TICKS;
    handleTap(APP_KEY_2);
    if (G.msgLen != 1 || G.msg[0] != 'B')
        return 1;
    handleTap(APP_KEY_STAR);
    if (G.msgLen != 2 || G.msg[1] != ' ')
        return 2;
    handleTap(APP_KEY_0);
    if (G.msgLen != 1)
        return 3;

    /* History keeps five full-length entries with receipt/ID prefixes. */
    histNew(false);
    histApp("OK");
    if (G.hn != 1 || G.hist[0].len != 4 || G.hist[0].text[0] != 'O' || G.hist[0].text[3] != 'K')
        return 4;
    histNew(true);
    histSetId((const uint8_t *)"ABC123");
    histApp("RX");
    if (G.hn != 2 || !(G.hist[1].meta & H_RX) || G.hist[1].len != 9 ||
        memcmp(G.hist[1].text, "ABC123:RX", 9) != 0)
        return 5;
    for (uint8_t i = 0; i < HIST_N; i++) {
        histNew((i & 1u) != 0);
        histApp("12345678901234567890123456789012");
    }
    if (G.hn != HIST_N)
        return 6;

    /* Exercise the byte-safe 8-byte helpers used by the identity/replay state. */
    uint8_t a[8] __attribute__((aligned(4))) = {0};
    uint8_t b[8] __attribute__((aligned(4))) = {0};
    a[0] = 1;
    if (zero8(a) || !zero8(b))
        return 7;
    copy8(b, a);
    if (!same8(a, b))
        return 8;

    /* Decimal display/PIN unpacking must terminate on non-multiples of ten. */
    char n[12];
    pz(n, 0u);
    if (strcmp(n, "0") != 0)
        return 9;
    pz(n, 32u);
    if (strcmp(n, "32") != 0)
        return 9;
    pz(n, 10032u);
    if (strcmp(n, "10032") != 0)
        return 9;
    uint32_t pin = 12345678u;
    unpackPin(pin);
    if (memcmp(G.pin, "12345678", 8) != 0)
        return 10;

    puts("SECMSG STATE PASS");
    return 0;
}
