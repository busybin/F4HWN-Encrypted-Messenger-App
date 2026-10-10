#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"

/* Screen layout, locked down so it cannot silently drift:
 *
 *   row 0        inverted identity line, local ID only (plus the F+ downlink
 *                marker) - no frequency, no "RX", no message count
 *   rows 1..4    the message box, BOX_LINES visible lines
 *   row 5        the compose box: ">text_" plus the remaining-character hint
 *                "(nn)" right aligned, inverted
 *   row 6        status row: the ACK receipt interval left ("ACK 1.5") and the
 *                frequency flush right ("145.07500")
 */

/* Flat 19-cell-per-row text model of the screen: the app prints a row in
 * pieces at different x offsets, so the recorder has to place each piece in
 * the right cell instead of letting the last print win. */
#define ROWW 19
static char rowbuf[7 * ROWW];
#define ROW(r) (rowbuf + (size_t)(r) * ROWW)
static uint8_t fb_mem[7][128];
static unsigned rx_freq_calls;

/* Place the string into an 18-cell model of the row at its x pixel column, the
 * way the firmware does, so a left and a right aligned print on the same row
 * do not clobber each other. */
static void rec(const char *s, uint8_t a, uint8_t b, uint8_t line)
{
    (void)b;
    if (line >= 7) return;
    char *dst = ROW(line);
    size_t col = (size_t)(a / 7u);
    for (const char *p = s; *p && col < 18u; p++, col++) dst[col] = *p;
    dst[18] = 0;
}
static void rec_zero(uint8_t line)
{
    if (line >= 7) return;
    memset(ROW(line), ' ', 18);
    ROW(line)[18] = 0;
}
static void rec_start(void)
{
    for (uint8_t r = 0; r < 7; r++) rec_zero(r);
}

static void rec_bold(const char *s, uint8_t a, uint8_t b, uint8_t line) { rec(s, a, b, line); }
static void rec_inv(const char *s, uint8_t x, uint8_t l, bool a, bool b, uint8_t e)
{ (void)s; (void)x; (void)l; (void)a; (void)b; (void)e; }  /* status bar only */
static uint32_t freq_fn(void) { rx_freq_calls++; return 14500000u; }
static void noop(void) {}

int main(void)
{
    app_api_t api; memset(&api, 0, sizeof api);
    A = &api;
    api.fb = (app_fb_t)fb_mem;
    api.print_normal = rec; api.print_bold = rec_bold; api.print_inverse = rec_inv;
    api.rx_freq = freq_fn;
    api.display_clear = noop; api.status_clear = noop; api.draw_battery = noop;
    api.blit_full = noop; api.blit_status = noop;

    memset(&G, 0, sizeof(G));
    memset(G.id.localId, 0, 8);
    for (int i = 0; i < ID_MAX; i++) G.id.localId[i] = (uint8_t)"LAYOUT"[i];
    G.idLen = ID_MAX;
    G.flags = F_RUN;
    G.ackS = ACK_DEF;

    /* One received entry so the box has something to show. */
    histNew(true);
    histApp("BOB:HELLO");   /* the app composes "<senderID>:<text>" before storing */

    rec_start();
    memset(fb_mem, 0, sizeof fb_mem);
    rx_freq_calls = 0;

    strcpy(G.msg, "AB"); G.msgLen = 2; G.blink = 0;
    draw();

    if (rx_freq_calls == 0) return 1;                 /* status row shows the freq */

    /* Row 0: identity only, no digits that could be a frequency. */
    if (strncmp(ROW(0), "LAYOUT", 6) != 0) return 2;
    if (strpbrk(ROW(0), ".0123456789")) return 3;

    /* Rows 1..4 are the box, row 5 composes, row 6 is the status row. */
    if (BOX_LINES != 4 || BOX_TOP != 1 || ROW_IN != 5 || ROW_ST != 6) return 5;
    if (strncmp(ROW(1), "BOB:HELLO", 9) != 0) return 6; /* sender ID prefix, one line */
    if (strcmp(ROW(2), "                  ") != 0) return 7;
    if (strcmp(ROW(4), "                  ") != 0) return 8;

    /* Compose box: text at the left, remaining-character hint at the right. */
    if (strncmp(ROW(ROW_IN), ">AB", 3) != 0) return 9;
    char want[20];
    snprintf(want, sizeof want, "(%u)", (unsigned)(MSG_MAX - G.msgLen));
    if (strcmp(ROW(ROW_IN) + 18 - strlen(want), want) != 0) return 10;   /* flush right */
    if (strcmp(ROW(ROW_IN), ">AB           (16)") != 0) return 11;   /* whole row */

    /* Cursor blink is part of the compose box. */
    G.blink = 1;
    rec_start();
    draw();
    if (strncmp(ROW(ROW_IN), ">AB_", 4) != 0) return 12;

    /* The hint shrinks as text is typed, and the visible window slides right
     * once the text no longer fits in the space left of the hint. */
    for (uint8_t i = 0; i < MSG_MAX; i++) G.msg[i] = (char)('A' + (i % 26u));
    G.msgLen = MSG_MAX; G.blink = 0;
    rec_start();
    draw();
    snprintf(want, sizeof want, "(0)");
    if (strcmp(ROW(ROW_IN) + 18 - strlen(want), want) != 0) return 13;
    /* 14 cells are reserved for the widest hint "(18)", so the text window is
     * ">" plus MSG_MAX-5 characters and slides to the tail of the message. */
    if (strcmp(ROW(ROW_IN), ">FGHIJKLMNOPQR (0)") != 0) return 14;

    /* The scroll bar is gone (its bytes bought history entries), so the strip
     * right of the 16-cell text must stay empty. */
    for (uint8_t r = BOX_TOP; r < BOX_TOP + BOX_LINES; r++)
        for (int x = 112; x < 128; x++)
            if (fb_mem[r][x]) return 18;

    /* Status row: ACK interval left, frequency flush right, neither inverted. */
    if (strncmp(ROW(ROW_ST), "ACK 1.5", 7) != 0) return 28;
    if (strncmp(ROW(ROW_ST) + 9, "145.00000", 9) != 0) return 29;
    if (fb_mem[ROW_ST][0] == 0xFFu) return 30;

    /* Compose box and identity row are inverted; the message box is not. */
    memset(fb_mem, 0, sizeof fb_mem);
    draw();
    if (fb_mem[0][3] != 0xFFu) return 15;             /* identity row inverted */
    if (fb_mem[ROW_IN][3] != 0xFFu) return 16;        /* compose row inverted */
    if (fb_mem[2][3] == 0xFFu && fb_mem[2][100] == 0xFFu) return 17; /* box rows not inverted */

    puts("SECMSG LAYOUT PASS");
    return 0;
}
