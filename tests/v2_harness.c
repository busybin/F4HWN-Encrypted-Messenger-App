#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"

/* Launch smoke test: run the real app_main loop against a minimal API surface
 * and make sure it starts, idles and exits via the two-press EXIT gesture. */

static unsigned calls;
static uint8_t fb_mem[7][128];
static uint8_t key_fn(void)
{
    calls++;
    if (calls <= 20) return APP_KEY_INVALID;
    /* EXIT is a two-press action; E on every third call quits. */
    return (calls % 3u == 1u) ? APP_KEY_EXIT : APP_KEY_INVALID;
}
static void delay_fn(uint32_t ms) { (void)ms; }
static void noop(void) {}
static uint8_t af_last = 0xFF;
static void setaf_fn(uint8_t m) { af_last = m; }
static void tones_fn(void) { af_last = APP_AF_MUTE; }  /* driver ends with SetAF(MUTE) */
static void print3_fn(const char *s, uint8_t a, uint8_t b, uint8_t c) { (void)s; (void)a; (void)b; (void)c; }
static void print6_fn(const char *s, uint8_t a, uint8_t b, bool c, bool d, uint8_t e) { (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; }
static void bool_fn(bool b) { (void)b; }
static void uid0_fn(uint8_t *p, uint8_t n) { memset(p, 0, n); }
static void save_fn(const uint8_t *p, uint8_t n) { (void)p; (void)n; }
static uint16_t bk_read_fn(uint8_t r) { (void)r; return 0; }
static void bk_write_fn(uint8_t r, uint16_t v) { (void)r; (void)v; }
static uint32_t freq_fn(void) { return 14500000u; }
static uint8_t txstate_fn(void) { return 0; }
static int8_t nav_fn(uint8_t k) { (void)k; return 0; }
static void uid_fn(uint8_t *p) { for (int i = 0; i < 12; i++) p[i] = (uint8_t)(i + 1); }

int main(void)
{
    app_api_t api; memset(&api, 0, sizeof api);
    api.abi_major = APP_ABI_MAJOR; api.api_level = APP_API_LEVEL; api.api_size = sizeof api;
    api.fb = (app_fb_t)fb_mem; api.get_key = key_fn; api.delay_ms = delay_fn;
    api.display_clear = noop; api.status_clear = noop; api.blit_full = noop; api.blit_status = noop;
    api.print_bold = print3_fn; api.print_normal = print3_fn; api.print_inverse = print6_fn; api.draw_battery = noop;
    api.bk_read = bk_read_fn; api.bk_write = bk_write_fn; api.tones_off_rx = tones_fn;
    api.set_agc = bool_fn; api.audio_path = bool_fn; api.backlight_on = noop; api.backlight_update = noop;
    api.set_af = setaf_fn;
    api.rx_freq = freq_fn; api.tx_freq = freq_fn; api.tx_state = txstate_fn; api.nav_dir = nav_fn;
    api.tx_end = noop;
    api.uid_get = uid_fn; api.cfg_load = uid0_fn; api.cfg_save = save_fn; api.led = bool_fn; api.battery_sample = noop;
    app_main(&api);
    if (af_last != APP_AF_MUTE) { puts("HARNESS FAIL: idle AF not muted"); return 1; }
    puts("HARNESS PASS");
    return 0;
}
