/* SPDX-License-Identifier: GPL-3.0-only */
/* GRANA in a browser: the FM-1 the app sees, in WebAssembly. platform.c includes this file instead of the
 * hardware when GRANA_WEB is set; the page (web/index.html) runs it in an AudioWorklet and draws the panel.
 * The app (firmware/src/grana.c) is the same source that goes on the FM-1. */
#include <stdint.h>
#define WEB_EXPORT(name) __attribute__((export_name(name)))
static int32_t web_batt = 600;                  /* 1.8: the simulator's battery, full */
#define BATT_RAW() web_batt
#define BATT_USB() 0

#ifndef APP_NAME
#define APP_NAME "GRANA"
#endif
#ifndef APP_VER
#define APP_VER "web"
#endif
#define FS 44100u
#define HALF_FRAMES 256u
#define FM1_TICKS_PER_US 24u

void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static volatile uint32_t fm1_ms;
static uint32_t fm1_ticks(void) { return 0; }
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
static struct { uint32_t notes, buttons; } fm1_in;
static uint32_t web_edges, web_rel;
static int32_t web_enc_d[7];
static uint8_t web_led[64];
static uint32_t fm1_input_edges(uint32_t *released) { uint32_t b = web_edges; web_edges = 0; *released = web_rel; web_rel = 0; return b; }
static uint32_t fm1_input_note_edges(void) { return 0; }
static int32_t fm1_enc_take(uint32_t e) { int32_t d = web_enc_d[e % 7u]; web_enc_d[e % 7u] = 0; return d; }
static uint8_t web_led_bg[64];
static volatile uint16_t fm1_led_bg_ns;
static void fm1_led_key(uint32_t id, int on) { if (id < 64u) web_led[id] = (uint8_t)(on ? 2 : web_led_bg[id]); }
static void fm1_led_back(uint32_t id, int on) { if (id < 64u) { web_led_bg[id] = (uint8_t)(on ? 1 : 0); if (web_led[id] != 2u) web_led[id] = web_led_bg[id]; } }

/* the screen: 240 x 240 RGB565 */
static uint16_t fb[240 * 240];
static uint32_t fb_gen;
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
    fb_gen++;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
            }
    fb_gen++;
}
#define CV_MAX (240u * 88u)
#include "gfx.c"

/* the settings sector and the user area (the presets): the page keeps them in the browser */
#define USER_LEN 0x4000u
static uint8_t web_store[4 + 2048 + USER_LEN];   /* [valid][len lo][len hi][saves], the settings, the user area */
static int settings_load(void *p, uint32_t n)
{
    uint32_t i;
    if (!(web_store[0] & 1u) || n > 2048u || (web_store[1] | (uint32_t)web_store[2] << 8) != n) return -1;
    for (i = 0; i < n; i++) ((uint8_t *)p)[i] = web_store[4 + i];
    return 0;
}
static int settings_save(const void *p, uint32_t n)
{
    uint32_t i;
    if (n > 2048u) return -1;
    for (i = 0; i < n; i++) web_store[4 + i] = ((const uint8_t *)p)[i];
    web_store[0] |= 1u;
    web_store[1] = (uint8_t)n;
    web_store[2] = (uint8_t)(n >> 8);
    web_store[3]++;
    return 0;
}

static uint8_t *const web_user = web_store + 4 + 2048;
static int user_read(uint32_t off, void *p, uint32_t n)
{
    uint32_t i;
    if (off > USER_LEN || n > USER_LEN - off) return -1;
    if (!(web_store[0] & 2u)) { for (i = 0; i < USER_LEN; i++) web_user[i] = 0xFF; web_store[0] |= 2u; }
    for (i = 0; i < n; i++) ((uint8_t *)p)[i] = web_user[off + i];
    return 0;
}
static int user_erase(uint32_t off)
{
    uint32_t i;
    if ((off & 0xFFFu) || off >= USER_LEN) return -1;
    for (i = 0; i < 4096u; i++) web_user[off + i] = 0xFF;
    web_store[0] |= 2u;
    web_store[3]++;
    return 0;
}
static int user_write(uint32_t off, const void *p, uint32_t n)
{
    uint32_t i;
    if (off > USER_LEN || n > USER_LEN - off) return -1;
    for (i = 0; i < n; i++) web_user[off + i] &= ((const uint8_t *)p)[i];
    web_store[3]++;
    return 0;
}

/* 1.6: the user bank (the FM-1's flash after the presets), as the editor sends it; read by bank_open */
#define UB_LEN 0x45000u
static uint8_t web_ubank[UB_LEN];
static uint8_t web_ub_fresh = 1;            /* the flash as it comes: erased */
static void web_ub_init(void) { uint32_t i; if (web_ub_fresh) { for (i = 0; i < UB_LEN; i++) web_ubank[i] = 0xFF; web_ub_fresh = 0; } }
static int ub_erase(uint32_t off)
{
    uint32_t i;
    if ((off & 0xFFFu) || off >= UB_LEN) return -1;
    web_ub_init();
    for (i = 0; i < 4096u; i++) web_ubank[off + i] = 0xFF;
    return 0;
}
static int ub_write(uint32_t off, const void *p, uint32_t n)
{
    uint32_t i;
    if (off > UB_LEN || n > UB_LEN - off) return -1;
    web_ub_init();
    for (i = 0; i < n; i++) web_ubank[off + i] &= ((const uint8_t *)p)[i];
    return 0;
}
static int flash_peek(uint32_t at, void *p, uint32_t n)   /* the free areas: erased, in the simulator */
{
    uint32_t i;
    if (at < 0x93000u || at > 0x100000u || n > 0x100000u - at) return -1;
    for (i = 0; i < n; i++) ((uint8_t *)p)[i] = 0xFF;
    return 0;
}

/* MIDI: Web MIDI in, nothing out */
static uint8_t mq[64][3];
static uint32_t mq_w, mq_r;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    if (mq_r == mq_w) return 0;
    *st = mq[mq_r % 64u][0]; *d1 = mq[mq_r % 64u][1]; *d2 = mq[mq_r % 64u][2];
    mq_r++;
    return 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { (void)st; (void)d1; (void)d2; }
static uint32_t web_clock_n;               /* the clock bytes the FM-1 would send (tests) */
static void midi_clock_out(uint32_t b) { if (b == 0xF8u) web_clock_n++; }
static uint8_t web_sx_out[48000], web_sx_in[1024];   /* SysEx to and from a test playing the computer's side */
static uint32_t web_sx_n;
static int sysex_send(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (web_sx_n + n > sizeof web_sx_out) return -1;
    for (i = 0; i < n; i++) web_sx_out[web_sx_n + i] = p[i];
    web_sx_n += n;
    return 0;
}
static void app_sysex(const uint8_t *p, uint32_t n);
static void web_presets_out(void);
static void web_presets_in(void);

static void app_audio(int32_t *out, uint32_t n);
static void app_setup(void);
static void app_step(void);
static uint8_t *web_bank_buf(void);
static uint32_t web_bank_size(void);
static void web_bank_changed(void);
static uint32_t web_state(void);

static int32_t web_abuf[2u * HALF_FRAMES];
static int32_t web_master = 32767;

WEB_EXPORT("init") void web_init(void) { app_setup(); }
WEB_EXPORT("step") void web_step(uint32_t ms) { fm1_ms = ms; app_step(); }
WEB_EXPORT("audio") int32_t *web_audio(void)
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_FRAMES; i++) web_abuf[i] = 0;
    app_audio(web_abuf, HALF_FRAMES);
    for (i = 0; i < 2u * HALF_FRAMES; i++) {    /* the MASTER knob, then the clip (as platform.c) */
        int32_t v = (int32_t)(((int64_t)web_abuf[i] * web_master) >> 15);
        web_abuf[i] = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
    }
    return web_abuf;
}
WEB_EXPORT("keys") void web_keys(uint32_t mask) { fm1_in.notes = mask; }
WEB_EXPORT("button") void web_button(uint32_t id, uint32_t down)
{
    if (id > 13u) return;
    if (down) { fm1_in.buttons |= 1u << id; web_edges |= 1u << id; }
    else { fm1_in.buttons &= ~(1u << id); web_rel |= 1u << id; }
}
WEB_EXPORT("enc") void web_enc(uint32_t id, int32_t d) { web_enc_d[id % 7u] += d; }
WEB_EXPORT("midi") void web_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    mq[mq_w % 64u][0] = (uint8_t)st; mq[mq_w % 64u][1] = (uint8_t)d1; mq[mq_w % 64u][2] = (uint8_t)d2;
    mq_w++;
}
WEB_EXPORT("master") void web_set_master(int32_t q15) { web_master = q15; }
WEB_EXPORT("fb") uint16_t *web_fb(void) { return fb; }
WEB_EXPORT("fb_gen") uint32_t web_fb_gen(void) { return fb_gen; }
WEB_EXPORT("leds") uint8_t *web_leds(void) { return web_led; }
WEB_EXPORT("store") uint8_t *web_store_ptr(void) { return web_store; }
WEB_EXPORT("store_len") uint32_t web_store_len(void) { return sizeof web_store; }
WEB_EXPORT("bank_buf") uint8_t *web_bank_ptr(void) { return web_bank_buf(); }
WEB_EXPORT("bank_size") uint32_t web_bank_cap(void) { return web_bank_size(); }
WEB_EXPORT("bank_changed") void web_bank_new(void) { web_bank_changed(); }
WEB_EXPORT("ubank_buf") uint8_t *web_ubank_ptr(void) { web_ub_fresh = 0; return web_ubank; }   /* the page writes the user bank here (1.6) */
WEB_EXPORT("ubank_size") uint32_t web_ubank_cap(void) { return UB_LEN; }
WEB_EXPORT("state") uint32_t web_get_state(void) { return web_state(); }
WEB_EXPORT("sx_in") uint8_t *web_sx_in_ptr(void) { return web_sx_in; }
WEB_EXPORT("sx_feed") void web_sx_feed(uint32_t n) { if (n > 3u && n <= sizeof web_sx_in) app_sysex(web_sx_in + 3, n - 3u); }   /* a frame without F0 F7 */
WEB_EXPORT("sx_out") uint8_t *web_sx_out_ptr(void) { return web_sx_out; }
WEB_EXPORT("sx_out_len") uint32_t web_sx_out_len(void) { uint32_t n = web_sx_n; web_sx_n = 0; return n; }
WEB_EXPORT("presets_out") void web_presets_out_x(void) { web_presets_out(); }   /* every preset into the user area (the factory ones too) */
WEB_EXPORT("presets_in") void web_presets_in_x(void) { web_presets_in(); }      /* the user area changed: read it again */
WEB_EXPORT("clock_n") uint32_t web_clock_count(void) { return web_clock_n; }
