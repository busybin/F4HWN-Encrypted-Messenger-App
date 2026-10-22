#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"
#include "host_api.h"

/* F+1..9 prosigns through the real app_main loop: latch F (holdTick counts
 * fHoldMs while the key reads back as F), then press a digit; the prosign text
 * is appended to the compose buffer exactly like the CW app. */

static const uint8_t *script;
static unsigned script_len, script_pos;
static uint8_t key_fn(void)
{
    if (script_pos < script_len) return script[script_pos++];
    return APP_KEY_INVALID;
}

static uint16_t bk0(uint8_t r) { (void)r; return 0; }
static void bkw(uint8_t r, uint16_t v) { (void)r; (void)v; }
static void noop(void) {}
static void noop_b(bool b) { (void)b; }
static void noop_u8(uint8_t u) { (void)u; }
static void noop_u32(uint32_t u) { (void)u; }
static void print3(const char *s, uint8_t a, uint8_t b, uint8_t c) { (void)s; (void)a; (void)b; (void)c; }
static void print6(const char *s, uint8_t a, uint8_t b, bool c, bool d, uint8_t e)
{ (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; }
static uint8_t fb_mem[7][128];
static void cfg0(uint8_t *p, uint8_t n) { memset(p, 0, n); }
static void cfgw(const uint8_t *p, uint8_t n) { (void)p; (void)n; }
static uint8_t tx0(void) { return 0; }
static uint32_t frq(void) { return 14500000u; }
static int8_t nav0(uint8_t k) { (void)k; return 0; }
static void uid0(uint8_t *p) { for (int i = 0; i < 12; i++) p[i] = (uint8_t)(i + 1); }

static void run(const uint8_t *keys, unsigned n)
{
    app_api_t api = {0};
    memset(&api, 0, sizeof(api));
    api.abi_major = APP_ABI_MAJOR; api.api_level = APP_API_LEVEL; api.api_size = sizeof(api);
    api.fb = (app_fb_t)fb_mem;
    api.display_clear = noop; api.status_clear = noop;
    api.print_bold = print3; api.print_normal = print3; api.print_inverse = print6;
    api.blit_full = noop; api.blit_status = noop; api.draw_battery = noop;
    api.get_key = key_fn; api.delay_ms = noop_u32; api.led = noop_b;
    api.bk_read = bk0; api.bk_write = bkw;
    api.set_agc = noop_b; api.set_af = noop_u8; api.audio_path = noop_b;
    api.tones_off_rx = noop; api.rx_freq = frq; api.tx_freq = frq;
    api.tx_state = tx0; api.tx_set_params = noop; api.tx_end = noop;
    api.uid_get = uid0; api.cfg_load = cfg0; api.cfg_save = cfgw; api.nav_dir = nav0;
    api.backlight_on = noop; api.backlight_update = noop; api.battery_sample = noop;

    memset(&G, 0, sizeof(G));
    script = keys; script_len = n; script_pos = 0;
    A = &api; HOST_API_STUBS(api);
    app_main(&api);
}

#define I APP_KEY_INVALID
#define F APP_KEY_F

int main(void)
{
    /* F held two ticks, released -> F_FDOWN; then the prosign of that key.
     * Table after the F4HWN reset: F+1 CQ F+2 DE F+3 UR F+4 RST F+5 R
     * F+6 TU F+7 73 F+8 AR F+9 SK; F+0 is empty. */
    static const uint8_t s_de[] = {
        F, F, F, F, F, I, 2, I, I, I, APP_KEY_EXIT, I, APP_KEY_EXIT, APP_KEY_EXIT, I, APP_KEY_EXIT,
    };
    run(s_de, sizeof(s_de));
    if (G.msgLen != 2u || memcmp(G.msg, "DE", 2) != 0) return 1;

    static const uint8_t s_sk[] = {
        F, F, F, I, 9, I, I, I, APP_KEY_EXIT, I, APP_KEY_EXIT, APP_KEY_EXIT, I, APP_KEY_EXIT,
    };
    run(s_sk, sizeof(s_sk));
    if (G.msgLen != 2u || memcmp(G.msg, "SK", 2) != 0) return 2;

    static const uint8_t s_r[] = {
        F, F, F, I, 5, I, I, I, APP_KEY_EXIT, I, APP_KEY_EXIT, APP_KEY_EXIT, I, APP_KEY_EXIT,
    };
    run(s_r, sizeof(s_r));
    if (G.msgLen != 1u || G.msg[0] != 'R') return 4;

    static const uint8_t s_73[] = {
        F, F, F, I, 7, I, I, I, APP_KEY_EXIT, I, APP_KEY_EXIT, APP_KEY_EXIT, I, APP_KEY_EXIT,
    };
    run(s_73, sizeof(s_73));
    if (G.msgLen != 2u || memcmp(G.msg, "73", 2) != 0) return 3;

    puts("SECMSG PROSIGN PASS");
    return 0;
}
