#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"
#include "host_api.h"

/* End-to-end receive -> receipt through the real app_main loop with a mocked
 * BK4819: the FIFO hands over a captured 72-byte frame, and we assert that
 *  (1) the ACK is really transmitted (36 FIFO writes), not immediately but
 *      about ACK_WAIT ticks later (the sender is still keyed at RXFIN), and
 *  (2) the AF is opened for the burst but muted again afterwards (no hiss). */

static uint8_t  cfgblob[CFG_SIZE];
static uint16_t script[PK_WORDS];
static uint16_t recv_ack[PK_WORDS];
static uint8_t  capture_ack;
static unsigned ack_words;
static unsigned key_calls, r5f_reads, r5f_writes;
static unsigned rx_done_keys, ack_start_keys;
static int      irq_left;        /* IRQ-flag reads still reporting "pending" */
static unsigned irq_src;         /* R02 reads: 1..8 FIFO, 9 = RX_FINISHED   */
static uint8_t  rx_live;         /* scripted reception in progress          */
static uint8_t  crc_bad;         /* R0B reports a chip CRC/format error      */
static unsigned af_fm, af_mute;

static void noop(void) {}
static void noop_b(bool b) { (void)b; }
static void noop_u32(uint32_t u) { (void)u; }
static void print3(const char *s, uint8_t a, uint8_t b, uint8_t c) { (void)s; (void)a; (void)b; (void)c; }
static void print6(const char *s, uint8_t a, uint8_t b, bool c, bool d, uint8_t e)
{ (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; }
static uint8_t fb_mem[7][128];

static void cfg_save_fn(const uint8_t *p, uint8_t n) { if (n == CFG_SIZE) memcpy(cfgblob, p, CFG_SIZE); }
static void cfg_load_fn(uint8_t *p, uint8_t n) { memcpy(p, cfgblob, n); }
static uint8_t txstate_fn(void) { return 0; }
static uint32_t freq_fn(void) { return 14500000u; }
static int8_t nav_fn(uint8_t k) { (void)k; return 0; }
static void uid_fn(uint8_t *p) { for (int i = 0; i < 12; i++) p[i] = (uint8_t)(i + 1); }
static void af_fn(uint8_t m) { if (m == APP_AF_FM) af_fm++; else if (m == APP_AF_MUTE) af_mute++; }

static uint16_t bk_read_fn(uint8_t r)
{
    if (r == 0x0Cu) {                       /* interrupt-flag register */
        if (!rx_live) return 0;
        if (irq_left > 0) { irq_left--; return 1; }
        return 0;
    }
    if (r == 0x02u) {                       /* interrupt source */
        return (++irq_src >= 9u) ? F_RXFIN : F_FFULL;
    }
    if (r == 0x5Fu) {                       /* FSK FIFO word */
        uint16_t w = script[r5f_reads++];
        if (r5f_reads == PK_WORDS) { rx_done_keys = key_calls; rx_live = 0; }
        return w;
    }
    if (r == 0x0Bu) return crc_bad ? 0x0010u : 0u;   /* status: CRC error bit */
    return 0;                               /* R30 etc. */
}

static void bk_write_fn(uint8_t r, uint16_t v)
{
    if (r == 0x5Fu) {
        if (rx_done_keys && !ack_start_keys) ack_start_keys = key_calls;
        r5f_writes++;
        if (capture_ack && ack_words < PK_WORDS) recv_ack[ack_words++] = v;
    }
}

static uint8_t key_fn(void)
{
    key_calls++;
    if (!rx_live && !rx_done_keys && G.kdfLeft == 0) {  /* key schedule ready: play the frame */
        rx_live = 1;
        irq_left = 9;
    }
    /* EXIT now needs two presses: E, I, I, E. */
    if (key_calls > 2000) return (key_calls % 3u == 1u) ? APP_KEY_EXIT : APP_KEY_INVALID;
    return APP_KEY_INVALID;
}

static void setup(app_api_t *api)
{
    memset(api, 0, sizeof(*api));
    api->abi_major = APP_ABI_MAJOR; api->api_level = APP_API_LEVEL; api->api_size = sizeof(*api);
    api->fb = (app_fb_t)fb_mem;
    api->display_clear = noop; api->status_clear = noop;
    api->print_bold = print3; api->print_normal = print3; api->print_inverse = print6;
    api->blit_full = noop; api->blit_status = noop; api->draw_battery = noop;
    api->get_key = key_fn; api->delay_ms = noop_u32; api->led = noop_b;
    api->bk_read = bk_read_fn; api->bk_write = bk_write_fn;
    api->set_agc = noop_b; api->set_af = af_fn; api->audio_path = noop_b;
    api->tones_off_rx = noop; api->rx_freq = freq_fn; api->tx_freq = freq_fn;
    api->tx_state = txstate_fn; api->tx_set_params = noop; api->tx_end = noop;
    api->uid_get = uid_fn;
    api->cfg_load = cfg_load_fn; api->cfg_save = cfg_save_fn;
    api->nav_dir = nav_fn;
    api->backlight_on = noop; api->backlight_update = noop; api->battery_sample = noop;
}

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
    A = &api; HOST_API_STUBS(api);

    /* 1. Receiver config blob (valid magic+CRC so app_main loads the PIN). */
    memset(&G, 0, sizeof(G));
    memcpy(G.pin, "12345678", 8);
    G.pinLen = PIN_LEN;
    set_local("RECV99", 0x78);
    clearPeer();
    setup(&api);
    saveConfig();

    /* 2. Sender: derive the same PIN key and capture one wire frame. */
    memset(&G, 0, sizeof(G));
    memcpy(G.pin, "12345678", 8);
    G.pinLen = PIN_LEN;
    set_local("SEND99", 0x34);
    clearPeer();
    kdfStart();
    while (G.kdfLeft) kdfStep();
    uint8_t counter[8] = {1, 0, 0, 0, 0, 0, 0, 0};   /* matches inc8(0) in sendMessage */
    memcpy(G.msg, "HI", 2);
    G.msgLen = 2;
    buildPacket(counter);
    memcpy(script, G.pkt.word, sizeof(script));

    /* 3. Fresh receiver state; app_main loads the blob and runs. The ACK it
     *    transmits is captured out of the FIFO writes. */
    memset(&G, 0, sizeof(G));
    setup(&api);
    A = &api; HOST_API_STUBS(api);
    capture_ack = 1;
    app_main(&api);
    capture_ack = 0;

    if (G.hn != 1u) return 1;
    if (G.hist[0].len != 9u || memcmp(G.hist[0].text, "SEND99:HI", 9) != 0) return 2;
    if (r5f_writes != PK_WORDS) return 3;              /* ACK really went out */
    if (!rx_done_keys || !ack_start_keys) return 4;
    unsigned gap = ack_start_keys - rx_done_keys;
    if (gap < 280u || gap > 330u) return 5;            /* ~ACK_WAIT ticks, not instant */
    if (af_fm == 0u || af_mute == 0u) return 6;        /* burst audible, then silent */
    if (ack_words != PK_WORDS) return 7;               /* captured the whole ACK */

    /* 4. Now the real send path: sendMessage() must leave txNext in the form
     *    the receipt check expects, so the captured ACK is accepted. */
    memset(&G, 0, sizeof(G));
    memcpy(G.pin, "12345678", 8);
    G.pinLen = PIN_LEN;
    set_local("SEND99", 0x34);
    clearPeer();
    kdfStart();
    while (G.kdfLeft) kdfStep();
    memcpy(G.msg, "HI", 2);
    G.msgLen = 2;
    r5f_writes = 0; ack_words = 0;
    setup(&api);
    A = &api; HOST_API_STUBS(api);
    sendMessage();
    if (!G.ackPending || r5f_writes != PK_WORDS) return 8;
    memcpy(G.pkt.word, recv_ack, sizeof(recv_ack));     /* peer's ACK comes back */
    if (parsePacket() != 2) return 9;                   /* accepted -> tick */
    if (G.hist[0].text[0] != 'O') return 10;
    histAck('V');
    if (G.hist[0].text[0] != 'V') return 11;

    /* 5. Incomplete frame: a chip CRC error must surface as 'N' - once as the
     *    receipt of an outstanding message, once as an inbound marker. */
    memset(&G, 0, sizeof(G));
    G.flags = F_RXON;
    histNew(false); histApp("OUT");
    G.ackPending = 1;
    crc_bad = 1; rx_live = 1; irq_left = 9; irq_src = 0; r5f_reads = 0;
    if (fskPoll() != -1) return 20;                     /* flagged incomplete */
    rxPacket(-1);
    if (G.hist[0].text[0] != 'N' || G.ackPending) return 21;
    crc_bad = 1; rx_live = 1; irq_left = 9; irq_src = 0; r5f_reads = 0;
    if (fskPoll() != -1) return 22;
    rxPacket(-1);
    if (!(G.hist[1].meta & H_RX) || G.hist[1].len != 1u || G.hist[1].text[0] != 'N')
        return 23;

    puts("SECMSG ACK-DELAY/AUDIO PASS");
    return 0;
}
