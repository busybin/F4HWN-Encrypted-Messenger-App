/* Host stand-ins for the API level 3 services the App code reaches through the
 * resident table. The radio firmware provides them; the host tests must not
 * dereference NULL for the ones the drawing paths now call. */
#ifndef HOST_API_H
#define HOST_API_H

#include <stdint.h>

/* uidivmod: quotient in the low word, remainder in the high word. */
static uint64_t host_uidivmod(uint32_t n, uint32_t d)
{
    if (d == 0u) return (uint64_t)n << 32;
    return ((uint64_t)(n % d) << 32) | (n / d);
}

static char *host_append_u32(char *d, uint32_t v)
{
    char t[10];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *d++ = t[--n];
    return d;
}

static void host_blit_page(uint8_t page) { (void)page; }
static void host_noop(void) { }

#define HOST_API_STUBS(api) do {                  \
        (api).blit_page = host_blit_page;         \
        (api).append_u32 = host_append_u32;       \
        (api).uidivmod = host_uidivmod;           \
        (api).viewer_update = host_noop;          \
    } while (0)

#endif
