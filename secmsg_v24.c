#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_api.h"
#include "secmsg_crypto.h"

/* SECMSG v2.0: encrypted short messages and authenticated delivery receipts.
 * Fixed 72-byte frame:
 *   [start][len64]["SM" ver kind/len sender64 counter64 ct32 tag12][crc][end]
 * Normal messages keep the v2 32-byte payload. ACK uses kind 0x40 and carries
 * the original sender/counter in its encrypted payload. The first six sender
 * ID bytes are the user ID; the last two are a UID-derived suffix.
 * Crypto: salted 65536-round Speck KDF, separate Kenc/Kmac, CTR + 96-bit
 * CBC-MAC, 64-bit counters and a persisted 64-packet anti-replay window.
 *
 * UI feedback is visual only: the FSK modem owns REG_70, so tones would damage
 * its configuration. F+0 opens the PIN panel; MENU opens the ID editor.
 */

#define LOOP_MS     10      /* main-loop pacing: one iteration ~= 10 ms real time.
                             * The loop free-spins otherwise, and every timer below
                             * would expire in milliseconds (PTT/F dead, popups invisible). */
#define HOLD_TICKS  150     /* 1500 ms long-press threshold */
#define TAP_TICKS   90      /* 900 ms multitap window */
#define FTAP_TICKS  40      /* 400 ms double-tap window */
#define POP_TICKS   150     /* 1500 ms on-screen prompt lifetime */
#define BLINK_TICKS 40      /* 400 ms cursor blink */
#define BAT_TICKS   200     /* 2000 ms battery sample */
#define RX_TIMEOUT  200     /* 2000 ms without RX progress: drop partial frame */
#define MSG_MAX     18
#define PIN_LEN     8
#define ID_MAX      6
#define TEXT_MAX    (ID_MAX + 1 + MSG_MAX)   /* "ID" + ':' + text (BSS counts) */
#define HIST_N      43
#define BOX_LINES   4       /* visible message-box lines (rows 1..4) */
#define BOX_TOP     1
#define ROW_IN      5       /* compose/hint row, just above the status row */
#define ROW_ST      6       /* bottom status row: ACK left, frequency right */
#define BOX_CH      16
#define CFG_MAGIC   0xE2
#define CFG_VER     3u
#define CFG_SIZE    49
#define CFG_CRC     46      /* c[46..47] hold the CRC over c[0..45] */
#define KDF_ROUNDS  65536u
/* Receipt interval: the receiver waits ackS*0.5 s after a frame before ACKing. */
#define ACK_MIN     1u      /* 0.5 s  */
#define ACK_MAX     20u     /* 10.0 s */
#define ACK_DEF     3u      /* 1.5 s  */
#define ACK_UNIT    50u     /* 10 ms ticks per 0.5 s */
#define AF_HOLD     30      /* 300 ms of audible AF after the last FSK activity */
#define EXIT_TICKS  150     /* EXIT must be pressed twice within 1.5 s to quit */

#define PK_WORDS    36u
#define PK_MAGIC0   'S'
#define PK_MAGIC1   'M'
#define PK_VER      2u
#define PK_KIND_MASK 0xC0u
#define PK_KIND_ACK  0x40u  /* low six bits remain the payload length */
#define TAG_LEN     12

/* History meta: bit7 direction; sent text starts O/V/X for receipt state. */
#define H_RX       0x80u

/* BK4819 registers / bits (mirror driver/bk4819-regs.h, overlay has no defs) */
#define R02 0x02u
#define R0B 0x0Bu
#define R0C 0x0Cu
#define R30 0x30u
#define R3F 0x3Fu
#define R59 0x59u
#define R5F 0x5Fu
#define F_TXFIN  0x8000u
#define F_RXFIN  0x2000u
#define F_FFULL  0x1000u
#define GPIO_REG 0x33u

#define F_RUN   (1<<0)
#define F_RXON  (1<<1)   /* FSK receiver armed */
#define F_EDIT  (1<<2)   /* settings menu: selected row is being edited */
#define F_FDOWN (1<<4)

/* The table is indexed directly; rows 1..9 are the non-empty mappings. */
static const char KEYMAP[10][5] = {
    {0}, {'1','.', '0','/'}, {'A','B','C','2'}, {'D','E','F','3'},
    {'G','H','I','4'}, {'J','K','L','5'}, {'M','N','O','6'}, {'P','Q','R','S','7'},
    {'T','U','V','8'}, {'W','X','Y','Z','9'}
};
static const char POPUPS[4][6] = {"BUSY","FULL","TXOFF","EXIT?"};
/* F+1..9 prosigns (same set as the CW app), packed into one blob with byte
 * offsets. Offset 0 points at the leading NUL, so unassigned keys insert
 * nothing and no 0xFF sentinel branch is needed. */
static const char PROSIGN_ALL[] = "\0" "CQ\0" "DE\0" "UR\0" "RST\0" "R\0" "TU\0" "73\0" "AR\0" "SK\0";
static const uint8_t PROSIGN_OFF[10] = {0u, 1u, 4u, 7u, 10u, 14u, 16u, 19u, 22u, 25u};

/* Big-endian numeric forms of the fixed protocol labels. */
#define KDF_LABEL0 0x534D32004B444631ULL
#define KDF_SALT0  0x534D3253414C5431ULL
#define ENC_LABEL0  0x534D3200454E4300ULL
#define MAC_LABEL0  0x534D32004D414300ULL
static const app_api_t *A;
static void clearPeer(void);
static void saveConfig(void);

typedef struct __attribute__((aligned(4))) {
    uint8_t localId[8], txNext[8], peerId[8], rxHi[8], rxBits[8];
} secmsg_id_state_t;
typedef struct {
    char text[TEXT_MAX];
    uint8_t len, meta;
} hist_entry_t;
typedef struct __attribute__((aligned(4))) {
    uint16_t word[PK_WORDS];
} secmsg_packet_t;
_Static_assert(sizeof(secmsg_id_state_t) == 40u, "identity state layout");
_Static_assert(_Alignof(secmsg_id_state_t) >= 4u, "identity state alignment");
_Static_assert(offsetof(secmsg_id_state_t, txNext) == 8u, "identity state layout");
_Static_assert(offsetof(secmsg_id_state_t, peerId) == 16u, "identity state layout");
_Static_assert(offsetof(secmsg_id_state_t, rxHi) == 24u, "identity state layout");
_Static_assert(offsetof(secmsg_id_state_t, rxBits) == 32u, "identity state layout");
_Static_assert(sizeof(hist_entry_t) == TEXT_MAX + 2u, "history state layout");
_Static_assert(_Alignof(secmsg_packet_t) >= 4u, "packet state alignment");

typedef struct {
    uint8_t  tapMs, fTapMs, fHoldMs, pttHoldMs;
    uint8_t  flags, msgLen, lastKey, tapIdx, prevKey, batTimer;
    bool     pinDirty;
    uint8_t  menu, pinLen, scroll, blink, pop, popT, hn, exArm;
    uint8_t  idLen, ackPending;
    uint8_t  ackS, afT;            /* ackS: receipt interval in 0.5 s units */
    uint16_t ackWait, ackT;        /* receiver ACK delay / sender timeout    */
    uint32_t kdfLeft;  /* background KDF iterations remaining; 0 = not deriving */
    uint64_t key[2];    /* shared PIN-KDF base key; per-sender subkeys are temporary */
    secmsg_id_state_t id;
    char     msg[MSG_MAX+1];       /* length-delimited; NUL is added only for history */
    char     pin[PIN_LEN];         /* length-delimited PIN digits */
    hist_entry_t hist[HIST_N];
    secmsg_packet_t pkt;
    uint8_t  fIdx, fAge;
} secmsg_state_t;
_Static_assert(offsetof(secmsg_state_t, id) % 4u == 0u, "state id alignment");
_Static_assert(offsetof(secmsg_state_t, pkt) % 4u == 0u, "state packet alignment");
static secmsg_state_t G;

static char *pstr(char *d, const char *s){
    while((*d = *s++)) d++;
    return d;
}

/* One decimal digit: repeated subtraction keeps libgcc division out of the
 * image. UI values are tiny except the frequency fraction (<100000). */
static uint32_t div10(uint32_t n, uint8_t *rem){
    uint32_t q = 0;
    while(n >= 10u){ n -= 10u; q++; }
    *rem = (uint8_t)n;
    return q;
}

/* Decimal, optionally zero-padded on the left (pad = minimum digit count). The
 * frequency needs five decimals, so the pad path is not optional. */
static char *pnum(char *d, uint32_t v, uint8_t pad){
    char t[10]; uint8_t n = 0, r;
    do { v = div10(v, &r); t[n++] = (char)('0' + r); } while(v);
    while(n < pad) t[n++] = '0';
    while(n--) *d++ = t[n];
    *d = 0;
    return d;
}

static uint16_t crc16(const uint8_t *data, uint16_t size){
    uint16_t crc = 0;
    while(size--){
        crc ^= (uint16_t)*data++ << 8;
        for(uint8_t bit = 0; bit < 8u; bit++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static inline bool zero8(const uint8_t *p){
    uint8_t v = 0;
    for(uint8_t i = 0; i < 8u; i++) v |= p[i];
    return v == 0;
}
static inline bool same8(const uint8_t *a, const uint8_t *b){
    uint8_t d = 0;
    for(uint8_t i = 0; i < 8u; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
static inline void copy8(uint8_t *d, const uint8_t *s){
    for(uint8_t i = 0; i < 8u; i++) d[i] = s[i];
}
__attribute__((noinline)) static uint8_t idLenOf(const uint8_t *id){
    uint8_t n = 0;
    while(n < ID_MAX && id[n] >= 0x20u && id[n] <= 0x7Eu) n++;
    return n;
}
/* Little-endian increment; true means no wrap. */
__attribute__((noinline)) static bool inc8(uint8_t *a){
    for(uint8_t i = 0; i < 8; i++) if(++a[i]) return true;
    return false;
}
static void ctrV2(const uint64_t encKey[2], const uint8_t counter[8], uint8_t block, uint8_t out[16]){
    uint64_t w[2], o[2];
    w[0] = ld64be(counter); w[1] = block;
    speck128_enc(encKey, w, o);
    st64be(out, o[0]); st64be(out + 8, o[1]);
}
static void deriveSession(const uint8_t id[8], uint64_t enc[2], uint64_t mac[2]){
    uint64_t in[2], out[2];
    in[0] = ENC_LABEL0; in[1] = ld64be(id);
    speck128_enc(G.key, in, out); enc[0] = out[0]; enc[1] = out[1];
    in[0] = MAC_LABEL0;
    speck128_enc(G.key, in, out); mac[0] = out[0]; mac[1] = out[1];
}

/* One authenticated peer, 64-packet sliding replay window. */
static bool acceptCounter(const uint8_t id[8], const uint8_t counter[8]){
    if(zero8(id)) return false;
    if(zero8(G.id.peerId)){
        copy8(G.id.peerId, id); copy8(G.id.rxHi, counter);
        G.id.rxBits[0] = G.id.rxBits[1] = G.id.rxBits[2] = G.id.rxBits[3] = 0;
        G.id.rxBits[4] = G.id.rxBits[5] = G.id.rxBits[6] = 0; G.id.rxBits[7] = 1;
        return true;
    }
    if(!same8(G.id.peerId, id)) clearPeer();
    uint64_t hi = ld64be(G.id.rxHi), v = ld64be(counter), bits = ld64be(G.id.rxBits);
    if(v > hi){
        uint64_t d = v - hi;
        bits = (d >= 64u) ? 1u : ((bits << d) | 1u);
        hi = v;
    } else {
        uint64_t d = hi - v;
        if(d >= 64u || (bits & (1ULL << d))) return false;
        bits |= 1ULL << d;
    }
    st64be(G.id.rxHi, hi); st64be(G.id.rxBits, bits);
    return true;
}

__attribute__((noinline)) static void alert(uint8_t idx){ G.pop = idx; G.popT = POP_TICKS; }  /* visual only: tones would clobber REG_70 FSK config */

static void bw(uint8_t reg, uint16_t value){ A->bk_write(reg, value); }
static void dly(uint32_t ms){ A->delay_ms(ms); }
static uint16_t abr(uint8_t r){ return A->bk_read(r); }
static void redLed(bool on){
    uint16_t v = abr(GPIO_REG) & ~1u;
    bw(GPIO_REG, on ? (uint16_t)(v | 1u) : v);
}

/* ---- FSK modem (register sequences copied from resident BK4819_*FSK*) ---- */
static void fskReset(void){
    bw(R3F, 0x0000u);
    bw(R59, 0x0068u);
    dly(30);
    bw(R30, 0x0000u);
}

static void fskConfig(void){
    /* Packed {register, value} words; order is significant. */
    static const uint32_t cfg[] = {
        0x00E070u, 0x306572u, 0x00C158u, 0x56655Cu, 0x47005Du
    };
    for(uint8_t i = 0; i < 5u; i++) bw((uint8_t)cfg[i], (uint16_t)(cfg[i] >> 8));
}

static void fskSetup(void){
    fskConfig();
    fskReset();
}

static void fskRxArm(void){
    fskReset();
    A->tones_off_rx();   /* restore REG_30 RX_LINK/RX_DSP; also clears REG_70 */
    fskConfig();          /* Tone2/FSK config must follow tones_off_rx */
    bw(R02, 0);
    bw(R3F, 0);
    bw(R3F, (uint16_t)(F_RXFIN | F_FFULL));
    bw(R59, 0x4068u);
    bw(R59, 0x3068u);
    G.fIdx = 0;
    G.fAge = 0;
    G.flags |= F_RXON;
}

/* Returns 1 for a full 36-word frame in G.pkt, 0 for no frame, -1 when a frame
 * was started but not copied completely (truncated or chip CRC error). */
static int8_t fskPoll(void){
    if(!(G.flags & F_RXON)) return 0;
    uint8_t spins = 0;
    bool rxfin = false;
    while(spins++ < 64 && (abr(R0C) & 1u)){
        G.fAge = 0;
        bw(R02, 0);
        uint16_t irq = abr(R02);
        G.afT = AF_HOLD;             /* data on air: open the AF so it is audible */
        if(irq & F_RXFIN) rxfin = true;
        if(irq & (F_FFULL | F_RXFIN)){
            unsigned n = (irq & F_RXFIN) ? ((G.fIdx < PK_WORDS) ? (PK_WORDS - G.fIdx) : 0u) : 4u;
            for(unsigned i = 0; i < n; i++){
                uint16_t w = abr(R5F);
                if(G.fIdx < PK_WORDS) G.pkt.word[G.fIdx++] = w;
            }
        }
    }
    /* A stuck IRQ must never monopolise the app: recycle RX and keep UI alive. */
    if(spins >= 64 && (abr(R0C) & 1u)){ fskRxArm(); return 0; }
    /* RXFIN with a short FIFO is a truncated/aborted frame. */
    if(rxfin && G.fIdx < PK_WORDS){ fskRxArm(); return -1; }
    if(G.fIdx < PK_WORDS) return 0;
    G.fIdx = 0;
    uint16_t st = abr(R0B);
    fskRxArm();
    return (st & 0x0010u) ? -1 : 1;  /* chip CRC/format error = incomplete */
}

static void fskSend(void){
    uint8_t to = 200;
    A->tx_set_params();
    redLed(true);        /* after TX GPIO setup, otherwise its shadow write clears it */
    fskConfig();
    dly(20);
    bw(R3F, F_TXFIN);
    bw(R59, 0x8068u);
    bw(R59, 0x0068u);
    for(unsigned i = 0; i < PK_WORDS; i++) bw(R5F, G.pkt.word[i]);
    dly(20);
    bw(R59, 0x2868u);
    while(to-- && (abr(R0C) & 1u) == 0){
        A->backlight_update();
        dly(5);
    }
    redLed(false);
    bw(R02, 0);
    dly(20);
    fskReset();
    A->tx_end();
    A->audio_path(true);   /* RADIO_SetTxParameters killed the speaker path */
}

/* ---- packet build / parse ---- */
static uint8_t *pbytes(void){ return (uint8_t *)G.pkt.word; }

static void finishPacket(uint8_t kind, uint8_t len){
    uint8_t *p = pbytes();
    p[0] = 0xCDu; p[1] = 0xABu; p[2] = 0; p[3] = 64u;
    p[4] = PK_MAGIC0; p[5] = PK_MAGIC1; p[6] = PK_VER; p[7] = kind | len;
    uint8_t ks[16]; uint64_t enc[2], mac[2];
    deriveSession(G.id.localId, enc, mac);
    for(uint8_t i = 0; i < MSG_MAX; i++){
        if((i & 15u) == 0) ctrV2(enc, p + 16, (uint8_t)(i >> 4), ks);
        p[24 + i] ^= ks[i & 15u];
    }
    uint8_t tag[TAG_LEN];
    cbcmac(mac, p + 4, 64, tag);
    for(uint8_t i = 0; i < TAG_LEN; i++) p[56 + i] = tag[i];
    uint16_t c = crc16(p + 2, 66);
    p[68] = (uint8_t)(c >> 8); p[69] = (uint8_t)c;
    p[70] = 0xBAu; p[71] = 0xDCu;
}

static void buildPacket(const uint8_t counter[8]){
    uint8_t *p = pbytes();
    for(uint8_t i = 0; i < 72; i++) p[i] = 0;
    for(uint8_t i = 0; i < 8; i++){ p[8 + i] = G.id.localId[i]; p[16 + i] = counter[i]; }
    for(uint8_t i = 0; i < MSG_MAX; i++) p[24 + i] = (i < G.msgLen) ? (uint8_t)G.msg[i] : 0u;
    finishPacket(0u, G.msgLen);
}

static bool buildAck(void){
    uint8_t *p = pbytes(), next[8];
    copy8(next, G.id.txNext); if(!inc8(next)) return false;
    for(uint8_t i = 0; i < 16u; i++) p[24 + i] = p[8 + i];
    copy8(G.id.txNext, next);
    for(uint8_t i = 0; i < 8u; i++){ p[8 + i] = G.id.localId[i]; p[16 + i] = next[i]; }
    for(uint8_t i = 40u; i < 72u; i++) p[i] = 0;
    finishPacket(PK_KIND_ACK, 16u);
    return true;
}

static bool ackCounter(const uint8_t *counter){
    if(!G.ackPending) return false;
    /* sendMessage/buildAck leave txNext == the counter just put on air. */
    return same8(G.id.txNext, counter);
}

static void sendAck(void){
    if(A->tx_state() || !buildAck()) return;
    saveConfig();
    G.flags &= (uint8_t)~F_RXON;
    fskSend();
    fskRxArm();
}

/* Returns 1 = good message, 2 = receipt, 0 = ignore/replay, -1 = crypto fail. */
static int8_t parsePacket(void){
    uint8_t *p = pbytes();
    if(p[0] != 0xCDu || p[1] != 0xABu || p[2] != 0u || p[3] != 64u ||
       p[70] != 0xBAu || p[71] != 0xDCu) return 0;
    uint16_t c = ((uint16_t)p[68] << 8) | p[69];
    if(c != crc16(p + 2, 66)) return 0;
    if(p[4] != PK_MAGIC0 || p[5] != PK_MAGIC1 || p[6] != PK_VER) return 0;
    uint8_t kind = p[7] & PK_KIND_MASK, mlen = p[7] & (uint8_t)~PK_KIND_MASK;
    if(kind == PK_KIND_ACK) mlen = 16u;
    else if(kind != 0u || mlen < 1u || mlen > MSG_MAX) return 0;
    uint8_t tag[TAG_LEN], got[TAG_LEN], diff = 0;
    uint64_t enc[2], mac[2]; deriveSession(p + 8, enc, mac);
    for(uint8_t i = 0; i < TAG_LEN; i++){ got[i] = p[56 + i]; p[56 + i] = 0; }
    cbcmac(mac, p + 4, 64, tag);
    for(uint8_t i = 0; i < TAG_LEN; i++) diff |= (uint8_t)(tag[i] ^ got[i]);
    if(diff) return -1;
    /* Decrypt in place: plaintext stays at p[24..] so the sender ID and
     * counter at p[8..23] survive for the display and for the ACK echo. */
    uint8_t ks[16];
    for(uint8_t i = 0; i < MSG_MAX; i++){
        if((i & 15u) == 0) ctrV2(enc, p + 16, (uint8_t)(i >> 4), ks);
        uint8_t ch = (uint8_t)(p[24 + i] ^ ks[i & 15u]);
        if(i < mlen){
            if(kind != PK_KIND_ACK && (ch < 0x20u || ch > 0x7Eu)) return 0;
            p[24 + i] = ch;
        } else if(ch) return 0;
    }
    p[24 + mlen] = 0;
    if(kind == PK_KIND_ACK){
        if(!ackCounter(p + 32) || !same8(G.id.localId, p + 24)) return 0;
        if(!acceptCounter(p + 8, p + 16)) return 0;
        return 2;
    }
    if(!acceptCounter(p + 8, p + 16)) return 0;
    return 1;
}

/* ---- message history: received ID + bold text, sent receipt state ---- */
static void histNew(bool rx){
    if(G.hn >= HIST_N){
        /* Entries are contiguous and byte-packed; shift them as raw bytes. */
        uint8_t *h = (uint8_t *)G.hist;
        for(uint8_t j = 0; j < (uint8_t)((HIST_N - 1u) * sizeof(hist_entry_t)); j++)
            h[j] = h[j + sizeof(hist_entry_t)];
        G.hn = HIST_N - 1;
    }
    G.hist[G.hn].meta = rx ? H_RX : 0u;
    if(rx){ G.hist[G.hn].len = 0u; }        /* "<id>:<text>" is built by histSetId */
    else { G.hist[G.hn].text[0] = 'O'; G.hist[G.hn].text[1] = ' '; G.hist[G.hn].len = 2u; }
    G.hn++;
}
static void histApp(const char *s){
    hist_entry_t *h = &G.hist[G.hn - 1u];
    while(*s && h->len < TEXT_MAX) h->text[h->len++] = *s++;
}
static void histSetId(const uint8_t *id){
    hist_entry_t *h = &G.hist[G.hn - 1u];
    uint8_t n = idLenOf(id);
    for(uint8_t i = 0; i < n && h->len < TEXT_MAX; i++) h->text[h->len++] = (char)id[i];
    if(h->len < TEXT_MAX) h->text[h->len++] = ':';
}
static void histAck(char state){
    for(uint8_t i = 0; i < G.hn; i++)
        if(!(G.hist[i].meta & H_RX) && G.hist[i].text[0] == 'O'){
            G.hist[i].text[0] = state; return;
        }
}
static uint8_t msgLines(uint8_t i){ return (uint8_t)((G.hist[i].len + 15u) >> 4); }
static uint8_t boxTotalLines(void){
    uint8_t t = 0;
    for(uint8_t i = 0; i < G.hn; i++) t += msgLines(i);
    return t;
}
/* How far back the view can go. Shared by the box (which clamps to it) and by
 * the UP/DOWN handler, so the two can never disagree. */
static uint8_t boxMaxScroll(void){
    uint8_t total = boxTotalLines();
    return (uint8_t)(total > BOX_LINES ? total - BOX_LINES : 0);
}
static void drawBoxLine(uint8_t i, uint8_t k, uint8_t row){
    hist_entry_t *h = &G.hist[i]; char b[BOX_CH + 1];
    uint8_t off = (uint8_t)(k * BOX_CH), c = 0;
    for(uint8_t j = 0; j < BOX_CH && (uint8_t)(off + j) < h->len; j++) b[c++] = h->text[off + j];
    b[c] = 0;
    if(h->meta & H_RX) A->print_bold(b, 0, 0, row);
    else A->print_normal(b, 0, 0, row);
}
static void drawBox(void){
    uint8_t total = boxTotalLines();
    uint8_t maxScroll = (uint8_t)(total > BOX_LINES ? total - BOX_LINES : 0);
    if(G.scroll > maxScroll) G.scroll = maxScroll;
    int16_t start = (int16_t)total - BOX_LINES - (int16_t)G.scroll;
    if(start < 0) start = 0;
    uint8_t lc = 0, r = 0;
    for(uint8_t i = 0; i < G.hn && r < BOX_LINES; i++){
        uint8_t ml = msgLines(i);
        for(uint8_t k = 0; k < ml; k++){
            if(lc >= (uint8_t)start){ drawBoxLine(i, k, (uint8_t)(BOX_TOP + r)); r++; }
            lc++;
            if(r >= BOX_LINES) break;
        }
    }
}

/* ---- display ---- */
/* Whole-row inversion, shared by the identity row, the KDF box and the compose
 * row. Inlined three times it costs 32 bytes more than one out-of-line copy. */
__attribute__((noinline)) static void invertRow(uint8_t r){
    for(uint8_t x = 0; x < 128; x++) A->fb[r][x] ^= 0xFF;
}
static void blitAll(void){ A->blit_status(); A->blit_full(); }

static void chrome(void){
    A->display_clear(); A->status_clear();
    /* Six characters of the 3x5 tiny font span x = 2..25, inside the box. */
    A->print_inverse("SECMSG", 2, 0, true, true, 26);
    A->draw_battery();
}

/* Identity row (line 0, inverted): the configured device ID, so you can tell
 * units apart at a glance. The F latch adds " F+" while it is armed. */
static void drawPreview(void){
    char b[10], *p = b;
    for(uint8_t j = 0; j < G.idLen; j++) *p++ = (char)G.id.localId[j];
    if(G.flags & F_FDOWN) p = pstr(p, " F+");
    *p = 0;
    A->print_normal(b, 0, 0, 0);
    invertRow(0);
}

/* Half-second value as "1.5" / "0.5" / "10.0"; shared by the ACK menu row and
 * the status row so the two can never disagree. */
static inline __attribute__((always_inline)) char *pHalf(char *p, uint8_t halfsec){
    p = pnum(p, (uint32_t)(halfsec >> 1), 1);
    *p++ = '.'; *p++ = (halfsec & 1u) ? '5' : '0';
    return p;
}

/* Bottom status row (line 6): the receipt interval (ACK, in half-second steps)
 * on the left and the frequency the App is working on, flush right. "ACK 1.5"
 * takes six cells and "145.07500" nine, so they never collide on 18 cells. */
static void drawStatus(void){
    char b[12], *p = b;
    p = pstr(p, "ACK ");
    p = pHalf(p, G.ackS);
    A->print_normal(b, 0, 0, ROW_ST);
    uint32_t hz = A->rx_freq(), whole = 0;
    while(hz >= 100000u){ hz -= 100000u; whole++; }
    p = b; p = pnum(p, whole, 1);
    *p++ = '.'; p = pnum(p, hz, 5);
    A->print_normal(b, (uint8_t)(126 - (p - b) * 7), 0, ROW_ST);
}

/* Settings: rows PIN / ID / ACK. UP/DOWN picks the row, MENU enters edit
 * ('*' cursor), then digits (PIN/ID) or UP/DOWN (ACK) change the value; MENU
 * stores it again, EXIT saves. PIN always shows plaintext. */
static void drawMenu(void){
    char b[19], *p;
    uint8_t ed = (G.flags & F_EDIT) ? 1u : 0u;
    /* PIN row */
    p = b; *p++ = (G.menu == 1) ? (ed ? '*' : '>') : ' ';
    p = pstr(p, "PIN ");
    for(uint8_t j = 0; j < G.pinLen; j++) *p++ = G.pin[j];
    *p = 0; A->print_normal(b, 2, 0, 1);
    /* ID row */
    p = b; *p++ = (G.menu == 2) ? (ed ? '*' : '>') : ' ';
    p = pstr(p, "ID ");
    for(uint8_t j = 0; j < G.idLen; j++) *p++ = (char)G.id.localId[j];
    *p = 0; A->print_normal(b, 2, 0, 2);
    /* ACK interval row, seconds with one decimal: ackS * 0.5 s */
    p = b; *p++ = (G.menu == 3) ? (ed ? '*' : '>') : ' ';
    p = pstr(p, "ACK ");
    p = pHalf(p, G.ackS);
    *p = 0; A->print_normal(b, 2, 0, 3);
    if(G.pinLen != PIN_LEN) A->print_normal("NEED 8 DIGITS", 2, 0, ROW_IN);
}

static void draw(void){
    char b[19], r[8], *q;
    chrome();
    drawPreview();
    if(G.menu){ drawMenu(); drawStatus(); blitAll(); return; }
    if(G.kdfLeft){                       /* key derivation: input box locked */
        uint32_t done = (uint32_t)KDF_ROUNDS - G.kdfLeft;
        A->print_normal("LOADING", 0, 0, ROW_IN);
        q = r; q = pnum(q, (done * 100u) >> 16, 1); *q++ = '%'; *q = 0;
        A->print_normal(r, (uint8_t)(126 - (q - r) * 7), 0, ROW_IN);
        invertRow(ROW_IN);
        drawStatus();
        return;
    }
    drawBox();
    uint8_t hint = (uint8_t)(MSG_MAX - G.msgLen);   /* msgLen is never > MSG_MAX */
    q = r; *q++ = '('; q = pnum(q, hint, 1); *q++ = ')'; *q = 0;
    uint8_t maxc = (uint8_t)(MSG_MAX - 4u);   /* "(18)" reserves four cells */
    uint8_t st = (G.msgLen > (uint8_t)(maxc - 1u)) ? (uint8_t)(G.msgLen - (maxc - 1u)) : 0;
    uint8_t c = 0;
    b[c++] = '>';
    for(uint8_t i = 0; i < (uint8_t)(maxc - 1) && (uint8_t)(st + i) < G.msgLen; i++) b[c++] = G.msg[st + i];
    if(G.blink) b[c++] = '_';
    b[c] = 0;
    if(G.popT){ const char *z = POPUPS[G.pop]; c = 0; while(*z) b[c++] = *z++; b[c] = 0; }
    A->print_normal(b, 0, 0, ROW_IN);
    A->print_normal(r, (uint8_t)(126 - (q - r) * 7), 0, ROW_IN);
    invertRow(ROW_IN);
    drawStatus();
}

static void drawAndBlit(void){ draw(); if(!G.menu) blitAll(); }

/* ---- config + v2 key schedule (48B at slot+0x40, API level 2) ---- */
static uint32_t mix32(uint32_t h, uint8_t v){
    h ^= v; h *= 0x01000193u; h ^= h >> 16; return h;
}
__attribute__((noinline)) static void clearPeer(void){
    uint8_t *p = (uint8_t *)&G.id;
    for(uint8_t i = 16u; i < 40u; i++) p[i] = 0;
}
static void defaultId(void){
    G.id.localId[0] = 'S'; G.id.localId[1] = 'E'; G.id.localId[2] = 'C';
    G.id.localId[3] = 'M'; G.id.localId[4] = 'S'; G.id.localId[5] = 'G';
    G.idLen = ID_MAX;
}
static void initIdentity(void){
    uint8_t uid[12]; A->uid_get(uid);
    /* Fold the UID into one chain; only the two suffix bytes survive defaultId,
     * and the same value seeds the send counter. */
    uint32_t a = 0x811C9DC5u;
    for(uint8_t i = 0; i < 12; i++) a = mix32(a, uid[i]);
    if(!a) a = 1;
    defaultId();                              /* fills localId[0..5] */
    G.id.localId[6] = (uint8_t)(a >> 8);
    G.id.localId[7] = (uint8_t)a;
    st64be(G.id.txNext, ((uint64_t)a << 32) | (a + 1u));
    clearPeer();
}
static uint32_t pinValue(void){
    uint32_t v = 0; for(uint8_t i = 0; i < PIN_LEN; i++) v = v * 10u + (uint32_t)(G.pin[i] - '0');
    return v;
}
static void unpackPin(uint32_t v){
    if(v >= 100000000u){ G.pinLen = 0; return; }
    for(int8_t i = PIN_LEN - 1; i >= 0; i--){
        uint8_t r; v = div10(v, &r); G.pin[i] = (char)('0' + r);
    }
    G.pinLen = PIN_LEN;
}
static void loadConfig(void){
    uint8_t c[CFG_SIZE]; A->cfg_load(c, CFG_SIZE);
    /* Accept both the current format and the pre-interval one (c[1]==2), so an
     * upgrade keeps the PIN and device identity. */
    if(c[0] == CFG_MAGIC && (c[1] == CFG_VER || c[1] == 2u) &&
              crc16(c, CFG_CRC) == (((uint16_t)c[CFG_CRC] << 8) | c[CFG_CRC + 1u])){
        uint32_t pin = ((uint32_t)c[2] << 24) | ((uint32_t)c[3] << 16) | ((uint32_t)c[4] << 8) | c[5];
        if((pin & 0xF8000000u) == 0x70000000u) unpackPin(pin & 0x07FFFFFFu);
        uint8_t *state = (uint8_t *)&G.id;
        for(uint8_t i = 0; i < 40u; i++) state[i] = c[6 + i];
        if(zero8(G.id.localId)) initIdentity();
        else G.idLen = idLenOf(G.id.localId);
        if(!zero8(G.id.peerId) && zero8(G.id.rxBits)){ G.id.rxBits[7] = 1; }
        if(c[1] == CFG_VER && c[48] >= ACK_MIN && c[48] <= ACK_MAX) G.ackS = c[48];
    } else {
        G.pinLen = 0; initIdentity();
    }
}
static void saveConfig(void){
    /* Every byte below is assigned before cfg_save; no zero-fill is needed. */
    uint8_t c[CFG_SIZE];
    c[0] = CFG_MAGIC; c[1] = CFG_VER;
    uint32_t pin = (G.pinLen == PIN_LEN) ? (0x0Eu << 27) | pinValue() : 0;
    c[2] = (uint8_t)(pin >> 24); c[3] = (uint8_t)(pin >> 16); c[4] = (uint8_t)(pin >> 8); c[5] = (uint8_t)pin;
    const uint8_t *state = (const uint8_t *)&G.id;
    for(uint8_t i = 0; i < 40u; i++) c[6 + i] = state[i];
    c[48] = G.ackS;
    uint16_t sum = crc16(c, CFG_CRC);
    c[CFG_CRC] = (uint8_t)(sum >> 8); c[CFG_CRC + 1u] = (uint8_t)sum;
    A->cfg_save(c, CFG_SIZE);
}
static void clearKeys(void){
    G.key[0] = G.key[1] = 0;
    G.kdfLeft = 0;
}
static void kdfStart(void){
    clearKeys();
    if(G.pinLen != PIN_LEN || zero8(G.id.localId)) return;
    G.key[0] = KDF_SALT0;
    G.key[1] = (uint64_t)KDF_ROUNDS;
    G.kdfLeft = KDF_ROUNDS;
}
static void kdfStep(void){
    uint8_t n = 192;
    uint64_t p0 = KDF_LABEL0, p1 = ld64be((const uint8_t *)G.pin);
    while(n-- && G.kdfLeft){
        uint64_t in[2], out[2]; in[0] = G.key[0] ^ p0; in[1] = G.key[1] ^ p1;
        speck128_enc(G.key, in, out); G.key[0] = out[0]; G.key[1] = out[1]; G.kdfLeft--;
    }
    /* G.key is the shared PIN-KDF base; session subkeys are derived per sender. */
}

/* ---- input ---- */
static void cancelMultitap(void){ G.lastKey = APP_KEY_INVALID; G.tapMs = 0; }
static void clearFState(void){
    G.flags &= (uint8_t)~F_FDOWN; G.fTapMs = 0; G.fHoldMs = 0;
    cancelMultitap();
}

static void tapStep(uint8_t key, uint8_t *buf, uint8_t *len, uint8_t max){
    const char *set = KEYMAP[key];
    if(key == G.lastKey && G.tapMs > 0 && *len > 0){
        G.tapIdx++; if(!set[G.tapIdx]) G.tapIdx = 0;
        buf[*len - 1u] = (uint8_t)set[G.tapIdx];
    } else if(*len < max){
        G.tapIdx = 0; buf[(*len)++] = (uint8_t)set[0];
    } else alert(1);
    G.lastKey = key; G.tapMs = TAP_TICKS;
}

static void openMenu(uint8_t m){ G.menu = m; G.flags &= (uint8_t)~F_EDIT; clearFState(); }

static void handleTap(uint8_t key){
    if(G.menu == 1){
        if(key == APP_KEY_STAR){ G.pinLen = 0; G.pinDirty = true; }
        else if(key <= APP_KEY_9 && G.pinLen < PIN_LEN){
            G.pin[G.pinLen++] = (char)('0' + key);
            G.pinDirty = true;
        }
        return;
    }
    if(G.menu == 3) return;         /* ACK row: value is changed with UP/DOWN */
    uint8_t *buf; uint8_t *len; uint8_t max;
    if(G.menu == 2){ buf = G.id.localId; len = &G.idLen; max = ID_MAX; }
    else { buf = (uint8_t *)G.msg; len = &G.msgLen; max = MSG_MAX; }
    if(key == APP_KEY_0){
        if(*len > 0u) buf[--*len] = 0;
        cancelMultitap();
    } else if(key == APP_KEY_STAR){
        if(*len < max) buf[(*len)++] = ' '; else alert(1);
        cancelMultitap();
    } else if(key <= APP_KEY_9){
        tapStep(key, buf, len, max);
    }
}

static void sendMessage(void){
    if(G.msgLen == 0 || G.kdfLeft || G.menu || G.pinLen != PIN_LEN || G.ackPending){ alert(0); return; }
    if(A->tx_state()){ alert(2); return; }
    uint8_t counter[8] __attribute__((aligned(4))); copy8(counter, G.id.txNext);
    if(!inc8(counter)){ alert(1); return; }  /* never reuse a counter at 2^64-1 */
    G.flags &= ~F_RXON;
    G.ackWait = 0;                /* the operator owns the channel now */
    buildPacket(counter);
    copy8(G.id.txNext, counter);
    saveConfig();                 /* staged atomically; committed when app exits */
    histNew(false);
    G.msg[G.msgLen] = 0;
    histApp(G.msg);
    G.ackPending = 1;
    G.ackT = (uint16_t)(ACK_UNIT * G.ackS + 300u);   /* interval + 3 s margin */
    G.scroll = 0;
    fskSend();
    G.msgLen = 0;      /* clear the edit box after sending */
    fskRxArm();
    drawAndBlit();
}

static void drainKeys(void){
    uint8_t n = 50;   /* bound: a stuck key must not wedge the loop */
    while(n-- && A->get_key() != APP_KEY_INVALID) dly(10);
    G.prevKey = APP_KEY_INVALID;
}

/* Input is locked while the key is deriving, while a frame is being received
 * or its receipt is still pending. Re-evaluated on every key, so a lock can
 * never leak past the first press. */
static bool inputLocked(void){
    return G.kdfLeft != 0u || G.ackWait != 0u || G.fIdx != 0u;
}

static bool closeMenu(void){
    /* Do not let a 1-7 digit edit overwrite a working PIN. */
    if(G.pinLen != PIN_LEN) return false;
    G.menu = 0;
    G.flags &= (uint8_t)~F_EDIT;
    clearFState();
    if(G.pinDirty){ kdfStart(); G.pinDirty = false; }
    saveConfig();
    fskRxArm();
    return true;
}

/* Settings-menu keys: MENU enters/stores the edit, UP/DOWN picks the row (or
 * changes the ACK interval while editing), digits edit PIN/ID in edit mode. */
static void menuKey(uint8_t key){
    uint8_t ed = (G.flags & F_EDIT) ? 1u : 0u;
    if(key == APP_KEY_MENU){
        cancelMultitap();
        if(ed) G.flags &= (uint8_t)~F_EDIT; else G.flags |= F_EDIT;
        return;
    }
    int8_t dir = A->nav_dir(key);
    if(!ed){                               /* UP/DOWN picks the row */
        if(dir > 0){ if(G.menu > 1u) G.menu--; }        /* UP: toward the first row */
        else if(dir < 0){ if(G.menu < 3u) G.menu++; }
    } else if(G.menu == 3u){               /* ACK interval: 0.5 s per step */
        if(dir > 0){ if(G.ackS < ACK_MAX) G.ackS++; }
        else if(dir < 0){ if(G.ackS > ACK_MIN) G.ackS--; }
    } else if(!dir){                       /* PIN/ID text entry */
        handleTap(key);
    }
}

/* Handle one completed (or truncated) FSK frame once the key/unlock checks are
 * done. Truncated frames are reported as a receipt 'N' or an inbound 'N'. */
__attribute__((noinline)) static void rxPacket(int8_t got){
    if(got < 0){
        if(G.ackPending){ G.ackPending = 0; G.ackT = 0; histAck('N'); }
        else { histNew(true); histApp("N"); }
        drawAndBlit();
        return;
    }
    int8_t r = parsePacket();
    if(r == 1){
        A->led(true); dly(60); A->led(false);
        histNew(true);
        histSetId(pbytes() + 8);
        histApp((const char *)(pbytes() + 24));
        G.ackWait = (uint16_t)(ACK_UNIT * G.ackS);   /* receipt interval */
    } else if(r == 2){
        G.ackPending = 0; G.ackT = 0; histAck('V');
    }
    if(r == 1 || r == 2) saveConfig();
    drawAndBlit();
}

__attribute__((noinline)) static void holdTick(void){
    uint8_t key = A->get_key();
    if(G.menu){
        G.fHoldMs = 0;                 /* F actions belong to the home screen only */
    } else if(key == APP_KEY_F){
        if(G.fHoldMs < HOLD_TICKS + 1u) G.fHoldMs++;
    } else {
        if(G.fHoldMs > 0){
            if(G.fHoldMs < HOLD_TICKS){
                if(G.fTapMs > 0){
                    /* double-tap F: clear input + message box */
                    G.msgLen = 0;
                    G.hn = 0; G.scroll = 0;
                    clearFState();
                } else {
                    cancelMultitap();
                    G.flags ^= F_FDOWN; G.fTapMs = FTAP_TICKS;
                }
                drawAndBlit();
            }
            G.fHoldMs = 0;
        }
    }
    if(key == APP_KEY_PTT){
        if(G.pttHoldMs < HOLD_TICKS + 1u) G.pttHoldMs++;
    } else {
        if(G.pttHoldMs > 0 && G.pttHoldMs < HOLD_TICKS && !G.menu && !inputLocked()){
            cancelMultitap(); sendMessage(); drainKeys(); drawAndBlit();
        }
        G.pttHoldMs = 0;
    }
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api){
    uint8_t rxAcc = 0;
    A = api;

    /* The resident loader clears the complete overlay before copying .text;
       therefore the NOLOAD state below is already zero on entry. */
    G.lastKey = APP_KEY_INVALID;
    G.prevKey = APP_KEY_INVALID;
    G.flags = F_RUN;
    G.ackS = ACK_DEF;

    loadConfig();
    kdfStart();   /* background: UI instant even with saved PIN */
    A->set_agc(false);
    fskSetup();
    A->audio_path(true);      /* speaker path live; AF stays muted until data */
    A->backlight_on();
    fskRxArm();
    drawAndBlit();

    while(G.flags & F_RUN){
        uint8_t key = A->get_key();
        if(key == APP_KEY_SAVER){
            G.prevKey = APP_KEY_INVALID;
            dly(LOOP_MS);
            continue;                 /* let the resident screen saver own the LCD */
        }

        if(key == APP_KEY_EXIT && key != G.prevKey){
            if(G.menu){
                closeMenu(); G.prevKey = key; drawAndBlit(); continue;
            }
            if(G.exArm){ G.flags &= ~F_RUN; continue; }   /* second press quits */
            G.exArm = EXIT_TICKS; alert(3); G.prevKey = key; drawAndBlit(); continue;
        }

        uint8_t lk = inputLocked() ? 1u : 0u;
        if(!lk && (G.flags & F_FDOWN) && key != G.prevKey && key != APP_KEY_INVALID && key != APP_KEY_F && !G.menu){
            /* F latch: F+0 opens settings; F+1..9 appends a prosign to the message */
            G.flags &= (uint8_t)~F_FDOWN; G.fTapMs = 0; G.prevKey = key; A->backlight_on();
            cancelMultitap();
            if(key == APP_KEY_0) openMenu(1);
            else if(key <= APP_KEY_9){          /* 4/5/7/8 map to an empty prosign */
                const char *s = PROSIGN_ALL + PROSIGN_OFF[key];
                while(*s && G.msgLen < MSG_MAX) G.msg[G.msgLen++] = *s++;
            }
            drawAndBlit();
        } else if(!lk && key != APP_KEY_INVALID && key != G.prevKey && key != APP_KEY_F && key != APP_KEY_PTT){
            G.prevKey = key; A->backlight_on();
            if(G.menu) menuKey(key);
            else if(key == APP_KEY_MENU){ cancelMultitap(); openMenu(1); }
            else {
                int8_t dir = A->nav_dir(key);
                if(dir){
                    cancelMultitap();
                    uint8_t maxScroll = boxMaxScroll();
                    if(dir > 0){ if(G.scroll < maxScroll) G.scroll++; }   /* UP: older */
                    else if(G.scroll > 0) G.scroll--;                     /* DOWN: newer */
                }
                else handleTap(key);
            }
            drawAndBlit();
        } else if(key != APP_KEY_F && key != APP_KEY_PTT){
            G.prevKey = key;
        }
        holdTick();

        if(G.tapMs && !--G.tapMs) G.lastKey = APP_KEY_INVALID;
        if(G.fTapMs) --G.fTapMs;
        if(G.exArm) G.exArm--;                     /* EXIT arm window decays */

        /* Always drain/re-arm the FSK FIFO. Packets arriving while the key is
         * unavailable (KDF, menu) are deliberately discarded. */
        if(G.fIdx && ++G.fAge >= RX_TIMEOUT) fskRxArm();
        int8_t got = fskPoll();
        if(G.kdfLeft){ kdfStep(); drawAndBlit(); }
        else if(got && G.pinLen == PIN_LEN && !G.menu) rxPacket(got);
        if(G.ackPending && G.ackT && --G.ackT == 0){
            G.ackPending = 0; histAck('X'); drawAndBlit();
        }
        if(G.ackWait && !--G.ackWait) sendAck();   /* reply once the air is clear */

        /* Audible data bursts: open the AF while FSK activity lasts, mute it
         * again shortly after, so the idle channel stays silent (no hiss). */
        if(G.afT){
            A->set_af(APP_AF_FM);             /* heard: re-assert over tones_off_rx */
            if(!--G.afT) A->set_af(APP_AF_MUTE);
        }

        if(G.popT && --G.popT == 0){ G.pop = 0; drawAndBlit(); }
        if(++rxAcc >= BLINK_TICKS){ rxAcc = 0; G.blink ^= 1; drawAndBlit(); }

        if(++G.batTimer >= BAT_TICKS){ G.batTimer = 0; A->battery_sample(); }
        A->backlight_update();
        dly(LOOP_MS);   /* keep key/tap/KDF timers in real milliseconds */
    }

    saveConfig();
    fskReset();                 /* disable FSK IRQ/RX before returning to resident */
    bw(0x70u, 0u);
    A->set_agc(true);
    A->audio_path(false);
    A->led(false);
    A->tx_end();
}
