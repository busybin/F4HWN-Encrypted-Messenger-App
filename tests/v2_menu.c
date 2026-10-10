#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"

/* Settings menu: three rows (PIN / ID / ACK). UP/DOWN picks the row, MENU
 * enters and stores the selected row's edit, then digits (PIN/ID) or UP/DOWN
 * (ACK interval) change the value. PIN always renders plaintext. */
static char rows[7][20];
static void rec_normal(const char *s, uint8_t a, uint8_t b, uint8_t line)
{
    (void)a; (void)b;
    if (line < 7) {
        size_t n = strlen(s);
        if (n > 18) n = 18;
        memcpy(rows[line], s, n);
        rows[line][n] = 0;
    }
}
static void rec_bold(const char *s, uint8_t a, uint8_t b, uint8_t line) { rec_normal(s, a, b, line); }
static int8_t fake_nav(uint8_t k)
{
    if (k == APP_KEY_UP) return 1;
    if (k == APP_KEY_DOWN) return -1;
    return 0;
}
static uint8_t saved_cfg[64];
static void cfg_save_fn(const uint8_t *p, uint8_t n) { if (n <= sizeof(saved_cfg)) memcpy(saved_cfg, p, n); }
static uint16_t bk_read_fn(uint8_t r) { (void)r; return 0; }
static void bk_write_fn(uint8_t r, uint16_t v) { (void)r; (void)v; }
static void delay_fn(uint32_t ms) { (void)ms; }
static void noop(void) {}

static void draw_clear(void) { for (int i = 0; i < 7; i++) rows[i][0] = 0; }

int main(void)
{
    app_api_t api = {0};
    A = &api;
    memset(&G, 0, sizeof(G));
    api.print_normal = rec_normal;
    api.print_bold = rec_bold;
    api.nav_dir = fake_nav;
    api.cfg_save = cfg_save_fn;
    api.bk_read = bk_read_fn;
    api.bk_write = bk_write_fn;
    api.tones_off_rx = noop;
    api.delay_ms = delay_fn;

    memcpy(G.pin, "12345678", 8);
    G.pinLen = 8;
    memcpy(G.id.localId, "BOB", 3);
    G.idLen = 3;
    G.ackS = 3;                     /* 1.5 s */
    G.menu = 1;

    /* Selected row uses '>', others ' '; no edit cursor yet. */
    draw_clear(); drawMenu();
    if (strcmp(rows[1], ">PIN 12345678") != 0) return 1;
    if (strcmp(rows[2], " ID BOB") != 0) return 2;
    if (strcmp(rows[3], " ACK 1.5") != 0) return 3;

    /* Edit cursor '*' marks the row being edited. */
    G.flags |= F_EDIT;
    draw_clear(); drawMenu();
    if (strcmp(rows[1], "*PIN 12345678") != 0) return 4;
    G.flags &= (uint8_t)~F_EDIT;
    G.menu = 3;
    draw_clear(); drawMenu();
    if (strcmp(rows[3], ">ACK 1.5") != 0) return 5;

    /* 10.0 s upper end renders correctly. */
    G.ackS = 20;
    draw_clear(); drawMenu();
    if (strcmp(rows[3], ">ACK 10.0") != 0) return 6;
    G.ackS = 3;

    /* UP/DOWN only moves the selection while not editing; UP goes toward the
     * first row and the ends clamp. */
    G.menu = 1;
    menuKey(APP_KEY_UP);
    if (G.menu != 1 || G.ackS != 3) return 7;        /* clamped at the first row */
    menuKey(APP_KEY_DOWN);
    if (G.menu != 2 || G.ackS != 3) return 8;
    menuKey(APP_KEY_DOWN);
    if (G.menu != 3 || G.ackS != 3) return 9;
    menuKey(APP_KEY_DOWN);
    if (G.menu != 3) return 10;                      /* clamped at the last row */
    menuKey(APP_KEY_UP);
    if (G.menu != 2) return 11;

    /* Digits do nothing until MENU enters edit (PIN row). */
    G.menu = 1;
    handleTap(APP_KEY_5);                            /* menuKey gates this in app_main */
    if (G.pinLen != 8) return 12;

    /* MENU enters and stores the edit, and the ACK value needs the edit too. */
    menuKey(APP_KEY_MENU);
    if (!(G.flags & F_EDIT)) return 13;
    menuKey(APP_KEY_UP);
    if (G.ackS != 3) return 14;                      /* PIN row: UP/DOWN is inert */
    menuKey(APP_KEY_MENU);
    if (G.flags & F_EDIT) return 15;
    G.menu = 3;
    menuKey(APP_KEY_MENU);
    menuKey(APP_KEY_UP);
    if (G.ackS != 4) return 16;                      /* +0.5 s */
    menuKey(APP_KEY_DOWN);
    if (G.ackS != 3) return 17;
    for (int i = 0; i < 40; i++) menuKey(APP_KEY_UP);
    if (G.ackS != ACK_MAX) return 18;                /* clamped at 10 s */
    for (int i = 0; i < 40; i++) menuKey(APP_KEY_DOWN);
    if (G.ackS != ACK_MIN) return 19;                /* clamped at 0.5 s */
    menuKey(APP_KEY_MENU);

    /* Incomplete PIN blocks saving (keeps the working PIN). */
    G.pinLen = 5;
    draw_clear(); drawMenu();
    if (strcmp(rows[ROW_IN], "NEED 8 DIGITS") != 0) return 20;  /* compose row */
    if (closeMenu()) return 21;
    G.pinLen = 8;
    G.ackS = 4;
    if (!closeMenu() || G.menu != 0 || (G.flags & F_EDIT)) return 22;
    if (saved_cfg[0] != CFG_MAGIC || saved_cfg[1] != CFG_VER || saved_cfg[48] != 4u) return 23;

    puts("SECMSG MENU PASS");
    return 0;
}
