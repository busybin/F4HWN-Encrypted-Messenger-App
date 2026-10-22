#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"
#include "host_api.h"

/* Input lock: while the key schedule is being derived the input box shows
 * LOADING and every key is swallowed - not just the first press. The lock is a
 * live predicate, re-evaluated on each key, so a burst of distinct digits must
 * all be ignored; only keys after the KDF may land in the compose buffer. */

static uint8_t  cfgblob[CFG_SIZE];
static const uint8_t *script;
static unsigned script_len, script_pos, key_calls;
static uint8_t  saw_loading;
static char     last_row0[20], last_row_in[20];

static void rec_normal(const char *s, uint8_t a, uint8_t b, uint8_t line)
{
    (void)a; (void)b;
    if (line == 0 || line == ROW_IN) {
        char *dst = (line == 0) ? last_row0 : last_row_in;
        size_t n = strlen(s); if (n > 18) n = 18;
        memcpy(dst, s, n); dst[n] = 0;
        if (line == ROW_IN && strcmp(s, "LOADING") == 0) saw_loading = 1;
    }
}
static void rec_bold(const char *s, uint8_t a, uint8_t b, uint8_t line) { rec_normal(s, a, b, line); }
static void rec_inv(const char *s, uint8_t x, uint8_t l, bool a, bool b, uint8_t e)
{ (void)s; (void)x; (void)l; (void)a; (void)b; (void)e; }
static void noop(void) {}
static void noop_b(bool b) { (void)b; }
static void noop_u8(uint8_t u) { (void)u; }
static void noop_u32(uint32_t u) { (void)u; }
static uint8_t fb_mem[7][128];
static void cfg_save_fn(const uint8_t *p, uint8_t n) { if (n == CFG_SIZE) memcpy(cfgblob, p, CFG_SIZE); }
static void cfg_load_fn(uint8_t *p, uint8_t n) { memcpy(p, cfgblob, n); }
static uint8_t tx0(void) { return 0; }
static uint32_t frq(void) { return 14500000u; }
static int8_t nav0(uint8_t k) { (void)k; return 0; }
static void uid0(uint8_t *p) { for (int i = 0; i < 12; i++) p[i] = (uint8_t)(i + 1); }
static uint16_t bk0(uint8_t r) { (void)r; return 0; }
static void bkw(uint8_t r, uint16_t v) { (void)r; (void)v; }
static uint8_t key_fn(void)
{
    key_calls++;
    if (script_pos < script_len) return script[script_pos++];
    return APP_KEY_INVALID;
}

static void setup(app_api_t *api)
{
    memset(api, 0, sizeof(*api));
    api->abi_major = APP_ABI_MAJOR; api->api_level = APP_API_LEVEL; api->api_size = sizeof(*api);
    api->fb = (app_fb_t)fb_mem;
    api->display_clear = noop; api->status_clear = noop;
    api->print_bold = rec_bold; api->print_normal = rec_normal; api->print_inverse = rec_inv;
    api->blit_full = noop; api->blit_status = noop; api->draw_battery = noop;
    api->get_key = key_fn; api->delay_ms = noop_u32; api->led = noop_b;
    api->bk_read = bk0; api->bk_write = bkw;
    api->set_agc = noop_b; api->set_af = noop_u8; api->audio_path = noop_b;
    api->tones_off_rx = noop; api->rx_freq = frq; api->tx_freq = frq;
    api->tx_state = tx0; api->tx_set_params = noop; api->tx_end = noop;
    api->uid_get = uid0; api->cfg_load = cfg_load_fn; api->cfg_save = cfg_save_fn;
    api->nav_dir = nav0;
    api->backlight_on = noop; api->backlight_update = noop; api->battery_sample = noop;
}

int main(void)
{
    app_api_t api = {0};
    A = &api; HOST_API_STUBS(api);

    /* Valid receiver config so app_main loads the PIN and runs the KDF. */
    memset(&G, 0, sizeof(G));
    memcpy(G.pin, "12345678", 8);
    G.pinLen = PIN_LEN;
    memset(G.id.localId, 0, 8);
    for (int i = 0; i < ID_MAX; i++) G.id.localId[i] = (uint8_t)"LOCK01"[i];
    clearPeer();
    setup(&api);
    saveConfig();

    /* Script: a burst of distinct digits while the KDF runs, then enough idle
     * ticks to finish it, then four digits that must be accepted. */
    static uint8_t sc[1400];
    unsigned n = 0;
    for (unsigned r = 0; r < 6; r++)
        for (uint8_t k = 2; k <= 9; k++) { sc[n++] = k; sc[n++] = APP_KEY_INVALID; }
    while (n < 1000u) sc[n++] = APP_KEY_INVALID;
    for (uint8_t k = 1; k <= 4; k++) {
        sc[n++] = k; sc[n++] = APP_KEY_INVALID;
        sc[n++] = APP_KEY_INVALID; sc[n++] = APP_KEY_INVALID;
    }
    /* EXIT needs two presses now: main-loop positions must see E, I, E. */
    sc[n++] = APP_KEY_EXIT; sc[n++] = APP_KEY_INVALID;
    sc[n++] = APP_KEY_EXIT; sc[n++] = APP_KEY_EXIT;
    sc[n++] = APP_KEY_INVALID; sc[n++] = APP_KEY_EXIT;

    memset(&G, 0, sizeof(G));
    setup(&api);
    A = &api; HOST_API_STUBS(api);
    script = sc; script_len = n; script_pos = 0; key_calls = 0;
    app_main(&api);

    /* The locked burst (48 calls, all distinct-adjacent) must not have typed. */
    if (G.msgLen != 4u || memcmp(G.msg, "1ADG", 4) != 0) return 1;
    if (!saw_loading) return 2;      /* the input box showed LOADING during the KDF */
    if (strcmp(last_row0, "LOCK01") != 0) return 10;   /* identity row shows the ID */

    /* The predicate itself: locked while deriving / receiving / awaiting ACK. */
    memset(&G, 0, sizeof(G));
    if (inputLocked()) return 3;
    G.ackWait = 1; if (!inputLocked()) return 4; G.ackWait = 0;
    G.fIdx = 1;     if (!inputLocked()) return 5; G.fIdx = 0;
    G.kdfLeft = 1;  if (!inputLocked()) return 6; G.kdfLeft = 0;
    if (inputLocked()) return 7;

    /* A pre-interval (CFG_VER 2) config must still load its PIN/identity and
     * simply fall back to the default receipt interval. */
    {
        uint8_t *c = cfgblob;
        c[1] = 2u; c[48] = 0xFFu;                 /* no interval field in v2 */
        uint16_t s = crc16(c, CFG_CRC);
        c[CFG_CRC] = (uint8_t)(s >> 8); c[CFG_CRC + 1u] = (uint8_t)s;
        memset(&G, 0, sizeof(G));
        G.ackS = ACK_DEF;
        loadConfig();
        if (G.pinLen != PIN_LEN) return 8;
        if (G.ackS != ACK_DEF) return 9;
    }

    /* EXIT is a two-press action: a lone press must arm only, a stale second
     * press (after the window) must re-arm, and a fresh pair must quit. We
     * detect "quit too early" by how much of the script was consumed. */
    {
        static uint8_t sx[400];
        unsigned m = 0;
        sx[m++] = APP_KEY_EXIT;                                    /* arm */
        for (int i = 0; i < 300; i++) sx[m++] = APP_KEY_INVALID;   /* window lapses (150 ticks) */
        sx[m++] = APP_KEY_EXIT;                                    /* re-arm (stale pair) */
        sx[m++] = APP_KEY_INVALID; sx[m++] = APP_KEY_INVALID;
        sx[m++] = APP_KEY_EXIT;                                    /* quit */
        unsigned tail = m;
        while (m < sizeof(sx)) sx[m++] = APP_KEY_INVALID;

        memset(&G, 0, sizeof(G));
        G.ackS = ACK_DEF;
        loadConfig();
        setup(&api);
        A = &api; HOST_API_STUBS(api);
        script = sx; script_len = tail; script_pos = 0; key_calls = 0;
        app_main(&api);
        if (script_pos < 250u) return 11;   /* would have quit on the first press */
        if (G.flags & F_RUN) return 12;
    }

    puts("SECMSG LOCK PASS");
    return 0;
}
