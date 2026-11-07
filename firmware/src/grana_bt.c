/* SPDX-License-Identifier: GPL-3.0-only */
/* GRANA + Bluetooth (GRANA_BT=1, tools/build.py --bt): the FM-1 as a BLE-MIDI central (host), for one controller at a
 * time, and the M-VAVE SMC-Mixer in its DAW (Mackie Control) mode.
 *
 * The stack is Optimist's (w0ts/optimist, firmware/src/ble/, GPL-3.0-only OR MPL-2.0): written from the Bluetooth Core
 * and BLE-MIDI specifications, with its driver for the WL82 baseband engine and the radio's start-up captured from stock
 * V15; built here at BLE_CENTRAL 1 (1.4.1: the central pairs, Just Works, and keeps the bond, for a controller that
 * asks for it; 1.4 had level 3, without pairing). The way around it follows
 * fm1-dock's HX (joakimeriksson/fm1-dock, src/ble/hx.c) and Optimist's io/midi/midi_ble.c. The SMC-Mixer's messages and
 * its soft takeover follow Hortator's smc.c (DeadActive, GPL-3.0), checked there on a unit.
 *
 *   in    the stack decodes the controller's writes in the BLE interrupts into USB-MIDI packets: bt_in_q, read by the
 *         main loop (bt_step): MIDI mode as USB's and the TRS jack's (midi_in), SMC mode to smc_in
 *   out   only the SMC-Mixer's LEDs and fader positions (the main loop): bt_out_q, taken by the stack's notifications
 *   radio started the first time BLUETOOTH is ON (never before: no BLE / RF register written while OFF), and only with
 *         the unit's stored trims (stock's VM at flash 0xE6000.., or the copy kept in the settings); the boot guard
 *         keeps it off after a boot that failed
 *   link  the remembered controller is connected to whenever BLUETOOTH is ON and it is not linked: initiating for a
 *         while, then a pause, again; CERCA scans 10 s for the nearby BLE-MIDI devices (BLUETOOTH page, GLO) */
#define BLE_DEVICE_NAME "GRANA"
#define FELUCCA_BLE_CENTRAL 1
#define RING_PUBLISH_BT() __asm__ volatile("" ::: "memory")
static struct { uint8_t flat; } dualcfg;        /* (Optimist's second-core test switch: not here) */
#include "ble/ble_stack.c"
#include "ble/ble_vm.c"

#define BTQ 64u
static uint32_t bt_in_q[BTQ], bt_out_q[BTQ], bt_out_ms[BTQ];
static volatile uint32_t bti_w, bti_r, bto_w, bto_r;
static volatile uint8_t bt_out_on, bt_lost;
static uint8_t bt_was_conn, bt_up, bt_rf_src, bt_held_boot, bt_kept_new, bt_attached;
static struct ble_rf_trims bt_use;
static uint8_t bt_held[16][16];                 /* the notes the controller has on (bit per note, per channel) */
enum { BTS_OFF, BTS_NOCAL, BTS_HELD, BTS_IDLE, BTS_SCAN, BTS_CONNECTING, BTS_LINK, BTS_LOST };
static const char *const BT_STN[8] = {"SPENTO", "NO CALIBRAZIONE", "FERMO (AVVIO FALLITO)", "ACCESO", "CERCO...",
                                      "COLLEGO...", "COLLEGATO", "PERSO"};
static uint8_t bt_scan_on, bt_mode_live, bt_rescan, bt_rescan_at;   /* 1.4.3: a scan for the remembered name */
static uint32_t bt_in_n, bt_in_last;              /* 1.4.3: the messages received, the last (shown on the BT page) */
static uint32_t bt_scan_t0, bt_try_t0, bt_wait_t0, bt_seen_gen;
static struct ble_scan_tab bt_found;
static uint8_t bt_sel;                          /* DISPOSITIVO: the row the knob points at */
static uint8_t bt_name_cur[BLE_NAME_MAX + 1u];  /* the linked device's name, as known */
static uint8_t bt_fail, bt_fail_code, bt_tries, bt_was_cs;   /* 1.4.1: why the last attempt failed, and how many */
static volatile uint8_t bt_keys_new;
static struct ble_keys bt_keys;                 /* a pairing's keys (the BLE interrupts'), kept by the main loop */
enum { BTF_NONE, BTF_TIMEOUT = 0xF0u };

BLE_API void ble_app_state(void)                /* the BLE interrupts, or the main loop with them held */
{
    uint8_t c = (uint8_t)ble_connected();
    bt_out_on = (uint8_t)ble_midi_ready();
    if (!bt_out_on) bto_r = bto_w;
    if (bt_was_conn && !c) bt_lost = 1;
    bt_was_conn = c;
}
BLE_API int ble_app_midi_peek(uint32_t *pkt, uint32_t *t_ms)
{
    if (bto_r == bto_w) return 0;
    RING_PUBLISH_BT();
    *pkt = bt_out_q[bto_r % BTQ];
    *t_ms = bt_out_ms[bto_r % BTQ];
    return 1;
}
BLE_API void ble_app_midi_pop(void) { RING_PUBLISH_BT(); bto_r++; }
BLE_API void ble_app_bond(const uint8_t rand[8], uint16_t ediv, const uint8_t ltk[16]) { (void)rand, (void)ediv, (void)ltk; }   /* (a computer bonding to GRANA: not kept) */
BLE_API void ble_app_peer_id(const uint8_t irk[16], const uint8_t addr[6], uint8_t addr_rand) { (void)irk, (void)addr, (void)addr_rand; }
BLE_API void ble_app_central_keys(const struct ble_keys *k)   /* the controller paired: its keys, for the main loop */
{
    bt_keys = *k;
    bt_keys_new = 1;
}
BLE_API void ble_app_midi_in(uint32_t pkt, uint16_t ts, uint16_t last_ts)
{
    uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu;
    (void)ts, (void)last_ts;
    if (!(cin >= 8u && cin <= 0xEu) && !(cin == 0xFu && (st == 0xF8u || (st >= 0xFAu && st <= 0xFCu)))) return;
    if (bti_w - bti_r >= BTQ) return;
    bt_in_q[bti_w % BTQ] = pkt;
    RING_PUBLISH_BT();
    bti_w++;
}
static int bt_send(uint32_t st, uint32_t d1, uint32_t d2)   /* the main loop: one message to the controller -> 1 queued */
{
    if (!bt_out_on || bto_w - bto_r >= BTQ - 2u) return 0;
    bt_out_q[bto_w % BTQ] = (st >> 4) | st << 8 | (d1 & 0x7Fu) << 16 | (d2 & 0x7Fu) << 24;
    bt_out_ms[bto_w % BTQ] = fm1_ms;
    RING_PUBLISH_BT();
    bto_w++;
    return 1;
}

#include "grana_smc.c"

static int bt_vm_rd(void *ctx, uint32_t off, uint8_t *dst, uint32_t n)   /* the flash as stored */
{
    (void)ctx;
#if defined(GRANA_WEB) || defined(DINGHY_HOST)
    (void)off, (void)dst, (void)n;
    return -1;
#else
    return flash_ok ? st_read(off, dst, n) : -1;
#endif
}

/* at boot (app_setup), the interrupts on: the trims chosen, the two vectors attached and masked. Nothing of the radio */
static void bt_boot(void)
{
    struct ble_vm_info info;
    struct ble_rf_trims vm;
    int complete, save;
    complete = ble_vm_scan(bt_vm_rd, 0, &info, &vm, 0, 0);
    bt_rf_src = (uint8_t)ble_rf_choose(complete, &vm, info.area, store.bt_kept + 8, &bt_use, &save);
    if (save) bt_kept_new = 1;
#if !defined(GRANA_WEB) && !defined(DINGHY_HOST)
    {
        uint32_t f = irq_save();
        fm1_guard_unlock_top();                 /* the vectors: the write limit over the table (fm1-dock, 2026-10-25) */
        ble_hw_wl82_attach();
        fm1_guard_lock_top();
        irq_restore(f);
    }
#endif
    bt_attached = 1;
    bt_held_boot = (uint8_t)(store.bt_auto && bt_rf_src != BLE_RF_NONE && !ble_boot_radio(1, bootguard.failed, 1));
}

static int bt_radio_start(void)                 /* the first ON: the radio, the baseband, the stack, advertising */
{
    uint8_t a[6], rnd;
    uint32_t i, f;
    if (bt_up) return 1;
    if (bt_rf_src == BLE_RF_NONE || bt_held_boot || !bt_attached) return 0;
    rnd = ble_hw_addr(a);
    if (rnd && store.bt_kept[6] == 0xA5u && (store.bt_kept[5] >> 6) == 3u) {
        for (i = 0; i < 6u; i++) a[i] = store.bt_kept[i];
    } else if (rnd) {
        for (i = 0; i < 6u; i++) store.bt_kept[i] = a[i];
        store.bt_kept[6] = 0xA5u;
        bt_kept_new = 1;
    }
    f = irq_save();
    ble_init(a, rnd);
    ble_rf_copy_get(store.bt_kept + 8, &bt_use);
    ble_hw_wl82_start(&bt_use);
    ble_enable(1);                              /* advertising too: a computer may connect to GRANA as well */
    irq_restore(f);
    fm1_ble_irqs_hold(0);
    bt_up = 1;
    return 1;
}

static uint8_t bt_on;                           /* BLUETOOTH, now (AVVIO: ON by itself at power-on) */
static uint32_t bt_state(void)
{
    uint8_t cs;
    if (!bt_on) return BTS_OFF;
    if (!bt_up) return bt_rf_src == BLE_RF_NONE ? BTS_NOCAL : BTS_HELD;
    if (ble_connected()) return BTS_LINK;
    if (bt_scan_on) return BTS_SCAN;
    cs = ble_central_state();
    if (cs == BLE_CS_CONNECTING || cs == BLE_CS_SETUP) return BTS_CONNECTING;
    return bt_lost ? BTS_LOST : BTS_IDLE;
}

static void bt_set(uint32_t on)
{
    on = on ? 1u : 0u;
    if (on == bt_on) return;
    bt_on = (uint8_t)on;
    if (on) { if (!bt_radio_start()) return; }
    if (!bt_up) return;
    fm1_ble_irqs_hold(1);
    if (!on) { ble_ll_scan(0); ble_central_cancel(); bt_scan_on = 0; }
    ble_enable((int)on);
    fm1_ble_irqs_hold(0);
    if (!on) bt_lost = 1;
}

static void bt_scan(int on)                     /* CERCA: 10 s of the nearby devices */
{
    if (!bt_up) return;
    fm1_ble_irqs_hold(1);
    if (on && ble_central_state() == BLE_CS_CONNECTING) ble_central_cancel();
    ble_ll_scan(on);
    fm1_ble_irqs_hold(0);
    bt_scan_on = (uint8_t)(on != 0);
    if (on) { ble_scan_clear(&bt_found); bt_scan_t0 = fm1_ms; }
}

static void bt_connect_to(const uint8_t a[6], uint8_t rnd)
{
    struct ble_peer p;
    uint32_t i;
    for (i = 0; i < sizeof p; i++) ((uint8_t *)&p)[i] = 0;
    for (i = 0; i < 6u; i++) p.addr[i] = a[i];
    p.addr_rand = rnd;
    if (0 && store.bt_bonded) {                 /* 1.4.4: not used - the SMC-Mixer refuses it; paired again each time, as CERCA. The bond it gave: the link encrypted with it, no pairing again */
        p.bonded = 1;
        for (i = 0; i < 16u; i++) p.ltk[i] = store.bt_ltk[i];
        for (i = 0; i < 8u; i++) p.rand[i] = store.bt_rand[i];
        p.ediv = (uint16_t)(store.bt_ediv[0] | store.bt_ediv[1] << 8);
    }
    fm1_ble_irqs_hold(1);
    (void)ble_central_connect(&p);
    bt_tries++;
    fm1_ble_irqs_hold(0);
    bt_try_t0 = fm1_ms ? fm1_ms : 1u;
}

/* the controller's notes still on when it goes: their note-offs */
static void bt_note_seen(uint32_t pkt)
{
    uint32_t st = (pkt >> 8) & 0xF0u, ch = (pkt >> 8) & 15u, n = (pkt >> 16) & 0x7Fu;
    if (st == 0x90u && ((pkt >> 24) & 0x7Fu)) bt_held[ch][n >> 3] |= (uint8_t)(1u << (n & 7u));
    else if (st == 0x80u || st == 0x90u) bt_held[ch][n >> 3] &= (uint8_t)~(1u << (n & 7u));
}
static void bt_notes_end(void)
{
    uint32_t ch, n;
    for (ch = 0; ch < 16u; ch++)
        for (n = 0; n < 128u; n++)
            if (bt_held[ch][n >> 3] & (1u << (n & 7u))) { note_off(n); bt_held[ch][n >> 3] &= (uint8_t)~(1u << (n & 7u)); }
}

static int bt_smc_now(void)                     /* MODO: AUTO (a name starting SMC), MIDI, SMC */
{
    if (store.bt_mode == 1u) return 0;
    if (store.bt_mode == 2u) return 1;
    {   /* 1.4.3: SMC anywhere in the name, any case */
        uint32_t i;
        for (i = 0; i + 2u < BLE_NAME_MAX && bt_name_cur[i + 2u]; i++)
            if ((bt_name_cur[i] | 0x20u) == 's' && (bt_name_cur[i + 1u] | 0x20u) == 'm' && (bt_name_cur[i + 2u] | 0x20u) == 'c') return 1;
        return 0;
    }
}

/* ---- the main loop */
static void bt_step(void)
{
    uint32_t st;
    if (!bt_on || !bt_up) return;
    while (bti_r != bti_w) {                    /* the controller's messages */
        uint32_t pkt;
        RING_PUBLISH_BT();
        pkt = bt_in_q[bti_r % BTQ];
        RING_PUBLISH_BT();
        bti_r++;
        bt_in_n++; bt_in_last = pkt;
        if (page == PG_BT) dirty_tabs = 1;
        if (bt_mode_live) smc_in(pkt);
        else { bt_note_seen(pkt); midi_in((pkt >> 8) & 0xFFu, (pkt >> 16) & 0x7Fu, (pkt >> 24) & 0x7Fu); }
    }
    if (bt_lost) {                              /* gone: its notes off, its SMC LEDs to send again next time */
        bt_lost = 0; bt_in_n = 0;
        bt_notes_end();
        smc.on = 0;
        if (lay_mute || lay_solo) { lay_mute = lay_solo = 0; engine_update(); }
        bt_wait_t0 = fm1_ms ? fm1_ms : 1u;
        dirty_tabs = 1;
        if (page == PG_BT) dirty_cells = 0xF;
    }
    if (bt_scan_on) {                           /* CERCA: the reports into the list, 10 s */
        uint8_t pdu[2 + 37];
        uint16_t rssi;
        uint32_t n;
        while ((n = ble_ll_scan_take(pdu, &rssi)) != 0u) ble_scan_add(&bt_found, pdu, n, rssi, fm1_ms);
        ble_scan_age(&bt_found, fm1_ms);
        if (bt_found.gen != bt_seen_gen) { bt_seen_gen = bt_found.gen; if (page == PG_BT) dirty_cells |= 2u; }
        if (bt_rescan && fm1_ms - bt_scan_t0 > 6000u) {   /* the remembered one by its name: its address now */
            uint8_t idx[BLE_SCAN_N];
            uint32_t nf = ble_scan_list(&bt_found, idx), i, j;
            bt_scan(0);
            bt_rescan = 0;
            for (i = 0; i < nf; i++) {
                const struct ble_found *f = &bt_found.e[idx[i]];
                for (j = 0; j <= BLE_NAME_MAX && f->name[j] == store.bt_dev_name[j] && f->name[j]; j++) {}
                if (store.bt_dev_name[0] && j <= BLE_NAME_MAX && f->name[j] == store.bt_dev_name[j]) {
                    for (j = 0; j < 6u; j++) store.bt_dev_addr[j] = f->addr[j];
                    store.bt_dev_rand = f->addr_rand;
                    settings_touch();
                    break;
                }
            }
            bt_wait_t0 = 0;
            dirty_tabs = 1;
        } else if (!bt_rescan && fm1_ms - bt_scan_t0 > 10000u) {
            uint8_t idx[BLE_SCAN_N];
            uint32_t nf = ble_scan_list(&bt_found, idx);
            char t[8];
            bt_scan(0);
            if (page == PG_BT) dirty_cells = 0xF;
            dirty_tabs = 1;
            if (!nf) learn_say("NESSUN DISPOSITIVO TROVATO", 0);
            else { fmt_u(t, nf); learn_say(nf == 1u ? "TROVATO 1: GIRA KNOB 2, POI OCT+" : "TROVATI: GIRA KNOB 2, POI OCT+", 0); (void)t; }
        }
    }
    st = bt_state();
    if (st == BTS_LINK) {
        uint8_t live = (uint8_t)bt_smc_now();
        if (live != bt_mode_live) { bt_mode_live = live; smc.on = 0; }
        if (bt_mode_live) { if (!smc.on) smc_fresh(); smc_frame(); }
        bt_try_t0 = 0;
    } else if (store.bt_dev_ok && !bt_scan_on) {   /* the remembered controller: initiating 8 s, a pause of 2 s, again */
        uint8_t cs = ble_central_state();
        if (cs == BLE_CS_CONNECTING && bt_try_t0 && fm1_ms - bt_try_t0 > 8000u) {
            fm1_ble_irqs_hold(1); ble_central_cancel(); fm1_ble_irqs_hold(0);
            bt_try_t0 = 0; bt_wait_t0 = fm1_ms ? fm1_ms : 1u;
            bt_fail = BTF_TIMEOUT; bt_fail_code = 0; dirty_tabs = 1;
        } else if (cs == BLE_CS_IDLE && bt_was_cs != BLE_CS_IDLE) {   /* an attempt ended without a link: why */
            uint8_t f = ble_central_fail();
            if (f != BLE_CF_NONE || bt_fail != BTF_TIMEOUT) { bt_fail = f ? f : BLE_CF_LOST; bt_fail_code = ble_central_code(); }
            if (f == BLE_CF_KEY && store.bt_bonded) { store.bt_bonded = 0; settings_touch(); }   /* the bond is no good to it: pair again */
            bt_wait_t0 = fm1_ms ? fm1_ms : 1u;
            dirty_tabs = 1;
        } else if (cs == BLE_CS_IDLE && bt_tries >= 3u && bt_tries != bt_rescan_at && (!bt_wait_t0 || fm1_ms - bt_wait_t0 > 2000u)) {
            bt_rescan_at = bt_tries;            /* 1.4.3: not answering where it was: look for its name, 6 s */
            bt_rescan = 1;
            bt_scan(1);
            dirty_tabs = 1;
        } else if (cs == BLE_CS_IDLE && (!bt_wait_t0 || fm1_ms - bt_wait_t0 > 2000u)) {
            uint32_t i;
            for (i = 0; i <= BLE_NAME_MAX; i++) bt_name_cur[i] = (uint8_t)store.bt_dev_name[i];
            bt_connect_to(store.bt_dev_addr, store.bt_dev_rand);
            bt_wait_t0 = 0;
        }
        bt_was_cs = ble_central_state();
    }
    if (st == BTS_LINK && bt_tries) { bt_tries = 0; bt_fail = BTF_NONE; dirty_tabs = 1; }
    if (bt_keys_new) {                          /* the controller paired: its bond kept for the next times */
        uint32_t i;
        bt_keys_new = 0;
        if (bt_keys.has & BLE_KEYS_LTK) {
            for (i = 0; i < 16u; i++) store.bt_ltk[i] = bt_keys.ltk[i];
            for (i = 0; i < 8u; i++) store.bt_rand[i] = bt_keys.rand[i];
            store.bt_ediv[0] = (uint8_t)bt_keys.ediv; store.bt_ediv[1] = (uint8_t)(bt_keys.ediv >> 8);
            store.bt_bonded = 1;
            settings_touch();
        }
    }
    {   /* the strip and the header's mark follow the state */
        static uint32_t was = 0xFFu;
        if (st != was) { was = st; dirty_tabs = 1; dirty_hdr = 1; if (page == PG_BT) dirty_cells = 0xF; }
    }
}

/* ---- the BLUETOOTH page (GLO, after MIDI): KNOB 1 BLUETOOTH, 2 DISPOSITIVO (the remembered one, those found, CERCA,
 * DIMENTICA; OCT+ acts on it), 3 MODO, 4 AVVIO */
static const char *const BT_PN[4] = {"BLUETOOTH", "DISPOSITIVO", "MODO", "AVVIO"};
static const char *const BT_MODEN[3] = {"AUTO", "MIDI", "SMC"};
static uint32_t bt_rows(uint8_t idx[BLE_SCAN_N])   /* the found devices shown, then CERCA, DIMENTICA */
{
    return ble_scan_list(&bt_found, idx);
}
static void bt_row_text(uint32_t r, char *s)
{
    uint8_t idx[BLE_SCAN_N];
    uint32_t n = bt_rows(idx), i;
    if (r == 0u) {                              /* the remembered one */
        if (!store.bt_dev_ok) { cat(s, "NESSUNO"); return; }
        for (i = 0; i < BLE_NAME_MAX && store.bt_dev_name[i]; i++) s[i] = store.bt_dev_name[i];
        s[i] = 0;
        if (!i) cat(s, "RICORDATO");
        return;
    }
    if (r - 1u < n) {
        const struct ble_found *e = &bt_found.e[idx[r - 1u]];
        char *e2 = cat(s, e->midi ? "" : "? ");
        cat(e2, e->name[0] ? e->name : "SENZA NOME");
        return;
    }
    cat(s, r - 1u == n ? "CERCA" : "DIMENTICA");
}
static uint32_t bt_nrows(void) { uint8_t idx[BLE_SCAN_N]; return 1u + bt_rows(idx) + 2u; }
static void bt_knob(uint32_t c, int32_t d)
{
    if (c == 0u) bt_set(d > 0);
    else if (c == 1u) { int32_t r = (int32_t)bt_sel + (d > 0 ? 1 : -1), n = (int32_t)bt_nrows(); bt_sel = (uint8_t)(r < 0 ? 0 : r >= n ? n - 1 : r); }
    else if (c == 2u) { int32_t m = (int32_t)store.bt_mode + (d > 0 ? 1 : -1); store.bt_mode = (uint8_t)(m < 0 ? 0 : m > 2 ? 2 : m); settings_touch(); }
    else { store.bt_auto = d > 0; settings_touch(); }
    dirty_cells |= 1u << c;
    dirty_tabs = 1;
}
static void bt_confirm(void)                    /* OCT+ on the BLUETOOTH page: DISPOSITIVO's row */
{
    uint8_t idx[BLE_SCAN_N];
    uint32_t n = bt_rows(idx), i;
    if (!bt_on) { learn_say("BLUETOOTH: PRIMA ACCENDILO (KNOB 1)", 0); return; }
    if (!bt_up) { learn_say("BLUETOOTH: ", BT_STN[bt_state()]); return; }
    if (bt_sel == 0u) {                         /* the remembered one: now */
        if (store.bt_dev_ok && !ble_connected()) { bt_wait_t0 = 0; bt_try_t0 = 0; learn_say("COLLEGO...", 0); }
    } else if (bt_sel - 1u < n) {               /* a found one: remembered, connected to */
        const struct ble_found *e = &bt_found.e[idx[bt_sel - 1u]];
        for (i = 0; i < 6u; i++) store.bt_dev_addr[i] = e->addr[i];
        store.bt_dev_rand = e->addr_rand;
        for (i = 0; i <= BLE_NAME_MAX; i++) store.bt_dev_name[i] = e->name[i];
        store.bt_dev_ok = 1;
        store.bt_bonded = 0;                    /* a new one: no bond yet */
        bt_fail = BTF_NONE; bt_tries = 0;
        settings_touch();
        if (bt_scan_on) bt_scan(0);
        if (ble_connected()) { fm1_ble_irqs_hold(1); ble_central_cancel(); fm1_ble_irqs_hold(0); }
        bt_wait_t0 = 0; bt_try_t0 = 0;
        bt_sel = 0;
        learn_say("COLLEGO A ", store.bt_dev_name);
    } else if (bt_sel - 1u == n) {
        bt_rescan = 0; bt_scan(1);
        learn_say("CERCO PER 10 S...", 0);
    } else {                                    /* DIMENTICA */
        store.bt_dev_ok = 0; store.bt_dev_name[0] = 0; store.bt_bonded = 0;
        bt_fail = BTF_NONE; bt_tries = 0;
        settings_touch();
        fm1_ble_irqs_hold(1); ble_central_cancel(); fm1_ble_irqs_hold(0);
        bt_sel = 0;
        learn_say("DISPOSITIVO DIMENTICATO", 0);
    }
    dirty_cells = 0xF;
}
static void draw_bt_cell(uint32_t c)
{
    char s[40];
    const felucca_font_t *f = &FONT_L;
    s[0] = 0;
    (void)f;
    cell_begin();
    cv_text(6, 2, &FONT_S, BT_PN[c], C_LAB);
    if (c == 0u) cat(s, bt_on ? "ACCESO" : "SPENTO");
    else if (c == 1u) bt_row_text(bt_sel, s);
    else if (c == 2u) cat(s, BT_MODEN[store.bt_mode % 3u]);
    else cat(s, store.bt_auto ? "SI" : "NO");
    cell_value(s, c == 0u && bt_on ? C_ACC : C_WHITE);
    cv_blit(c & 1u ? 121 : 1, c < 2u ? 140 : 190);
}
static const char *bt_why(void)
{
    switch (bt_fail) {
    case BTF_TIMEOUT: return "NON RISPONDE";
    case BLE_CF_NO_MIDI: return "NIENTE MIDI";
    case BLE_CF_PAIRING: return "ABBINAMENTO";
    case BLE_CF_AUTH: case BLE_CF_NEED_MITM: return "VUOLE UN CODICE";
    case BLE_CF_GATT: return "ERRORE GATT";
    case BLE_CF_KEY: return "CHIAVE";
    default: return bt_fail_code == 0x3Eu ? "NESSUN LINK" : "LINK PERSO";
    }
}
static void bt_strip(char *t)                   /* the tabs' strip on the BLUETOOTH page */
{
    uint32_t st = bt_state();
    char *e;
    if (bt_fail && st != BTS_LINK && st != BTS_SCAN) {   /* 1.4.1: why the attempts fail, and how many */
        static const char HX[] = "0123456789ABCDEF";
        e = cat(t, "FALLITO: ");
        e = cat(e, bt_why());
        if (bt_fail_code) { *e++ = ' '; *e++ = HX[bt_fail_code >> 4]; *e++ = HX[bt_fail_code & 15u]; *e = 0; }
        if (bt_tries > 1u) { e = cat(e, " X"); fmt_u(e, bt_tries); }
        return;
    }
    if (st == BTS_SCAN && bt_rescan) { cat(t, "CERCO "); cat(t + 6, store.bt_dev_name); return; }
    e = cat(t, BT_STN[st]);
    if (st == BTS_SCAN) {                       /* the devices found so far */
        uint8_t idx[BLE_SCAN_N];
        e = cat(e, " ");
        fmt_u(e, ble_scan_list(&bt_found, idx));
        return;
    }
    if (st == BTS_LINK) {                       /* 1.4.3: in which mode, and what arrives: "SMC IN 12 E0 7F 3F" */
        static const char HX[] = "0123456789ABCDEF";
        uint32_t k;
        if (bt_in_n) e = t;                     /* (the width: 30 letters, the title's included) */
        else *e++ = ' ';
        e = cat(e, bt_mode_live ? "SMC" : "MIDI");
        if (!bt_out_on) e = cat(e, " NO MIDI");
        if (!bt_in_n) return;
        e = cat(e, " IN ");
        fmt_u(e, bt_in_n % 1000u); while (*e) e++;
        for (k = 1; k < 4u; k++) { *e++ = ' '; *e++ = HX[(bt_in_last >> (8u * k + 4u)) & 15u]; *e++ = HX[(bt_in_last >> (8u * k)) & 15u]; }
        *e = 0;
    }
}
