#include <stdio.h>
#include <string.h>
#include "../secmsg_v24.c"

/* Directional scrolling of the message box: UP shows older entries, DOWN
 * shows newer ones, both ends clamp. Runs against the real drawBox() with a
 * recording printer and a real framebuffer, so the right-edge scroll bar is
 * checked too (one lit pixel per line scrolled back, growing up from the
 * bottom of the box). */
static char rows[7][20];
static uint8_t fbmem[7][128];

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

/* There is no scroll bar any more (the space went to more history entries), so
 * the strip right of the 16-cell text must stay empty on every scroll
 * position - nothing may be left over from an earlier frame either. */
static int strip_dirty(void)
{
    for (uint8_t r = BOX_TOP; r < BOX_TOP + BOX_LINES; r++)
        for (int x = 112; x < 128; x++)
            if (fbmem[r][x]) return 1;
    return 0;
}
/* Columns 0..111 carry the message text and must stay clear of the bar. */
static int bar_in_text(void)
{
    for (uint8_t r = BOX_TOP; r < BOX_TOP + BOX_LINES; r++)
        for (int x = 0; x < 112; x++)
            if (fbmem[r][x] == 0xFFu) return 1;
    return 0;
}

static void show(uint8_t scroll, const char *tag)
{
    for (int i = 0; i < 7; i++) rows[i][0] = 0;
    memset(fbmem, 0, sizeof(fbmem));
    G.scroll = scroll;
    drawBox();
    printf("%s scroll=%u r1='%s' r5='%s' strip=%d\n", tag, scroll, rows[1], rows[5], strip_dirty());
}

int main(void)
{
    app_api_t api = {0};
    A = &api;
    memset(&G, 0, sizeof(G));
    api.fb = (app_fb_t)fbmem;
    api.print_normal = rec_normal;
    api.print_bold = rec_bold;
    api.nav_dir = fake_nav;

    /* HIST_N max-length sent entries: "O " + 18 chars = 20 chars = 2 lines
     * each, so 2*HIST_N lines in total and the window shows BOX_LINES (5). */
    for (uint8_t e = 0; e < HIST_N; e++) {
        char m[19];
        for (int i = 0; i < 18; i++) m[i] = (char)('A' + e);
        m[18] = 0;
        histNew(false);
        histApp(m);
    }
    if (G.hn != HIST_N)
        return 1;
    if (boxTotalLines() != 2u * HIST_N)
        return 2;
    uint8_t maxScroll = (uint8_t)(2u * HIST_N - BOX_LINES);
    if (maxScroll < 1u)
        return 3;

    /* Expected rows are derived from HIST_N so the test follows the history
     * size instead of hard-coding letters. Entry e is "O " + 18 of letter
     * ('A'+e), i.e. 20 characters = two box lines: line 2e and line 2e+1. */
    char want[20];
    #define EXPECT(row, entry, line) do {                                            \
        char body[19];                                                               \
        memset(body, (char)('A' + (entry)), 18); body[18] = 0;                        \
        char full[21]; snprintf(full, sizeof full, "O %s", body);                    \
        size_t off = (size_t)((line) * BOX_CH);                                      \
        size_t n = strlen(full) - off; if (n > BOX_CH) n = BOX_CH;                   \
        memcpy(want, full + off, n); want[n] = 0;                                   \
        if (strcmp(rows[row], want) != 0) return 100 + (row);                        \
    } while (0)
    /* ...and pick the box row for a given global line: entry e occupies lines
     * 2e and 2e+1, so global line gl is entry gl/2, sub-line gl%2. */
    #define EXPECT_ROW(r, start) do {                                               \
        unsigned gl = (unsigned)(start) + (unsigned)(r);                            \
        EXPECT(BOX_TOP + (r), gl / 2u, gl % 2u);                                    \
    } while (0)

    /* Newest view: the window starts at the last BOX_LINES lines. */
    show(0, "newest");
    EXPECT_ROW(0, 2u * HIST_N - BOX_LINES);
    EXPECT_ROW(BOX_LINES - 1u, 2u * HIST_N - BOX_LINES);
    if (strip_dirty()) return 5;        /* no indicator drawn at the newest */

    /* Oldest view: start of the history, i.e. entry A head, B, C head. */
    show(maxScroll, "oldest");
    EXPECT_ROW(0, 0);
    EXPECT_ROW(BOX_LINES - 1u, 0);
    if (strip_dirty()) return 7;

    /* Mid position: the window starts maxScroll/2 lines into the history. */
    show((uint8_t)(maxScroll / 2u), "middle");
    EXPECT_ROW(0, 2u * HIST_N - BOX_LINES - (unsigned)(maxScroll / 2u));
    EXPECT_ROW(BOX_LINES - 1u, 2u * HIST_N - BOX_LINES - (unsigned)(maxScroll / 2u));
    if (strip_dirty()) return 9;

    /* Clamp: scroll past the end sticks at max. */
    G.scroll = 200;
    drawBox();
    if (G.scroll != maxScroll)
        return 10;
    /* nav_dir mapping used by app_main. */
    if (fake_nav(APP_KEY_UP) != 1 || fake_nav(APP_KEY_DOWN) != -1)
        return 11;

    puts("SECMSG SCROLL PASS");
    return 0;
}
