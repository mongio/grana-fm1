/* SPDX-License-Identifier: GPL-3.0-only */
/* GRANA: a granular sampler for the M-VAVE FM-1, in the spirit of the MicroGranny 2.
 * Built on Dinghy (zvenson), on Felucca's platform (Leo Kuroshita, Hügelton Instruments) as SLOOP carries it.
 *
 *   keys          play the cloud, 4 voices; C4 (with the octave at 0) is the sound's own pitch
 *   SEL  EDIT     pages GRANI (grains) and MOTO (motion)
 *   HOME          STRATI, then PITCH, INIZIO, AREA STRATI (each layer's own, 1.3); held: HOLD
 *   ENV  GLO      pages FORMA (shape) and MIX
 *   FX            pages FILTRO, DELAY and RIVERBERO (press again for the next)
 *   KNOB 1..4     the four parameters of the page
 *   SELECT        the position in the sound (the big knob: scrub it)
 *   ALGORITHM     the sound (8 built-in)
 *   PRESETS       the preset, 1..8
 *   SAVE          asks where (PRESETS chooses the slot), SAVE again keeps the sound, knobs and sequence there
 *   SEL           page GRANI
 *   HOME (0.6 s)  HOLD: the notes keep sounding when you let go (with the arpeggiator: the chord stays)
 *   ARP           the arpeggiator on / off, and its page
 *   SEQ           the sequencer's page; PLAY starts and stops it, REC writes it a step at a time
 *   LFO           page SPECIALI: drone, freeze, dice; pressed again there, it throws the dice
 *   effects       a multimode filter with a slow LFO, then a ping-pong delay and a plate reverb (22050 Hz)
 *   OCT- OCT+     the octave
 *   USB-MIDI, TRS notes in (any channel), CC to the parameters, MIDI LEARN, clock in and out (GLO: page MIDI)
 *
 * Integers only (no FPU, no libgcc): Q15 levels, Q12 positions, 32-bit phases. */
#define APP_NAME "GRANA"
#include "platform.c"
#include "grana_data.h"

#ifdef GRANA_WEB
#define POOL
#else
#define POOL __attribute__((section(".pool")))
#endif

/* ---- the sound in RAM: 22050 Hz mono, decoded from the flash (or loaded by the web page) */
#define SMP_RATE 22050u
#define SMP_BYTES 203808u                               /* the pool also holds the delay and a canvas */
#define SMP_MAX_HIFI (SMP_BYTES / 2u - 2u)              /* 16 bit: 101902 frames, 4.6 s */
#define SMP_MAX_LOFI (SMP_BYTES - 2u)                   /*  8 bit: 203806 frames, 9.2 s */
#define MOD_BYTES 8192u                                 /* 1.4: VELVET's line, after the sound (1.3: 32 KB for the stutter) */
static int16_t smp_mem[(SMP_BYTES + MOD_BYTES) / 2u] POOL;
#define mod_line (smp_mem + SMP_BYTES / 2u)             /* stereo frames: [2 * i] left, [2 * i + 1] right */
#define MODL_N (MOD_BYTES / 4u)                         /* 2048 frames, 46 ms at 44100: the chorus's and the flanger's reach */
#define smp smp_mem                                     /* HIFI: int16 frames */
#define smp8 ((int8_t *)smp_mem)                        /* LOFI: int8 frames, the old samplers' 8 bit */

/* the layers (STRATI): up to 4 sounds at once, sharing smp_mem. A sound longer than its share is loaded in part,
 * a window around where the grains read, and the window follows POSIZIONE (layers_sync) */
#define NL 4u
#define NOSND 0xFFFFu
typedef struct {
    volatile uint32_t len;                  /* frames in RAM (the window), 0 = off or loading */
    volatile uint32_t off;                  /* bytes into smp_mem, even */
    volatile uint32_t lofi;                 /* 8 bit */
    volatile uint32_t full;                 /* the sound's frames */
    volatile int32_t wstart;                /* the window's first frame in the sound */
    volatile int32_t pos0, area, spray;     /* in the sound's frames (engine_update) */
    volatile uint32_t gen;                  /* +1 at each load: the grains of the old one stop */
    volatile int32_t ct;                    /* 1.3: its own pitch, cents (PITCH STRATI and its LFO) */
    volatile int32_t pan;                   /* 1.4: its place, Q15 -32767 (left) .. 32767 (PAN STRATI and its LFO) */
    uint32_t snd;                           /* main loop: the sound in it, NOSND none */
    volatile int32_t vol;                   /* 1.5: its volume, Q15 (VOLUMI, MUTE and SOLO) */
    volatile uint32_t r11;                  /* 1.6: its sound is at 11025 Hz: read at half the step */
} layer_t;
static layer_t lay[NL] = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NOSND}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NOSND},
                          {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NOSND}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NOSND}};
static int32_t lp_cents(uint32_t v)         /* PITCH STRATI: 120 = 0; a cent a step to +-25, then a quarter tone a step to +-24 st */
{
    int32_t d = (int32_t)v - 120, a = d < 0 ? -d : d, c = a <= 25 ? a : 25 * a - 600;
    if (c > 2400) c = 2400;
    return d < 0 ? -c : c;
}
static volatile uint32_t lay_cum[NL];       /* the levels summed: a grain picks its layer by them */
/* 1.5, SPLIT: the keyboard in zones, one a layer with a sound, from the lowest; a note's zone is chosen when it starts */
static volatile uint8_t zone_n, zone_lay[NL], zone_pt[NL - 1u];
static int32_t zone_of(uint32_t note)       /* the layer a note plays, -1 all of them (SOVRAPPOSTI) */
{
    uint32_t n = zone_n, i, z = 0;
    if (n < 2u) return -1;
    for (i = 0; i + 1u < n; i++) if (note >= zone_pt[i]) z++;
    return zone_lay[z];
}
static uint8_t lay_mute, lay_solo;
static uint32_t smc_btn;                    /* 1.4.6: the SMC-Mixer's transport as the FM-1's own buttons */          /* 1.4: the SMC-Mixer's MUTE and SOLO (not saved) */

/* ---- the parameters */
enum { P_POS, P_AREA, P_SCAN, P_DIR,                     /* MOTO (SEL) */
       P_SIZE, P_DENS, P_SPRAY, P_PITCH,                /* GRANI (EDIT) */
       P_WIN, P_REV, P_ATK, P_REL,                      /* FORMA */
       P_PAN, P_COMP, P_RETRIG, P_VOL,                  /* MIX   */
       P_NMODE, P_DTIME, P_DFB, P_DMIX,                 /* NASTRO (1.4, was DELAY): mode, time, feedback / repeats, mix */
       P_RMIX, P_RDEC, P_RTONE, P_RMOD,                 /* RIVERBERO */
       P_AMODE, P_ADIV, P_AOCT, P_AGATE,                /* ARP */
       P_TEMPO, P_SWING, P_SLEN, P_SDIV,                /* SEQ */
       P_DRONE, P_FREEZE, P_CRUSH, P_TASTO,             /* SPECIALI; TASTO: a held note pressed again restarts or stops */
       P_FMODE, P_FCUT, P_FRES, P_FLFO,                 /* FILTRO */
       P_SND, P_SND2, P_SND3, P_SND4,                   /* STRATI: the sounds, 2..4 OFF or one of the bank */
       P_LV1, P_LV2, P_LV3, P_LV4,                      /* LIVELLI: how many grains each takes */
       P_JIT, P_SALTO, P_DADO, P_OGNI,                  /* MOTO 2 (SEL again) */
       P_L1W, P_L1R, P_L1D, P_L1T,                      /* LFO 1: shape, rate, depth (100 = 0), destination */
       P_L2W, P_L2R, P_L2D, P_L2T,                      /* LFO 2 */
       P_MTYPE, P_MRATE, P_MDEPTH, P_MMIX,              /* MODULAZIONE: ring, phaser, flanger, frequency shift */
       P_SHIM, P_SHINT, P_SHTONE, P_SHDIFF,             /* RIVERBERO 2: the shimmer */
       P_LP1, P_LP2, P_LP3, P_LP4,                      /* 1.3, PITCH STRATI: each layer's pitch on top of PITCH (120 = 0) */
       P_LS1, P_LS2, P_LS3, P_LS4,                      /* INIZIO STRATI: each layer's start on top of POSIZIONE (100 = 0) */
       P_LA1, P_LA2, P_LA3, P_LA4,                      /* AREA STRATI: each layer's share of AREA (100 = the same) */
       P_NVAR, P_DTONE, P_NPASSO, P_NCASO,              /* 1.4, NASTRO 2: variation, tone, step (50 = 0); 1.5: RIPETI's repeats at random */
       P_PN1, P_PN2, P_PN3, P_PN4,                      /* 1.4, PAN STRATI (GLO): each layer's place, 100 = the centre */
       P_VO1, P_VO2, P_VO3, P_VO4,                      /* 1.5, VOLUMI (GLO): each layer's own volume */
       P_DISP, P_SP1, P_SP2, P_SP3,                     /* 1.5, SPLIT (HOME): SOVRAPPOSTI / SPLIT, the three split notes */
       P_SCALA, P_TONICA, P_BIT12, P_MOTORE,            /* 1.6: the keyboard's scale and its tonic (TASTIERA), the 12-bit output (MIX);
                                                         * 1.8: the engine (ALGORITHM; was a free place) */
       P_M1, P_M2, P_M3, P_M4,                          /* 1.8, MOTORE (EDIT again): the engine's own four */
       NP };
#define NP5 80u                                         /* the parameters of 1.3's presets (PRESET0 is written in them) */
#define NPAGE 26u
#define NPAR (NPAGE * 4u)
static const uint8_t PMAX[NP] = {200, 200, 200, 3,  63, 63, 100, 48,  3, 100, 63, 63,  100, 15, 1, 100,   /* 1.5: AREA, SCAN 200 */
                                 4, 77, 95, 100,  100, 100, 100, 100,  4, 4, 3, 100,  220, 50, 63, 4,
                                 50, 1, 15, 1,  6, 200, 200, 100,  47, 48, 48, 48,   /* 1.6: 48 sounds */  100, 100, 100, 100,   /* FREQUENZA, RISONANZA 200 */
                                 100, 100, 100, 7,  6, 133, 200, 28,  6, 133, 200, 30,  202, 100, 100, 100,  100, 4, 100, 100,
                                 240, 240, 240, 240,  200, 200, 200, 200,  200, 200, 200, 200,
                                 100, 100, 100, 1,  200, 200, 200, 200,
                                 100, 100, 100, 100,  1, 127, 127, 127,
                                 14, 11, 1, 3,  100, 100, 100, 100};
static const char *const PNAME[NPAR] = {"POSIZIONE", "AREA", "SCAN", "DIREZIONE",
                                        "DIMENSIONE", "DENSITA'", "SPRAY", "PITCH",
                                        "FINESTRA", "REVERSE", "ATTACCO", "RILASCIO",
                                        "STEREO", "COMPRESSORE", "RETRIG", "VOLUME",
                                        "MODO", "TEMPO", "RITORNO", "MIX",
                                        "MIX", "CODA", "TONO", "MODULAZIONE",
                                        "MODO", "DIVISIONE", "OTTAVE", "GATE",
                                        "TEMPO", "SWING", "PASSI", "DIVISIONE",
                                        "DRONE", "FREEZE", "CRUSH", "TASTO",
                                        "MODO", "FREQUENZA", "RISONANZA", "MOVIMENTO",
                                        "STRATO 1", "STRATO 2", "STRATO 3", "STRATO 4",
                                        "QUANTITA' 1", "QUANTITA' 2", "QUANTITA' 3", "QUANTITA' 4",   /* 1.6.2: LIVELLO before */
                                        "JITTER", "SALTO", "DADO", "OGNI",
                                        "FORMA", "VELOCITA'", "INTENSITA'", "DESTINAZIONE",
                                        "FORMA", "VELOCITA'", "INTENSITA'", "DESTINAZIONE",
                                        "TIPO", "VELOCITA'", "PROFONDITA'", "MIX",
                                        "SHIMMER", "INTERVALLO", "TONO", "DIFFUSIONE",
                                        "PITCH 1", "PITCH 2", "PITCH 3", "PITCH 4",
                                        "INIZIO 1", "INIZIO 2", "INIZIO 3", "INIZIO 4",
                                        "AREA 1", "AREA 2", "AREA 3", "AREA 4",
                                        "VARIAZIONE", "TONO", "PASSO", "RIP. A CASO",
                                        "PAN 1", "PAN 2", "PAN 3", "PAN 4",
                                        "VOLUME 1", "VOLUME 2", "VOLUME 3", "VOLUME 4",
                                        "DISPOSIZIONE", "SPLIT 1", "SPLIT 2", "SPLIT 3",
                                        "SCALA", "TONICA", "USCITA", "MOTORE",
                                        "MOTORE 1", "MOTORE 2", "MOTORE 3", "MOTORE 4"};
enum { MO_CLASSICO, MO_STIRA, MO_FORMANTI, MO_RISON, NMOT };   /* 1.8: the engines, on ALGORITHM */
static const char *const MOT_NAME[NMOT] = {"CLASSICO", "STIRAMENTO", "FORMANTI", "RISONATORE"};
static const char *const MOT_KNOB[NMOT][4] = {{"", "", "", ""}, {"VELOCITA'", "SFOCATURA", "ARMONICI", "DERIVA"},
                                              {"FORMANTE", "LARGHEZZA", "RESPIRO", "VIBRATO"},
                                              {"DECADIMENTO", "TIMBRO", "DISPERSIONE", "MIX"}};
static const char *const PAGE[NPAGE + 2u] = {"MOTO", "GRANI", "FORMA", "MIX", "NASTRO", "RIVERBERO", "ARP", "SEQ", "SPECIALI",
                                        "FILTRO", "STRATI", "QUANTITA'", "MOTO 2", "LFO 1", "LFO 2", "MODULAZIONE", "RIVERBERO 2",
                                        "PITCH STRATI", "INI/AREA 1-2", "INI/AREA 3-4", "NASTRO 2",
                                        "PAN STRATI", "VOLUMI", "SPLIT", "TASTIERA", "MOTORE", "MIDI", "BT"};
enum { PG_MOTO = 0, PG_GRANI = 1, PG_FORMA = 2, PG_MIX = 3, PG_DLY = 4, PG_RIV = 5, PG_ARP = 6, PG_SEQ = 7, PG_SPEC = 8,
       PG_FILT = 9, PG_STR = 10, PG_LIV = 11, PG_MOTO2 = 12, PG_LFO1 = 13, PG_LFO2 = 14, PG_MOD = 15, PG_RIV2 = 16,
       PG_LPIT = 17, PG_LINI = 18, PG_LAREA = 19,   /* 1.3: the layers' own pitch, start, area (HOME again) */
       PG_NAS2 = 20, PG_LPAN = 21,          /* 1.4: NASTRO's second page (FX), the layers' pan (GLO) */
       PG_VOL = 22, PG_SPLIT = 23,          /* 1.5: the layers' volumes (GLO), the keyboard split (HOME) */
       PG_TAST = 24,                        /* 1.6: RETRIG, TASTO, SCALA, TONICA (ENV again) */
       PG_MOT = 25,                         /* 1.8: the engine's own knobs (EDIT again) */
       PG_MIDI = 26, PG_BT = 27 };          /* BLUETOOTH: GRANA + Bluetooth only (GRANA_BT) */
#ifndef GRANA_BT
#define GRANA_BT 0
#endif                     /* MIDI (1.2): the clock and MIDI LEARN, settings rather than a preset's parameters */
/* SELECT turns through these as the buttons sit on the panel: HOME, then FX SEL ENV LFO EDIT GLO left to right,
 * each button's pages in the order it opens them (ARP and SEQ only by their buttons) */
static const uint8_t PG_ORDER[24] = {PG_STR, PG_SPLIT, PG_LIV, PG_LPIT, PG_LINI, PG_LAREA,   /* HOME (1.5: SPLIT, QUANTITA') */
                                     PG_FILT, PG_MOD, PG_DLY, PG_NAS2, PG_RIV, PG_RIV2,   /* FX */
                                     PG_MOTO, PG_MOTO2,            /* SEL */
                                     PG_FORMA, PG_TAST,            /* ENV (1.6: TASTIERA) */
                                     PG_LFO1, PG_LFO2, PG_SPEC,    /* LFO */
                                     PG_GRANI, PG_MOT,             /* EDIT (1.8: MOTORE) */
                                     PG_VOL, PG_LPAN, PG_MIX};     /* GLO (1.5: VOLUMI where LIVELLI was) */
/* 1.5: INIZIO and AREA by layer: the two pages show the start and the area of layers 1-2, then 3-4 (the parameters keep their
 * places, so presets, CCs and LEARN are unchanged). Every other page shows its own four */
static uint32_t pp(uint32_t pg, uint32_t k)
{
    static const uint8_t TAST[4] = {P_RETRIG, P_TASTO, P_SCALA, P_TONICA};
    if (pg == PG_LINI || pg == PG_LAREA) return (pg == PG_LINI ? 0u : 2u) + (k >> 1) + (k & 1u ? P_LA1 : P_LS1);
    if (pg == PG_TAST) return TAST[k & 3u];                 /* 1.6: RETRIG from MIX, TASTO from SPECIALI */
    if (pg == PG_MIX && k == 2u) return P_BIT12;            /* MIX: STEREO, COMPRESSORE, USCITA, VOLUME */
    if (pg == PG_SPEC && k == 3u) return P_MOTORE;          /* SPECIALI: DRONE, FREEZE, CRUSH, MOTORE (1.8; a free place before) */
    return pg * 4u + k;
}
static int32_t cell_of(uint32_t pg, uint32_t p)   /* the knob that shows p on page pg, -1 none */
{
    uint32_t k;
    for (k = 0; k < 4u; k++) if (pp(pg, k) == p) return (int32_t)k;
    return -1;
}
static const char *const DIRN[4] = {"AVANTI", "INDIETRO", "ALTERNATO", "CASO"};
/* 1.6, SCALA: the keys snap to the nearest note of the scale (a tie: the lower one); bit i = i semitones above the tonic */
#define NSCALE 15u
static const char *const SCALEN[NSCALE] = {"CROMATICA", "MAGGIORE", "MINORE", "MIN. ARMONICA", "DORICA", "FRIGIA", "LIDIA",
                                          "MISOLIDIA", "PENTA MAGG.", "PENTA MIN.", "BLUES", "TONI INTERI", "HIRAJOSHI",
                                          "IN SEN", "ARABA"};
static const uint16_t SCALEM[NSCALE] = {0xFFF, 0xAB5, 0x5AD, 0x9AD, 0x6AD, 0x5AB, 0xAD5, 0x6B5, 0x295, 0x4A9, 0x4E9, 0x555,
                                        0x18D, 0x4A3, 0x9B3};
static volatile uint8_t scale_sel, scale_ton;   /* what the notes snap to (engine_update; the ISR's notes read them too) */
static uint32_t snap(uint32_t note)
{
    uint32_t m = SCALEM[scale_sel % NSCALE], d, k;
    if (m == 0xFFFu) return note;
    d = (note + 120u - scale_ton) % 12u;
    for (k = 0; k <= 6u; k++) {              /* the nearest: at each distance below first, then above */
        if (m >> ((d + 12u - k) % 12u) & 1u) return note >= k ? note - k : note;
        if (k && (m >> ((d + k) % 12u) & 1u)) return note + k > 127u ? note : note + k;
    }
    return note;
}
static const char *const LFOW[7] = {"SINUSOIDE", "TRIANGOLO", "DENTE SU", "DENTE GIU'", "QUADRA", "CASO GRADINI", "CASO MORBIDO"};
static const char *const LFOT[31] = {"POSIZIONE", "AREA", "SCAN", "DIMENSIONE", "DENSITA'", "SPRAY", "PITCH", "FILTRO",
                                     "RISONANZA", "VOLUME", "STEREO", "STRATI 1-2", "NASTRO MIX", "NASTRO TEMPO", "RIV MIX",
                                     "RIV CODA", "SHIMMER", "MOD VELOCITA'", "MOD PROFOND.", "MOD MIX",
                                     "PITCH 1", "PITCH 2", "PITCH 3", "PITCH 4", "MOD CARATT.", "PAN 1", "PAN 2", "PAN 3",
                                     "PAN 4", "VEL. LFO 1", "INT. LFO 1"};
enum { LD_POS, LD_AREA, LD_SCAN, LD_SIZE, LD_DENS, LD_SPRAY, LD_PITCH, LD_FCUT, LD_FRES, LD_VOL, LD_PAN, LD_MIX12,
       LD_DMIX, LD_DTIME, LD_RMIX, LD_RDEC, LD_SHIM, LD_MRATE, LD_MDEPTH, LD_MMIX, LD_LP1, LD_LP2, LD_LP3, LD_LP4,
       LD_MTYPE, LD_PN1, LD_PN2, LD_PN3, LD_PN4, LD_L1R, LD_L1D, LD_N };
/* MODULAZIONE (1.4): its first knob is the type and the colour at once: 0 OFF; 1..101 VELVET, its CARATTERE 0..100 (chorus
 * wide, chorus, flanger, phaser and what lies between); 102..202 METALLO, its LATO -100..+100 in steps of 2 (the frequency
 * shifted down, 0 a ring modulator, shifted up). One at a time. */
#define MT_VEL 1u
#define MT_MET 102u
static const char *const NMODEN[5] = {"ECO", "RIPETI", "INDIETRO", "NASTRO", "CONGELA"};
#define NT_SYNC 64u                         /* NASTRO's TEMPO: 0..63 free (DLY_MS), 64..77 in time with the clock */
#define MR_SYNC 87u                         /* VELVET's VELOCITA': 0..86 free, 87..100 in time with the clock */
static const char *const MSYNCN[14] = {"8/1", "4/1", "2/1", "1/1", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T",
                                       "1/32", "1/32T", "1/64"};
static const uint16_t MSYNC_T[14] = {3072, 1536, 768, 384, 192, 96, 64, 48, 32, 24, 16, 12, 8, 6};   /* in ticks, 96 a quarter */
static const char *const SHINTN[5] = {"+1 OTTAVA", "+QUINTA", "+OTT.+QUINTA", "+2 OTTAVE", "-1 OTTAVA"};
static const char *const SYNCN[7] = {"4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16"};
static const uint8_t SYNC_Q4[7] = {64, 32, 16, 8, 4, 2, 1};       /* a cycle, in sixteenths */
static const char *const FMODE[7] = {"OFF", "BASSO 24DB", "BASSO 12DB", "BANDA", "ALTO 12DB", "NOTCH", "ALTO 24DB"};
#define FC_LO 161                           /* 80 Hz: of the 1024 steps, 30 Hz .. 15.4 kHz */
#define FC_SPAN 755                         /* to 8 kHz */
static const uint8_t FM_ORDER[7] = {0, 1, 2, 3, 4, 6, 5};      /* the knob's order: ALTO 24DB before NOTCH (1.2) */
static uint32_t fm_pos(uint32_t v) { uint32_t i; for (i = 0; i < 7u; i++) if (FM_ORDER[i] == v) return i; return 0; }
static const char *const AMODE[5] = {"SU", "GIU'", "SU-GIU'", "ORDINE", "CASO"};
static const char *const DIVN[5] = {"1/4", "1/8", "1/8T", "1/16", "1/32"};
static const uint8_t DIV_TICKS[5] = {96, 48, 32, 24, 12};          /* at 96 ticks a quarter note */
static const uint8_t OGNI_N[8] = {0, 1, 2, 4, 8, 16, 32, 64};      /* sixteenths between two dice */
static const char *const WNAME[4] = {"MORBIDA", "PIATTA", "PERC", "INVERSA"};

static uint8_t par[NP];
#define NPRESET 64u                         /* 64 places; the first 8 hold the factory presets until you save over them */
#define NFACT 8u
static const uint8_t PRESET0[NFACT][NP5] = {   /* as 1.3 wrote them: prec_new14 makes them 1.4's */
    /* POS AREA SCAN DIR  SIZE DENS SPRAY PITCH  WIN REV ATK REL  PAN COMP RETRIG VOL  DMIX DTIME DFB DTONE  RMIX RDEC RTONE RMOD
     * AMODE ADIV AOCT AGATE  TEMPO-20 SWING PASSI-1 SDIV  DRONE FREEZE CRUSH -  FMODE FCUT FRES FLFO  SND SND2 SND3 SND4  LV1..LV4
     * JIT SALTO DADO OGNI  L1 SHAPE RATE DEPTH DEST  L2 SHAPE RATE DEPTH DEST  MTYPE MRATE MDEPTH MMIX  SHIM SHINT SHTONE SHDIFF
     * PITCH 1..4  INIZIO 1..4  AREA 1..4 (1.3)
     * (MIX: COMP where CRUSH was; SPECIALI: CRUSH in the third place) */
    {40, 60, 4, 0,  40, 40, 8, 24,  0, 0, 30, 40,  60, 20, 0, 80,  20, 46, 40, 45,  55, 85, 45, 60,  0, 3, 1, 60,  60, 0, 15, 3,  0, 0, 0, 0,  0, 100, 20, 0,  0, 2, 0, 0,  100, 35, 100, 100,  4, 0, 0, 0,  0, 14, 112, 0,  0, 20, 100, 0,  2, 20, 60, 40,  30, 0, 60, 60,  120, 120, 120, 120,  100, 100, 100, 100,  100, 100, 100, 100},   /* VOCI: a slow choir in a hall */
    {10, 100, 0, 0,  46, 46, 20, 36,  0, 10, 20, 46,  90, 15, 0, 75,  15, 52, 50, 60,  70, 95, 55, 80,  4, 3, 2, 40,  70, 0, 15, 3,  0, 0, 0, 0,  1, 72, 55, 60,  1, 0, 0, 0,  100, 100, 100, 100,  0, 0, 0, 0,  6, 18, 128, 7,  0, 20, 100, 0,  3, 12, 50, 30,  45, 0, 70, 70,  120, 120, 120, 120,  100, 100, 100, 100,  100, 100, 100, 100},   /* CAMPANA: shimmer, a long tail */
    {0, 100, 8, 0,  30, 36, 2, 24,  1, 0, 4, 30,  40, 25, 1, 80,  35, 40, 45, 50,  35, 70, 50, 40,  0, 3, 1, 70,  80, 20, 15, 3,  0, 0, 0, 0,  0, 100, 20, 0,  2, 0, 0, 0,  100, 100, 100, 100,  0, 0, 0, 0,  0, 20, 100, 0,  0, 20, 100, 0,  0, 30, 50, 50,  0, 0, 60, 50,  120, 120, 120, 120,  100, 100, 100, 100,  100, 100, 100, 100},   /* ARPA: echoes */
    {60, 100, 0, 0,  18, 54, 60, 24,  2, 30, 10, 40,  100, 20, 0, 80,  0, 30, 30, 50,  60, 88, 35, 50,  4, 4, 2, 30,  80, 0, 15, 3,  0, 0, 0, 0,  4, 40, 30, 35,  3, 7, 0, 0,  100, 45, 100, 100,  20, 0, 30, 2,  5, 40, 150, 10,  0, 20, 100, 0,  4, 55, 30, 30,  0, 0, 60, 50,  120, 120, 120, 120,  100, 100, 100, 100,  100, 100, 100, 100},   /* PIOGGIA: a dark room */
    {90, 20, 1, 0,  52, 44, 4, 24,  0, 0, 40, 50,  70, 10, 0, 75,  0, 40, 30, 50,  75, 100, 40, 70,  2, 2, 1, 80,  50, 0, 15, 2,  0, 0, 0, 0,  1, 60, 40, 45,  4, 0, 0, 0,  100, 100, 100, 100,  0, 0, 0, 0,  1, 12, 108, 13,  6, 8, 130, 7,  3, 8, 70, 40,  55, 0, 55, 80,  120, 120, 120, 120,  100, 100, 100, 100,  100, 100, 100, 100},   /* ORGANO: a frozen pad, endless */
    {0, 100, 20, 0,  26, 30, 0, 24,  1, 0, 1, 12,  20, 40, 1, 80,  40, 31, 55, 70,  15, 40, 60, 20,  0, 3, 1, 50,  100, 10, 15, 3,  0, 0, 0, 0,  0, 100, 20, 0,  5, 0, 0, 0,  100, 100, 100, 100,  0, 0, 0, 0,  0, 20, 100, 0,  0, 20, 100, 0,  0, 30, 50, 50,  0, 0, 60, 50,  120, 120, 120, 120,  100, 100, 100, 100,  100, 100, 100, 100},   /* RITMO: stutter, dub delay */
    {40, 100, 4, 1,  36, 40, 30, 24,  2, 70, 6, 36,  80, 20, 0, 75,  25, 56, 60, 40,  50, 90, 70, 90,  3, 3, 2, 50,  70, 25, 15, 3,  0, 0, 0, 0,  3, 80, 60, 50,  6, 0, 0, 0,  100, 100, 100, 100,  0, 0, 0, 0,  0, 20, 100, 0,  0, 20, 100, 0,  4, 62, 20, 40,  25, 1, 80, 40,  120, 120, 120, 120,  100, 100, 100, 100,  100, 100, 100, 100},   /* VETRO: backwards glass */
    {20, 80, 6, 0,  22, 50, 10, 12,  1, 0, 12, 30,  50, 35, 0, 75,  30, 20, 70, 30,  30, 60, 30, 30,  0, 4, 1, 40,  110, 0, 15, 3,  0, 0, 9, 0,  1, 55, 75, 40,  7, 0, 0, 0,  100, 100, 100, 100,  10, 0, 0, 0,  0, 20, 100, 0,  0, 20, 100, 0,  1, 40, 60, 30,  0, 0, 60, 50,  120, 120, 120, 120,  100, 100, 100, 100,  100, 100, 100, 100},   /* MOTORE: crushed engine, short slap */
};
static uint32_t cur_preset, edited;

/* ---- what the audio ISR reads, worked out by the main loop from the parameters (one writer: the main loop) */
static struct {
    volatile int32_t scan_ph;               /* the scan's step per output frame, a fraction of the area (2^32 = all) */
    volatile uint32_t dir;                  /* DIREZIONE: 0 forward, 1 back, 2 back and forth, 3 at random */
    volatile uint32_t salto;                /* SALTO: how far each new note starts on, a fraction of the area */
    volatile uint32_t gframes;              /* grain length, output frames */
    volatile uint32_t winc;                 /* grain window phase step */
    volatile int32_t period;                /* output frames between grains */
    volatile int32_t pitch_ct, jit_ct;      /* cents */
    volatile uint32_t win;                  /* window table offset */
    volatile uint32_t rev;                  /* reverse probability, 0..65536 */
    volatile int32_t atk, rel;              /* envelope steps per frame, Q23 */
    volatile int32_t pan;                   /* stereo spread, Q15 */
    volatile uint32_t crush_div, crush_mask;
    volatile int32_t gain;                  /* Q15: volume and overlap normalisation */
    volatile uint32_t retrig, freeze;       /* freeze: P_FREEZE */
    volatile int32_t d_mix, d_time, d_fb, d_tone;   /* delay: Q15, Q8 samples at 22050, Q15, Q15 */
    volatile int32_t r_mix, r_dec, r_tone, r_mod;   /* reverb: Q15, Q15, Q15, Q8 samples */
    volatile int32_t m_type, m_depth, m_mix, m_fb;   /* MODULAZIONE: type, depth Q15, mix Q15, feedback Q15 */
    volatile uint32_t m_inc;                          /* its rate, carrier or shift: phase per sample */
    volatile int32_t m_dir;                           /* the frequency shift: +1 up, -1 down */
    volatile int32_t comp;                            /* the compressor, 0..100 */
    volatile int32_t sh_amt, sh_tone;                 /* the shimmer: Q15 into the tank, its low-pass Q15 */
    volatile int32_t sh_step, sh_win;                 /* its pitch: the read's drift a sample (Q24), the window (samples) */
    volatile int32_t f_mode, f_cut, f_k, f_lk, f_lfo, f_in; /* filter: mode, cutoff step 0..1023, damping Q16, LFO depth (steps), input gain Q16 */
    volatile int32_t f_hin, f_bg, f_kn, f_n2, f_lo, f_span;   /* 1.2: high pass input Q16, band's gain Q16, notch damping Q16,
                                                              * the second notch's distance (steps), the LFO's reach (steps) */
    volatile int32_t m_fb2, m_fsc;          /* 1.2: the phaser's feedback Q15, the frequency shift's make-down Q15 */
    volatile int32_t v_d, v_s, v_t2, v_fb, v_ph, m_k;   /* 1.4, VELVET: centre and swing (Q8 samples), second tap, feedback,
                                                         * phaser's share (Q15); METALLO: LATO, Q15 */
    volatile int32_t n_mode, n_len, n_var, n_reps, n_short, n_pdrop, n_ratio;   /* 1.4, NASTRO (see fx_frame) */
    volatile int32_t n_caso;
    volatile uint32_t bit12;                /* 1.6: the output cut to 12 bits */
    volatile uint32_t mot;                  /* 1.8: the engine (MO_*) */
    volatile uint32_t st_oct;               /* STIRAMENTO: a grain an octave away, 0..65536 */
    volatile int32_t fo_ct, fo_breath, fo_vib, fo_vinc, fo_wmax;   /* FORMANTI: the formant's shift (cents), RESPIRO (0..65536),
                                                                     * VIBRATO (cents) and its phase step, the longest grain (frames) */
    volatile int32_t rs_fb, rs_damp, rs_ap, rs_mix, rs_gin;   /* RISONATORE: Q15 (rs_gin: the pluck's share, see engine_update) */                /* 1.5: RIPETI's repeats go on by chance (VARIAZIONE), up to RITORNO's */
} eng;

/* ---- the voices and their grains (ISR) */
#define NV 4u
#define NG 8u
typedef struct {
    int32_t pos, step;                      /* Q12 source frames */
    uint32_t wph, winc;
    int32_t gl, gr;                         /* pan gains, Q15 */
    uint32_t delay;                         /* frames to wait in this block */
    uint32_t gen;                           /* its layer's load */
    uint8_t on, lay;
} grain_t;
typedef struct {
    volatile int32_t base_ct;               /* the note in cents from C4 (written before gate) */
    volatile int32_t vel;                   /* Q15 */
    volatile int32_t pos_lock;              /* a sequencer step's POSIZIONE (0..200), -1 = the knob's */
    volatile uint8_t gate;                  /* 1 = held: written by the main loop, or by the ISR for its own voices */
    volatile uint8_t note;
    volatile uint8_t restart;               /* the ISR's voices: a new note on a voice already sounding */
    volatile uint8_t start;                 /* the frame of the block the note starts at */
    volatile int8_t zone;                   /* 1.5, SPLIT: the only layer it plays, -1 all */
    uint8_t active;                         /* the ISR's */
    int32_t env;                            /* Q23 */
    uint32_t head;                          /* the scan, a fraction of the area (2^32 = all) */
    int32_t timer;                          /* frames to the next grain */
    uint32_t age;
    grain_t g[NG];
    uint32_t facc, vph;                     /* 1.8, FORMANTI: the period's fraction (Q8), the vibrato's phase */
    int32_t per8;                           /* FORMANTI: the last period, Q8 frames; RISONATORE: the string's (Q8) */
    int32_t rs_w, rs_lp, rs_ax, rs_ay;      /* RISONATORE: the string's write place, low-pass, all-pass */
} voice_t;
static voice_t voice[NV];
static uint32_t ghead;                      /* the shared scan, for RETRIG off */
static uint32_t rnd = 0x12345678u;
static uint32_t xrand(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }
static uint32_t rnd_main = 0x9E3779B9u;      /* the main loop's own (the dice) */
static uint32_t xrand_main(void) { rnd_main ^= rnd_main << 13; rnd_main ^= rnd_main >> 17; rnd_main ^= rnd_main << 5; return rnd_main; }

/* what the screen shows (ISR writes, main reads; a torn read only draws a wrong pixel) */
static volatile int32_t vis_pos[NV * NG];  /* the frame in its layer's sound, -1 = off */
static volatile uint8_t vis_amp[NV * NG], vis_lay[NV * NG];
static volatile int32_t vis_head[NV + 1u]; /* each voice's scan (head >> 8), then the shared one; -1 = silent */
static volatile uint32_t audio_us;         /* the last render's time */

/* METALLO's FREQUENZA: 0.1 Hz .. 2 kHz, 101 steps of x 1.10417 (tenths of Hz; the phase step a sample at 44.1 kHz) */
static uint16_t METAL_DHZ[101];
static uint32_t METAL_INC[101];
static void metal_init(void)
{
    uint64_t f = (uint64_t)1 << 32;          /* tenths of Hz, Q32 */
    uint32_t i;
    if (METAL_INC[100]) return;
    for (i = 0; i < 101u; i++) {
        uint32_t t = (uint32_t)((f + 0x80000000u) >> 32);
        METAL_DHZ[i] = (uint16_t)t;
        METAL_INC[i] = (uint32_t)(((f >> 16) * 9739u) >> 16);   /* 2^32 / 441000 a tenth */
        f = (f * 72363u) >> 16;              /* 20000^(1/100), Q16 */
    }
}
static uint32_t metal_at(uint32_t dhz) { uint32_t i; metal_init(); for (i = 0; i < 100u && METAL_DHZ[i] < dhz; i++) continue; return i; }
static uint32_t cents_step(int32_t c)       /* cents from the sound's pitch -> Q12 source frames per output frame */
{
    uint32_t o, v;
    c += 4800;                              /* 4 octaves down at most */
    if (c < 0) c = 0;
    if (c > 9599) c = 9599;                 /* 4 up */
    o = (uint32_t)c / 1200u;
    v = (2048u * CENTS_Q15[(uint32_t)c - o * 1200u]) >> 15;     /* 22050 -> 44100: half a frame at unity */
    return o >= 4u ? v << (o - 4u) : v >> (4u - o);
}

static inline uint32_t scan_at(uint32_t h)  /* the head as the grains read it: ALTERNATO folds it back */
{
    if (eng.dir == 2u) return h < 0x80000000u ? h << 1 : (~h) << 1;
    return h;
}
static uint32_t salto_acc;
static void spawn(voice_t *v, uint32_t at)
{
    uint32_t k, L, r, tot = lay_cum[NL - 1u];
    grain_t *g = 0;
    layer_t *ly;
    int32_t f, sp, p, len;
    for (k = 0; k < NG; k++)
        if (!v->g[k].on) { g = &v->g[k]; break; }
    if (!g) return;                         /* all 8 busy: this one is skipped */
    if (v->zone >= 0) L = (uint32_t)v->zone;   /* SPLIT: its zone's layer */
    else {
        if (!tot) return;
        r = xrand() % tot;                  /* the layer, as often as its level says */
        for (L = 0; L + 1u < NL && r >= lay_cum[L]; L++) ;
    }
    ly = &lay[L];
    if (!ly->vol) return;
    len = (int32_t)ly->len;
    if (len < 64) return;
    f = (v->pos_lock >= 0 ? (int32_t)((uint32_t)v->pos_lock * ly->full / 200u) : ly->pos0) +
        (int32_t)(((uint64_t)(eng.dir == 3u ? xrand() : scan_at(v->head)) * (uint32_t)ly->area) >> 32);
    sp = ly->spray;
    if (sp > 0) f += (int32_t)(xrand() % (uint32_t)(2 * sp + 1)) - sp;
    f -= ly->wstart;                        /* into the window in RAM, around if it falls out */
    f %= len;
    if (f < 0) f += len;
    g->pos = f << 12;
    g->lay = (uint8_t)L;
    g->gen = ly->gen;
    p = eng.pitch_ct + v->base_ct + ly->ct;
    if (eng.mot == MO_FORMANTI) p = eng.fo_ct + ly->ct;    /* 1.8: read at the sound's own pace, the note is the grains' rate */
    if (eng.jit_ct > 0) p += (int32_t)(xrand() % (uint32_t)(2 * eng.jit_ct + 1)) - eng.jit_ct;
    if (eng.mot == MO_STIRA && (xrand() & 0xFFFFu) < eng.st_oct) {   /* ARMONICI: an octave up, down, or a twelfth up */
        uint32_t o = xrand() & 3u;
        p += o < 2u ? 1200 : o == 2u ? -1200 : 1902;
    }
    g->step = (int32_t)cents_step(p);
    if (ly->r11) g->step >>= 1;             /* 1.6: a sound at 11025 Hz */
    if ((xrand() & 0xFFFFu) < eng.rev) g->step = -g->step;
    {   /* its layer's place, STEREO scattering the grains around it */
        int32_t q = ly->pan;
        if (eng.pan > 0) q += (int32_t)(xrand() % (uint32_t)(2 * eng.pan + 1)) - eng.pan;   /* -pan..+pan, Q15 */
        if (q > 32767) q = 32767;
        if (q < -32767) q = -32767;
        g->gl = (((32768 - q) >> 1) * ly->vol) >> 15;   /* and its volume */
        g->gr = (((32768 + q) >> 1) * ly->vol) >> 15;
    }
    g->wph = 0;
    g->winc = eng.winc;
    if (eng.mot == MO_FORMANTI && v->per8 > 0) {    /* 1.8: at most 7 at once (8 a voice); about two sound as one */
        uint32_t per = (uint32_t)v->per8 >> 8, gfr = eng.gframes, amp;
        if (per < 1u) per = 1u;
        if (gfr > 7u * per) { gfr = 7u * per; g->winc = 0xFFFFFFFFu / gfr; }
        amp = 2u * per * 32768u / (gfr ? gfr : 1u);   /* fewer than two at once: up to twice as loud */
        if (amp > 65536u) amp = 65536u;
        if (amp != 32768u) { g->gl = (g->gl * (int32_t)amp) >> 15; g->gr = (g->gr * (int32_t)amp) >> 15; }
    }
    g->delay = at;
    g->on = 1;
}

/* ---- the effects, at 22050 Hz: a ping-pong delay, then a plate reverb (Jon Dattorro's figure-eight tank,
 * "Effect Design part 1", 1997, its lengths scaled from 29761 Hz). Q15 signals, int16 lines. */
static inline int32_t sat16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }

#define DLY_N 22050u                                    /* 1 s a side */
static int16_t dly_l[DLY_N] POOL, dly_r[DLY_N] POOL;
static int32_t dly_w, dly_cur = 100 << 8, dly_lpl, dly_lpr, dly_hp;
static int32_t frac_read(const int16_t *b, int32_t n, int32_t w, int32_t d)   /* d samples back (Q8), interpolated */
{
    int32_t r = w - (d >> 8), f = d & 255, a, c;
    while (r < 0) r += n;
    a = b[r];
    r = r ? r - 1 : n - 1;
    c = b[r];
    return a + (((c - a) * f) >> 8);
}

typedef struct { int16_t *b; int32_t n, w; } line_t;
/* the reverb's lines live in RAM (.bss), the pool is full with the sound and the delay */
static int16_t rv_mem[105 + 79 + 281 + 205 + (498 + 28) + 3300 + 1334 + 2757 + (673 + 28) + 3125 + 1968 + 2344];
static line_t ap_in[4], ap_ml, d_l1, ap_l2, d_l2, ap_mr, d_r1, ap_r2, d_r2;
static int32_t rv_lpl, rv_lpr, rv_endl, rv_endr, rv_hp, rv_lfo;

static void line_init(line_t *l, int16_t **mem, int32_t n) { l->b = *mem; l->n = n; l->w = 0; *mem += n; }
static void reverb_init(void)
{
    int16_t *m = rv_mem;
    line_init(&ap_in[0], &m, 105); line_init(&ap_in[1], &m, 79);
    line_init(&ap_in[2], &m, 281); line_init(&ap_in[3], &m, 205);
    line_init(&ap_ml, &m, 498 + 28); line_init(&d_l1, &m, 3300); line_init(&ap_l2, &m, 1334); line_init(&d_l2, &m, 2757);
    line_init(&ap_mr, &m, 673 + 28); line_init(&d_r1, &m, 3125); line_init(&ap_r2, &m, 1968); line_init(&d_r2, &m, 2344);
}
static inline int32_t tap(const line_t *l, int32_t k)  /* k samples behind the next write */
{
    int32_t r = l->w - k;
    if (r < 0) r += l->n;
    return l->b[r];
}
static inline int32_t delay_step(line_t *l, int32_t x)  /* n samples of delay */
{
    int32_t y = l->b[l->w];
    l->b[l->w] = (int16_t)sat16(x);
    if (++l->w >= l->n) l->w = 0;
    return y;
}
static inline int32_t allpass(line_t *l, int32_t x, int32_t g)   /* g Q15; negative for Dattorro's decay diffusion 1 */
{
    int32_t z = l->b[l->w], v = sat16(x - ((g * z) >> 15));
    l->b[l->w] = (int16_t)v;
    if (++l->w >= l->n) l->w = 0;
    return z + ((g * v) >> 15);
}
static inline int32_t allpass_mod(line_t *l, int32_t x, int32_t g, int32_t d)   /* read d samples back (Q8) */
{
    int32_t z = frac_read(l->b, l->n, l->w - 1 < 0 ? l->n - 1 : l->w - 1, d - 256), v = sat16(x - ((g * z) >> 15));
    l->b[l->w] = (int16_t)v;
    if (++l->w >= l->n) l->w = 0;
    return z + ((g * v) >> 15);
}
static int32_t lfo_sin(uint32_t ph)                     /* -32767..32767, a parabola */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x;
    return sat16((x * (32768 - ax)) >> 13);
}

/* the shimmer: the tank's sound pitched (two taps sliding through a short line, crossfaded), darkened, and fed back
 * into the reverb's input, so each pass climbs (or falls) another interval */
#define SH_N 2048u
static int16_t sh_buf[SH_N];
static uint32_t sh_w, sh_ph;
static int32_t sh_lp, sh_hp, sh_fb;
static int32_t sh_read(int32_t d8)           /* d8 samples back, Q8 */
{
    int32_t r = (int32_t)sh_w - (d8 >> 8), f = d8 & 255, a, b;
    a = sh_buf[(uint32_t)r & (SH_N - 1u)];
    b = sh_buf[(uint32_t)(r - 1) & (SH_N - 1u)];
    return a + (((b - a) * f) >> 8);
}
static int32_t shimmer(int32_t x)
{
    int32_t w = eng.sh_win, d1, d2, g1, g2, y;
    uint32_t p2;
    if (!eng.sh_amt) { sh_lp = sh_hp = sh_fb = 0; return 0; }
    sh_buf[++sh_w & (SH_N - 1u)] = (int16_t)sat16(x);
    sh_ph += (uint32_t)eng.sh_step;          /* Q24 of the window: the delay slides, the pitch moves */
    sh_ph &= 0xFFFFFFu;
    p2 = ((sh_ph >> 8) + 0x8000u) & 0xFFFFu;
    d1 = (int32_t)(((uint32_t)(0xFFFFu - (sh_ph >> 8)) * (uint32_t)w) >> 8);   /* Q8 samples */
    d2 = (int32_t)(((uint32_t)(0xFFFFu - p2) * (uint32_t)w) >> 8);
    g1 = (sh_ph >> 8) < 0x8000u ? (int32_t)(sh_ph >> 8) : (int32_t)(0xFFFFu - (sh_ph >> 8));  /* triangles: silent where a tap jumps */
    g2 = p2 < 0x8000u ? (int32_t)p2 : (int32_t)(0xFFFFu - p2);
    y = (sh_read(d1 + 512) * g1 + sh_read(d2 + 512) * g2) >> 15;
    sh_lp += ((y - sh_lp) * eng.sh_tone) >> 15;
    sh_hp += (sh_lp - sh_hp) >> 4;            /* ~220 Hz high-pass: the lows would gather round the loop */
    return ((sh_lp - sh_hp) * eng.sh_amt) >> 15;
}

/* NASTRO (1.4): the delay's second of sound a side, played five ways (n_mode):
 *   0 ECO       the ping-pong delay, its time gliding like tape; VARIAZIONE a slow wow
 *   1 RIPETI    the beat repeat: at each slice of TEMPO (on the clock's grid), by chance (VARIAZIONE), the last slice plays
 *               again instead of the sound, RITORNO times; each repeat may be half as long (PASSO under 40) or lower (over 60)
 *   2 INDIETRO  echoes played backwards: two heads read chunks of TEMPO in reverse, crossfaded; VARIAZIONE jitters the chunks
 *   3 NASTRO    a tape echo: wow and flutter (VARIAZIONE), repeats pitched by PASSO (+-12 st) a pass, warm in the loop
 *   4 CONGELA   the second stops being written and the last TEMPO of it loops, at PASSO's pitch; VARIAZIONE drifts it
 * The line is written at 22050 Hz; the output is the wet sound, and *duck how much of the dry stays (RIPETI). */
static volatile uint32_t st_sync;           /* NASTRO RIPETI: the clock started, the slices from here */
static int32_t nr_gph, nr_on, nr_rep, nr_from, nr_ph, nr_sub, nr_rate, nr_g;   /* RIPETI */
static int32_t ni_ph, ni_len;                                                     /* INDIETRO */
static uint32_t nt_wow, nt_flt, nt_s;                                             /* NASTRO, ECO's wow */
static int32_t nc_on, nc_end, nc_start, nc_ph, nc_fill;                          /* CONGELA */
static int32_t nst_prev = -1;
static inline int32_t rd_abs(const int16_t *b, int32_t pos8)   /* the line at an absolute place, Q8, interpolated */
{
    int32_t i = pos8 >> 8, f = pos8 & 255, a, c;
    i %= (int32_t)DLY_N; if (i < 0) i += (int32_t)DLY_N;
    a = b[i];
    c = b[i + 1 < (int32_t)DLY_N ? i + 1 : 0];
    return a + (((c - a) * f) >> 8);
}
static inline int32_t pitch_tap(const int16_t *b, int32_t d8, uint32_t s)   /* two heads around d8 sliding by s (Q16 of 1024) */
{
    uint32_t s2 = (s + 0x8000u) & 0xFFFFu;
    int32_t g1 = s < 0x8000u ? (int32_t)s : (int32_t)(0xFFFFu - s), g2 = s2 < 0x8000u ? (int32_t)s2 : (int32_t)(0xFFFFu - s2);
    int32_t a = frac_read(b, (int32_t)DLY_N, dly_w, d8 + (int32_t)(s >> 6) * 1024 / 4);
    int32_t c = frac_read(b, (int32_t)DLY_N, dly_w, d8 + (int32_t)(s2 >> 6) * 1024 / 4);
    return (a * g1 + c * g2) >> 15;
}
static void fx_frame(int32_t inl, int32_t inr, int32_t *ol, int32_t *or_, int32_t *duck)
{
    int32_t rl = 0, rr = 0, wl, wr, mono, x, a, dec = eng.r_dec, m, tl, mode = eng.n_mode;
    mono = (inl + inr) >> 1;
    dly_hp += (mono - dly_hp) >> 7;                     /* ~27 Hz high-pass on what goes in */
    *duck = 32768;
    if (mode != nst_prev) { nst_prev = mode; nr_on = 0; nr_gph = 0; nc_on = 0; nc_fill = 0; ni_ph = 0; }
    if (++dly_w >= (int32_t)DLY_N) dly_w = 0;
    switch (mode) {
    case 1: {                                           /* RIPETI */
        int32_t L = eng.n_len, e;
        if (st_sync) { st_sync = 0; nr_gph = L; nr_on = 0; }
        if (++nr_gph >= L) {                            /* on the grid: a roll may start */
            nr_gph = 0;
            if (!nr_on && (int32_t)(xrand() & 0xFFFFu) < eng.n_var) {
                nr_on = 1; nr_rep = 1; nr_from = dly_w - L; nr_ph = 0; nr_sub = L; nr_rate = 65536;
            }
        }
        if (nr_on && nr_ph >= nr_sub) {                 /* a repeat ends: the next, shorter or lower, or back to the sound */
            if (++nr_rep > eng.n_reps || (eng.n_caso && (int32_t)(xrand() & 0xFFFFu) >= eng.n_var)) nr_on = 0;
            else {
                nr_ph = 0;
                if (eng.n_short && nr_sub > 128) nr_sub >>= 1;
                nr_rate = (int32_t)(((int64_t)nr_rate * eng.n_pdrop) >> 16);
            }
        }
        if (!nr_on) { dly_l[dly_w] = (int16_t)sat16(inl); dly_r[dly_w] = (int16_t)sat16(inr); break; }
        a = (int32_t)(((int64_t)nr_ph * nr_rate) >> 8);   /* Q8 into the slice */
        rl = rd_abs(dly_l, (nr_from << 8) + a);
        rr = rd_abs(dly_r, (nr_from << 8) + a);
        e = nr_ph < nr_sub - nr_ph ? nr_ph : nr_sub - nr_ph;   /* 2 ms in and out */
        nr_g = (e >= 44 ? 32767 : e * 745) * (eng.d_mix >> 0) >> 15;
        *duck = 32768 - nr_g;
        rl = (rl * nr_g) >> 15; rr = (rr * nr_g) >> 15;
        nr_ph++;
        dly_lpl += ((rl - dly_lpl) * eng.d_tone) >> 15; dly_lpr += ((rr - dly_lpr) * eng.d_tone) >> 15;
        rl = dly_lpl; rr = dly_lpr;
        *ol = rl; *or_ = rr;
        goto reverb; }
    case 2: {                                           /* INDIETRO */
        int32_t L, ph2, g1, g2, d1, d2;
        if (!ni_len) ni_len = eng.n_len;
        L = ni_len;
        if (++ni_ph >= L) {                             /* a new chunk: its length, jittered by VARIAZIONE */
            int32_t j = eng.n_var ? (int32_t)(((int64_t)((int32_t)(xrand() & 0xFFFFu) - 32768) * eng.n_var) >> 17) : 0;
            ni_ph = 0;
            ni_len = eng.n_len + (int32_t)(((int64_t)eng.n_len * j) >> 16);
            if (ni_len < 256) ni_len = 256;
            if (ni_len > (int32_t)DLY_N / 2 - 4) ni_len = (int32_t)DLY_N / 2 - 4;
            L = ni_len;
        }
        ph2 = ni_ph + L / 2; if (ph2 >= L) ph2 -= L;
        g1 = (ni_ph < L / 2 ? ni_ph : L - ni_ph) * 65534 / L; g2 = 32767 - g1;
        d1 = (2 * ni_ph + 1) << 8; d2 = (2 * ph2 + 1) << 8;
        rl = (frac_read(dly_l, (int32_t)DLY_N, dly_w, d1) * g1 + frac_read(dly_l, (int32_t)DLY_N, dly_w, d2) * g2) >> 15;
        rr = (frac_read(dly_r, (int32_t)DLY_N, dly_w, d1) * g1 + frac_read(dly_r, (int32_t)DLY_N, dly_w, d2) * g2) >> 15;
        dly_lpl += ((rl - dly_lpl) * eng.d_tone) >> 15; dly_lpr += ((rr - dly_lpr) * eng.d_tone) >> 15;
        dly_l[dly_w] = (int16_t)sat16(inl - dly_hp + ((dly_lpl * eng.d_fb) >> 15));
        dly_r[dly_w] = (int16_t)sat16(inr - dly_hp + ((dly_lpr * eng.d_fb) >> 15));
        rl = dly_lpl; rr = dly_lpr;
        break; }
    case 3: {                                           /* NASTRO */
        int32_t d8, w;
        dly_cur += (eng.d_time - dly_cur) >> 10;
        nt_wow += 97391u; nt_flt += 1266083u;           /* 0.5 Hz, 6.5 Hz */
        w = (int32_t)(((int64_t)lfo_sin(nt_wow) * eng.n_var) >> 15) * 55 / 256 + (int32_t)(((int64_t)lfo_sin(nt_flt) * eng.n_var) >> 15) * 6 / 256;
        d8 = dly_cur + w * 256 / 128;
        if (d8 < 768) d8 = 768;
        if (eng.n_ratio != 65536) {                     /* the repeats pitched: two heads sliding through 1024 samples */
            nt_s = (nt_s + (uint32_t)((65536 - eng.n_ratio) / 4)) & 0xFFFFFFu;   /* Q24 of the 1024 samples */
            if (d8 > (int32_t)((DLY_N - 1100u) << 8)) d8 = (int32_t)((DLY_N - 1100u) << 8);
            rl = pitch_tap(dly_l, d8, nt_s >> 8); rr = pitch_tap(dly_r, d8, nt_s >> 8);
        } else {
            rl = frac_read(dly_l, (int32_t)DLY_N, dly_w, d8); rr = frac_read(dly_r, (int32_t)DLY_N, dly_w, d8);
        }
        dly_lpl += ((rl - dly_lpl) * eng.d_tone) >> 15; dly_lpr += ((rr - dly_lpr) * eng.d_tone) >> 15;
        wl = dly_lpl - (((dly_lpl >> 8) * (dly_lpl >> 8) >> 8) * (dly_lpl >> 8) >> 11);   /* tape: a soft knee */
        wr = dly_lpr - (((dly_lpr >> 8) * (dly_lpr >> 8) >> 8) * (dly_lpr >> 8) >> 11);
        dly_l[dly_w] = (int16_t)sat16(mono - dly_hp + ((wl * eng.d_fb) >> 15));
        dly_r[dly_w] = (int16_t)sat16(mono - dly_hp + ((wr * eng.d_fb) >> 15));
        rl = dly_lpl; rr = dly_lpr;
        break; }
    case 4: {                                           /* CONGELA */
        int32_t L = eng.n_len, F, g, p8;
        if (!nc_on) {                                   /* armed: the next TEMPO of sound is recorded, then held */
            dly_l[dly_w] = (int16_t)sat16(inl); dly_r[dly_w] = (int16_t)sat16(inr);
            if (!eng.d_mix) { nc_fill = 0; break; }     /* MIX at 0: CONGELA waits, and takes a new loop when it rises */
            if (++nc_fill < L + 1100) break;
            nc_on = 1; nc_end = dly_w; nc_start = nc_end - L; nc_ph = 0;
        }
        if (!eng.d_mix) { nc_on = 0; nc_fill = 0; break; }
        dly_w = nc_end;                                 /* frozen: the write stands still */
        F = L / 4 < 1024 ? L / 4 : 1024;
        p8 = (nc_start << 8) + nc_ph;
        rl = rd_abs(dly_l, p8); rr = rd_abs(dly_r, p8);
        if (nc_ph >= (L - F) << 8) {                    /* the loop's end crossfaded with what came before its start */
            g = (((L << 8) - nc_ph) << 7) / F;      /* Q15 (32 bit: no 64-bit division on the FM-1) */
            if (g > 32767) g = 32767;
            rl = (rl * g + rd_abs(dly_l, p8 - (L << 8)) * (32767 - g)) >> 15;
            rr = (rr * g + rd_abs(dly_r, p8 - (L << 8)) * (32767 - g)) >> 15;
        }
        nc_ph += eng.n_ratio >> 8;
        if (nc_ph >= L << 8) {                          /* round: VARIAZIONE moves the loop a little back in the second */
            nc_ph -= L << 8;
            nc_start = nc_end - L;
            if (eng.n_var) nc_start -= (int32_t)(((int64_t)(((int32_t)(xrand() & 0xFFFFu) * (eng.n_var >> 1)) >> 15) * (L / 2)) >> 15);
            if (nc_start < nc_end - (int32_t)DLY_N + F + 4) nc_start = nc_end - (int32_t)DLY_N + F + 4;
        }
        dly_lpl += ((rl - dly_lpl) * eng.d_tone) >> 15; dly_lpr += ((rr - dly_lpr) * eng.d_tone) >> 15;
        rl = dly_lpl; rr = dly_lpr;
        break; }
    default: {                                          /* ECO: the delay as it was, its time gliding (a tape's bend) */
        int32_t w = eng.n_var ? (int32_t)(((int64_t)lfo_sin(nt_wow += 116877u) * eng.n_var) >> 15) * 33 / 128 : 0;
        dly_cur += (eng.d_time - dly_cur) >> 10;
        rl = frac_read(dly_l, (int32_t)DLY_N, dly_w, dly_cur + w);
        rr = frac_read(dly_r, (int32_t)DLY_N, dly_w, dly_cur + w);
        dly_lpl += ((rr - dly_lpl) * eng.d_tone) >> 15;     /* L takes R's repeat and R takes L's: ping-pong */
        dly_lpr += ((rl - dly_lpr) * eng.d_tone) >> 15;
        wl = sat16(mono - dly_hp + ((dly_lpl * eng.d_fb) >> 15));
        wr = sat16((dly_lpr * eng.d_fb) >> 15);
        dly_l[dly_w] = (int16_t)wl;
        dly_r[dly_w] = (int16_t)wr;
        break; }
    }
    rl = (rl * eng.d_mix) >> 15;
    rr = (rr * eng.d_mix) >> 15;
    *ol = rl;
    *or_ = rr;
reverb:
    /* the reverb: the dry sound and the repeats, mono, through four diffusers into the tank */
    x = (mono + ((rl + rr) >> 1)) >> 1;
    rv_hp += (x - rv_hp) >> 6;                          /* ~55 Hz high-pass: no mud */
    x -= rv_hp;
    x = sat16(x + sh_fb);                               /* the shimmer comes back in here */
    x = allpass(&ap_in[0], x, 24576);                   /* 0.75 */
    x = allpass(&ap_in[1], x, 24576);
    x = allpass(&ap_in[2], x, 20480);                   /* 0.625 */
    x = allpass(&ap_in[3], x, 20480);
    rv_lfo += 155836;                                   /* 0.8 Hz: the tail's slow swirl */
    m = eng.r_mod;
    /* the left half of the tank */
    a = sat16(x + ((rv_endr * dec) >> 15));
    a = allpass_mod(&ap_ml, a, -22938, (498 << 8) + 14 * 256 + ((lfo_sin((uint32_t)rv_lfo) * m) >> 15));     /* -0.7 */
    a = delay_step(&d_l1, a);
    rv_lpl += ((a - rv_lpl) * eng.r_tone) >> 15;
    a = (rv_lpl * dec) >> 15;
    a = allpass(&ap_l2, a, 16384);                      /* 0.5 */
    rv_endl = delay_step(&d_l2, a);
    /* the right half, fed by the left */
    a = sat16(x + ((rv_endl * dec) >> 15));
    a = allpass_mod(&ap_mr, a, -22938, (673 << 8) + 14 * 256 + ((lfo_sin((uint32_t)rv_lfo + 0x40000000u) * m) >> 15));
    a = delay_step(&d_r1, a);
    rv_lpr += ((a - rv_lpr) * eng.r_tone) >> 15;
    a = (rv_lpr * dec) >> 15;
    a = allpass(&ap_r2, a, 16384);
    rv_endr = delay_step(&d_r2, a);
    /* the outputs: seven taps each across the tank */
    a = tap(&d_r1, 197) + tap(&d_r1, 2204) - tap(&ap_r2, 1418) + tap(&d_r2, 1479)
      - tap(&d_l1, 1475) - tap(&ap_l2, 139) - tap(&d_l2, 790);
    tl = (a * 19661) >> 15;
    *ol += (tl * eng.r_mix) >> 15;                      /* x 0.6 */
    a = tap(&d_l1, 262) + tap(&d_l1, 2688) - tap(&ap_l2, 910) + tap(&d_l2, 1981)
      - tap(&d_r1, 1564) - tap(&ap_r2, 248) - tap(&d_r2, 90);
    a = (a * 19661) >> 15;
    *or_ += (a * eng.r_mix) >> 15;
    sh_fb = shimmer((tl + a) >> 1);
}
static int32_t fx_prevl, fx_prevr;

/* ---- the clock (96 ticks a quarter note), the arpeggiator and the sequencer: they run in the ISR, so their
 * notes start on the frame they fall on. The arpeggiator plays voice 2, the sequencer voice 3; the drone has
 * voice 0. The main loop writes the notes and the steps, the ISR reads them. */
#define V_DRONE 0u
#define V_ARP 2u
#define V_SEQ 3u
#define NSTEP 64u
static volatile uint8_t seq_note[NSTEP];    /* 0 rest, 1 tie, else the MIDI note */
static volatile uint8_t seq_pos[NSTEP];     /* P_POS's scale (0..200); 255 = the knob's */
static struct {
    volatile uint32_t inc;                  /* the tick's phase step per frame */
    volatile uint32_t run;                  /* the sequencer runs (PLAY) */
    volatile uint32_t arp;                  /* the arpeggiator is on */
    volatile uint32_t sdiv, slen, swing, adiv, amode, aoct, agate, ogni;
    volatile uint32_t restart;              /* PLAY pressed: start from step 1, in step with the arpeggiator */
    volatile uint32_t ext;                  /* 1.2: CLOCK IN ESTERNO: the ticks follow the MIDI clock */
    volatile uint32_t out;                  /* 1.2: CLOCK OUT: 24 a quarter note to USB */
} clk;
static volatile uint32_t ext_ticks, clk_int;   /* the MIDI clock's ticks (x 4: ours are 96 a quarter), and ours that followed */
static volatile uint32_t ext_bpm10 = 1200;     /* its tempo, tenths of BPM */
static uint32_t clk_odiv;
static volatile uint8_t arp_ord[16], arp_up[16];   /* the notes held: as played, and sorted */
static volatile uint32_t arp_n, arp_gen;
static volatile int32_t seq_play = -1;      /* the step playing (the screen), -1 = stopped */
static volatile uint32_t dice_req;          /* the ISR asks the main loop to throw the dice */
static uint32_t clk_acc, s_tick, s_step, s_len, s_off, a_tick, a_k, a_len, a_off, a_gen, d_tick, was_run, was_arp;

static void isr_note(uint32_t v, uint32_t note, int32_t pos, uint32_t at)
{
    voice_t *vo = &voice[v];
    vo->note = (uint8_t)note;
    vo->base_ct = ((int32_t)snap(note) - 60) * 100;   /* 1.6: in the scale (the note itself kept, for its note off) */
    vo->pos_lock = pos;
    vo->zone = (int8_t)zone_of(note);
    vo->vel = 28000;
    vo->start = (uint8_t)at;
    if (vo->gate) vo->restart = 1;
    vo->gate = 1;
}

static void seq_tick(uint32_t at, uint32_t len)   /* a tick of the sequencer, at frame `at` */
{
    if (s_tick == 0) {                      /* a step begins */
        uint32_t nx = s_step, d = DIV_TICKS[clk.sdiv % 5u], sw = d * clk.swing / 100u;
        uint32_t nt = seq_note[nx];
        s_len = nx & 1u ? d - sw : d + sw;  /* the swing: the even steps longer, the odd shorter */
        seq_play = (int32_t)nx;
        if (nt >= 2u) {
            uint32_t ps = seq_pos[nx];
            isr_note(V_SEQ, nt, ps == 255u ? -1 : (int32_t)ps, at);
        } else if (nt == 0u)
            voice[V_SEQ].gate = 0;
        nx = (nx + 1u) % (clk.slen ? clk.slen : 1u);
        s_off = seq_note[nx] == 1u ? 0xFFFFu : s_len * 3u / 4u;   /* the next a tie: no release */
    }
    if (s_tick == s_off) voice[V_SEQ].gate = 0;
    if (++s_tick >= s_len) {
        s_tick = 0;
        s_step = (s_step + 1u) % (clk.slen ? clk.slen : 1u);
    }
}

static void arp_tick(uint32_t at)
{
    uint32_t n = arp_n;
    if (n > 16u) n = 16u;
    if (arp_gen != a_gen) { a_gen = arp_gen; a_k = 0xFFFFFFFFu; }
    if (!n) { voice[V_ARP].gate = 0; a_tick = 0; return; }
    if (a_tick == 0) {
        uint32_t oct = clk.aoct + 1u, tot = n * oct, k, i, note;
        a_len = DIV_TICKS[clk.adiv % 5u];
        a_k = a_k == 0xFFFFFFFFu ? 0u : a_k + 1u;
        switch (clk.amode) {
        case 1: k = tot - 1u - a_k % tot; i = k % n; note = arp_up[i] + 12u * (k / n); break;           /* down */
        case 2: {                                                                                     /* up-down */
            uint32_t per = tot > 1u ? 2u * tot - 2u : 1u, q = a_k % per;
            k = q < tot ? q : per - q; i = k % n; note = arp_up[i] + 12u * (k / n); break; }
        case 3: k = a_k % tot; i = k % n; note = arp_ord[i] + 12u * (k / n); break;                  /* as played */
        case 4: k = xrand() % tot; i = k % n; note = arp_up[i] + 12u * (k / n); break;               /* random */
        default: k = a_k % tot; i = k % n; note = arp_up[i] + 12u * (k / n); break;                  /* up */
        }
        if (note > 127u) note = 127u;
        isr_note(V_ARP, note, -1, at);
        a_off = a_len * clk.agate / 100u;
        if (a_off < 1u) a_off = 1u;
    }
    if (a_tick == a_off && a_off < a_len) voice[V_ARP].gate = 0;
    if (++a_tick >= a_len) a_tick = 0;
}

static void clock_block(uint32_t n, uint32_t len)
{
    uint32_t i;
    if (clk.restart) {                      /* PLAY: both from the top */
        clk.restart = 0;
        st_sync = 1;                        /* NASTRO RIPETI's slices too */
        s_tick = s_step = 0;
        a_tick = 0;
        a_k = 0xFFFFFFFFu;
        clk_acc = 0xFFFFFFFFu;              /* the first tick on the first frame */
    }
    if (was_run && !clk.run) { voice[V_SEQ].gate = 0; seq_play = -1; }
    if (was_arp && !clk.arp) voice[V_ARP].gate = 0;
    was_run = clk.run;
    was_arp = clk.arp;
    for (i = 0; i < n; i++) {
        uint32_t prev = clk_acc;
        clk_acc += clk.inc;
        if (clk.ext) {                      /* ESTERNO: our ticks at the clock's tempo, never ahead of it, caught up if behind */
            int32_t lag = (int32_t)(ext_ticks - clk_int);
            if (clk_acc >= prev) { if (lag <= 8) continue; }
            else if (lag <= 0) { clk_acc = prev; continue; }   /* on time: wait for the next clock at the edge */
            clk_int++;
        } else if (clk_acc >= prev) continue;   /* no tick on this frame */
        if (clk.out && clk_odiv++ % 4u == 0u) midi_clock_out(0xF8u);   /* CLOCK OUT: every fourth of ours */
        if (clk.run) seq_tick(i, len);
        if (clk.arp) arp_tick(i);
        if (clk.ogni && ++d_tick >= 24u * clk.ogni) { d_tick = 0; dice_req++; }
    }
}

/* ---- the filter: a TPT state variable filter (Andrew Simper's), stereo at 44.1 kHz, its cutoff moved by a slow
 * LFO (two sines, 0.12 and 0.077 Hz, so it never quite repeats). The states carry 4 extra bits. */
static int32_t f_s1[2][2], f_s2[2][2];      /* the state variable filters' two stages, a pair a side */
typedef struct { int64_t a1, a2, a3; } svf_c;
static uint32_t f_ph1, f_ph2;
static volatile int32_t f_vis;              /* the cutoff step now, for the screen */
static inline int32_t clamp24(int32_t x) { return x > (1 << 24) ? (1 << 24) : x < -(1 << 24) ? -(1 << 24) : x; }
static int32_t lad[2][4];                   /* the ladder's four stages, Q4 */
static int64_t recip28(int64_t d)           /* 1/d, d in Q28 from 1 to 16: 16 bits by division, 28 by a Newton step */
{
    int64_t a = (int64_t)(0xFFFFFFFFu / (uint32_t)(d >> 12)) << 12, err = (1 << 28) - ((d * a) >> 28);
    return a + ((a * err) >> 28);
}
static void svf_coef(svf_c *c, int64_t g, int64_t k)   /* g, k Q28 */
{
    c->a1 = recip28((1 << 28) + ((g * (g + k)) >> 28));
    c->a2 = (g * c->a1) >> 28;
    c->a3 = (g * c->a2) >> 28;
}
static inline int32_t svf_step(const svf_c *c, int32_t x, int32_t *st, int32_t *bp)   /* the low pass; *bp the band */
{
    int32_t v3 = x - st[1], v1, v2;
    v1 = (int32_t)((c->a1 * st[0] + c->a2 * v3) >> 28);
    v2 = st[1] + (int32_t)((c->a2 * st[0] + c->a3 * v3) >> 28);
    st[0] = clamp24(2 * v1 - st[0]);
    st[1] = clamp24(2 * v2 - st[1]);
    *bp = v1;
    return v2;
}
static inline int32_t sat_q4(int32_t x)     /* a soft clip to twice full scale (Q4: 1 << 20): x - 4x^3/27, flat from 1.5 */
{
    const int64_t L = 1 << 20;
    int64_t c = x;
    if (c >= 3 * L / 2) return (int32_t)L;
    if (c <= -3 * L / 2) return -(int32_t)L;
    return (int32_t)(c - (((((c * c) >> 20) * c) >> 20) * 4855 >> 15));
}
static void filter_block(int32_t *out, uint32_t n)
{
    uint32_t i, j, c, mode = (uint32_t)eng.f_mode;
    int64_t k = (int64_t)eng.f_k << 12;     /* Q28 */
    int32_t gin = eng.f_in;                 /* the SVF low pass: the resonance's peak tamed */
    f_ph1 += 2989000u;                      /* per block of 256: 0.12 Hz */
    f_ph2 += 1918000u;                      /* 0.077 Hz */
    if (!mode) {
        for (c = 0; c < 4u; c++) { f_s1[c >> 1][c & 1u] = 0; f_s2[c >> 1][c & 1u] = 0; }
        for (c = 0; c < 8u; c++) lad[c >> 2][c & 3u] = 0;
        f_vis = -1;
        return;
    }
    for (j = 0; j < n; j += 32u) {          /* the coefficients every 32 frames, Q28 */
        int32_t lfo = (2 * lfo_sin(f_ph1 + j * 11676u) + lfo_sin(f_ph2)) / 3, idx = eng.f_cut + ((lfo * eng.f_lfo) >> 15);
        int64_t g;
        if (idx < eng.f_lo) idx = eng.f_lo;
        if (idx > eng.f_lo + eng.f_span) idx = eng.f_lo + eng.f_span;
        if (idx < 0) idx = 0;
        if (idx > 1023) idx = 1023;
        f_vis = idx;
        g = SVF_G[idx];
        if (mode == 1u) {                   /* the ladder: four TPT one-poles, the feedback solved at once (Zavalishin),
                                             * the soft clip where the feedback meets the input */
            int64_t G = (g * recip28((1 << 28) + g)) >> 28, G2 = (G * G) >> 28, G3 = (G2 * G) >> 28, G4 = (G3 * G) >> 28;
            int64_t lk = (int64_t)eng.f_lk << 12, inv = recip28((1 << 28) + ((lk * G4) >> 28)), h = (1 << 28) - G;
            int64_t comp = (1 << 28) + ((lk * 154) >> 8);   /* the volume lost to the resonance, 0.6 of it given back */
            for (c = 0; c < 2u; c++) {
                int32_t *st = lad[c];
                for (i = j; i < j + 32u && i < n; i++) {
                    int32_t x = out[2u * i + c] * 16, u, y4, q, v;   /* Q4 */
                    int64_t sig = ((G3 * ((st[0] * h) >> 28)) >> 28) + ((G2 * ((st[1] * h) >> 28)) >> 28) +
                                  ((G * ((st[2] * h) >> 28)) >> 28) + ((st[3] * h) >> 28);
                    y4 = (int32_t)((((G4 * x) >> 28) + sig) * inv >> 28);
                    u = sat_q4(x - (int32_t)((lk * y4) >> 28));
                    for (q = 0; q < 4; q++) {
                        v = (int32_t)(((int64_t)(u - st[q]) * G) >> 28);
                        u = v + st[q];
                        st[q] = clamp24(u + v);
                    }
                    out[2u * i + c] = sat_q4((int32_t)((u * comp) >> 28)) >> 4;   /* made up after the filter */
                }
            }
            continue;
        }
        {                                   /* the state variable filters (Simper): one stage, or two for BANDA, ALTO 24DB, NOTCH */
            svf_c c1, c2;
            int64_t kn = (int64_t)eng.f_kn << 12;
            int32_t bg = eng.f_bg, hin = eng.f_hin;
            if (mode == 5u) {               /* NOTCH: the resonance widens it; a second one above deepens it, like a phaser at rest */
                int32_t i2 = idx + eng.f_n2;
                svf_coef(&c1, g, kn);
                svf_coef(&c2, SVF_G[i2 > 1023 ? 1023 : i2], kn);
            } else {
                svf_coef(&c1, g, k);
                svf_coef(&c2, g, mode == 3u ? (int64_t)1 << 28 : (int64_t)379625062);   /* BANDA: Q 1; ALTO 24DB: Q 0.707 */
            }
            for (c = 0; c < 2u; c++) {
                int32_t *s1 = f_s1[c], *s2 = f_s2[c];
                for (i = j; i < j + 32u && i < n; i++) {
                    int32_t x = out[2u * i + c] * 16, y, lp, bp;   /* Q4 */
                    switch (mode) {
                    case 2u:                                                             /* low pass, its peak tamed */
                        x = (int32_t)(((int64_t)out[2u * i + c] * gin) >> 12);
                        y = svf_step(&c1, x, s1, &bp);
                        break;
                    case 3u:                                                             /* band: a peak that grows with the
                                                                                          * resonance, then a gentle band again */
                        svf_step(&c1, x, s1, &bp);
                        x = (int32_t)(((int64_t)bp * bg) >> 16);
                        svf_step(&c2, x, s2, &bp);
                        y = bp;
                        break;
                    case 5u:                                                             /* notch, twice */
                        lp = svf_step(&c1, x, s1, &bp);
                        x = x - (int32_t)((kn * bp) >> 28);
                        lp = svf_step(&c2, x, s2, &bp);
                        y = x - (int32_t)((kn * bp) >> 28);
                        (void)lp;
                        break;
                    default:                                                             /* high pass, 12 or 24 dB */
                        x = (int32_t)(((int64_t)x * hin) >> 16);
                        lp = svf_step(&c1, x, s1, &bp);
                        y = x - (int32_t)((k * bp) >> 28) - lp;
                        if (mode == 6u) {
                            x = clamp24(y);
                            lp = svf_step(&c2, x, s2, &bp);
                            y = x - (int32_t)(((int64_t)379625062 * bp) >> 28) - lp;
                        }
                        break;
                    }
                    out[2u * i + c] = sat_q4(y) >> 4;
                }
            }
        }
    }
}

/* ---- MODULAZIONE (after the filter, on the grains): ring modulator, phaser (six all-passes swept), flanger (a short
 * line swept, with feedback), frequency shifter (a Hilbert pair of all-pass chains, Olli Niemitalo's, then the
 * analytic signal turned: every partial moves by the same Hz, the harmonics stop being harmonic) */
static inline int32_t sin_t(uint32_t ph)     /* Q15, the table interpolated */
{
    uint32_t i = ph >> 24, f = (ph >> 8) & 0xFFFFu;
    return SIN_T[i] + (int32_t)(((int64_t)(SIN_T[i + 1u] - SIN_T[i]) * (int32_t)f) >> 16);
}
static uint32_t md_ph, md_lfo;
#define PH_N 12u                            /* 1.2: twelve all-passes (six notches) */
static int32_t ph_z[2][PH_N], ph_last[2], ph_a[2], ph_lp[2];
static const int32_t HB_A[4] = {514752760, 940832443, 1048613677, 1071056671};   /* path I: a^2, Q30 */
static const int32_t HB_B[4] = {173686865, 787083823, 1015061512, 1063647745};   /* path Q */
static int32_t hb_x[2][2][4][2], hb_y[2][2][4][2], hb_d[2], fs_last[2];
static int32_t hb_path(int32_t x, int32_t c, int32_t pth, const int32_t *k)
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {               /* y = a^2 (x + y[n-2]) - x[n-2] */
        int32_t *xs = hb_x[c][pth][i], *ys = hb_y[c][pth][i], y;
        y = (int32_t)(((int64_t)k[i] * (x + ys[1])) >> 30) - xs[1];
        xs[1] = xs[0]; xs[0] = x;
        ys[1] = ys[0]; ys[0] = y;
        x = y;
    }
    return x;
}
/* VELVET (1.4): one line at 44.1 kHz, its taps swept by the LFO (the right a quarter turn on). CARATTERE goes from a
 * wide chorus (two taps, 15 and 23 ms +- 6) to a narrow one (8 ms), to a flanger (4 ms +- 3.8, its feedback rising),
 * then fades into the phaser (twelve all-passes, their corners between 100 Hz and 8 kHz). Half and half at MIX 100.
 * METALLO (1.4): a Hilbert pair (I, Q) turned by the carrier: I cos + k Q sin; k 0 is a ring modulator (both sidebands),
 * +1 every partial up by FREQUENZA, -1 down; RITORNO feeds it round (a spiral), its level made up for */
static uint32_t ch_w;
static void velvet_block(int32_t *out, uint32_t n)
{
    uint32_t i, c;
    int32_t mix = eng.m_mix, dep = eng.m_depth, vd = eng.v_d, vs = eng.v_s, t2 = eng.v_t2, fb = eng.v_fb, wp = eng.v_ph;
    for (i = 0; i < n; i++) {
        md_lfo += eng.m_inc;
        for (c = 0; c < 2u; c++) {
            int32_t x = out[2u * i + c], y = x, ycf = x, yph = x;
            uint32_t ph = md_lfo + (c ? 0x40000000u : 0u);
            if (wp < 32768) {               /* the line: chorus, flanger */
                int32_t sw = (int32_t)(((int64_t)sin_t(ph) * dep) >> 15), d8 = vd + (int32_t)(((int64_t)sw * vs) >> 15), r, f, a0, a1, y1;
                if (d8 < 512) d8 = 512;
                r = (int32_t)ch_w - (d8 >> 8); f = d8 & 255;
                a0 = mod_line[2u * ((uint32_t)r & (MODL_N - 1u)) + c]; a1 = mod_line[2u * ((uint32_t)(r - 1) & (MODL_N - 1u)) + c];
                y1 = a0 + (((a1 - a0) * f) >> 8);
                ycf = y1;
                if (t2) {                   /* the wide chorus's second voice, a third of a turn on, 8 ms further */
                    int32_t s2 = (int32_t)(((int64_t)sin_t(ph + 0x55555555u) * dep) >> 15), e8 = vd + (352 << 8) + (int32_t)(((int64_t)s2 * vs) >> 15), y2;
                    r = (int32_t)ch_w - (e8 >> 8); f = e8 & 255;
                    a0 = mod_line[2u * ((uint32_t)r & (MODL_N - 1u)) + c]; a1 = mod_line[2u * ((uint32_t)(r - 1) & (MODL_N - 1u)) + c];
                    y2 = a0 + (((a1 - a0) * f) >> 8);
                    ycf = y1 + (((y2 - y1) * (t2 >> 1)) >> 15);
                }
                mod_line[2u * (ch_w & (MODL_N - 1u)) + c] = (int16_t)sat16(x + ((y1 * fb) >> 15));
            }
            if (wp > 0) {                   /* the phaser */
                int32_t a, k, *z = ph_z[c], fbk;
                if ((i & 15u) == 0u) {
                    int32_t sw = sin_t(ph), idx = 556 + (int32_t)((((int64_t)sw * dep) >> 15) * 360 >> 15);
                    int64_t g = SVF_G[idx < 0 ? 0 : idx > 1023 ? 1023 : idx];
                    ph_a[c] = -(int32_t)((((int64_t)(1 << 28) - g) * recip28((1 << 28) + g)) >> 41);   /* (g - 1) / (g + 1), Q15 */
                }
                a = ph_a[c];
                fbk = ph_last[c] - ph_lp[c];  /* the feedback without its lows: the peaks sing, the bass stays put */
                ph_lp[c] += ((ph_last[c] - ph_lp[c]) * 700) >> 15;
                yph = sat16(x + ((fbk * eng.m_fb2) >> 15));
                for (k = 0; k < (int32_t)PH_N; k++) { int32_t o = ((a * yph) >> 15) + z[k]; z[k] = yph - ((a * o) >> 15); yph = o; }
                ph_last[c] = sat16(yph);
            }
            y = wp <= 0 ? ycf : wp >= 32768 ? yph : ycf + (((yph - ycf) * wp) >> 15);
            out[2u * i + c] = x + (((y - x) * mix) >> 16);   /* at most half and half */
        }
        ch_w++;
    }
}
static void metallo_block(int32_t *out, uint32_t n)
{
    uint32_t i, c;
    int32_t mix = eng.m_mix, k = eng.m_k;
    for (i = 0; i < n; i++) {
        md_ph += eng.m_inc;
        for (c = 0; c < 2u; c++) {
            int32_t x = out[2u * i + c], in = sat16(x + ((fs_last[c] * eng.m_fb) >> 15)), I, Q, y;
            I = hb_d[c]; hb_d[c] = hb_path(in, (int32_t)c, 0, HB_A);   /* path I a sample late */
            Q = hb_path(in, (int32_t)c, 1, HB_B);
            y = (int32_t)(((int64_t)I * sin_t(md_ph + 0x40000000u) + (((int64_t)Q * k) >> 15) * sin_t(md_ph)) >> 15);
            fs_last[c] = sat16(y);
            y = (y * eng.m_fsc) >> 15;   /* as loud with RITORNO up as without */
            out[2u * i + c] = x + ((y - x) * (mix >> 1) >> 14);
        }
    }
}
static void mod_block(int32_t *out, uint32_t n)
{
    if (!eng.m_type || !eng.m_mix) return;
    if (eng.m_type == 1) velvet_block(out, n);
    else metallo_block(out, n);
}

/* ---- the compressor, last: the loudest of the two sides followed (at once up, 190 ms down), above a threshold
 * that falls with the amount the gain comes down by the ratio (1.5 .. 8), and a third of the loss is made up */
static int32_t cp_env, cp_g = 4096;
static int32_t ilog2_q8(uint32_t x)          /* log2 x, Q8 */
{
    int32_t n = 0;
    if (!x) return 0;
    while (x >= 64u) { x >>= 1; n++; }
    while (x < 32u) { x <<= 1; n--; }
    return (n + 5) * 256 + LOG2T[x - 32u];
}
static int32_t iexp2_q12(int32_t y)          /* 2^(y/256), Q12, from silence to x 8 */
{
    int32_t i = y >> 8, m = EXP2T[(y & 255) >> 3];          /* m: Q15 of 2^(f/256) */
    if (i < -15) return 0;
    if (i > 2) i = 2;
    return i >= 3 ? m << (i - 3) : m >> (3 - i);
}
static void comp_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t amt = eng.comp, thr, red, mk, tg = cp_g;
    if (!amt) { cp_g = 4096; cp_env = 0; return; }
    thr = 15 * 256 - amt * 1024 / 100;      /* log2 of the threshold: 15 (0 dB) .. 11 (-24 dB) */
    for (i = 0; i < n; i++) {
        int32_t l = out[2u * i], r = out[2u * i + 1u], pk = l < 0 ? -l : l, pr2 = r < 0 ? -r : r;
        if (pr2 > pk) pk = pr2;
        if (pk > cp_env) cp_env = pk;                    /* at once: the grains are all attack */
        else cp_env -= cp_env >> 13;                     /* ~190 ms */
        if ((i & 3u) == 0u) {
            int32_t lv = ilog2_q8((uint32_t)cp_env + 1u), ratio_k = 32768 - 32768 * 10 / (15 + amt * 65 / 100);   /* 1 - 1/R, Q15 */
            red = lv > thr ? (int32_t)(((int64_t)(lv - thr) * ratio_k) >> 15) : 0;
            mk = (int32_t)(((int64_t)(15 * 256 - thr) * ratio_k) / 3 >> 15);     /* a third of the loss at full level */
            tg = iexp2_q12(mk - red);
        }
        if (tg < cp_g) cp_g = tg;                        /* down at once, back up slowly */
        else cp_g += (tg - cp_g) >> 10;
        out[2u * i] = (int32_t)(((int64_t)l * cp_g) >> 12);
        out[2u * i + 1u] = (int32_t)(((int64_t)r * cp_g) >> 12);
    }
}

static int32_t vbl[HALF_FRAMES], vbr[HALF_FRAMES];

/* 1.8, FORMANTI: the grains come at the note's rate (a period of the note each), each read at the sound's own pace:
 * the note is the pitch, the sound and its window the timbre (pitch-synchronous, the way formants are kept) */
static inline int32_t soft_sin(uint32_t ph)  /* Q15, a triangle bent toward a sine (3x - x^3) / 2 */
{
    int32_t t = (int32_t)(ph >> 15), x;      /* 0 .. 131071 */
    x = t < 65536 ? t - 32768 : 98303 - t;   /* -32768 .. 32767 */
    return (3 * x - (int32_t)(((int64_t)x * x >> 15) * x >> 15)) >> 1;
}
static int32_t note_per8(int32_t c)         /* a note (cents from C4) -> its period, Q8 output frames (168.6 at C4) */
{
    return (int32_t)(88371200u / cents_step(c));
}
static int32_t fo_next(voice_t *vo)         /* frames to the next grain */
{
    int32_t c = eng.pitch_ct + vo->base_ct, per, j;
    if (eng.fo_vib) c += (soft_sin(vo->vph) * eng.fo_vib) >> 15;
    per = note_per8(c);
    if (eng.fo_breath) {                     /* RESPIRO: the periods uneven, a breath in the voice */
        j = (int32_t)(((int64_t)per * eng.fo_breath) >> 21);   /* up to +-3 % a period */
        if (j > 0) per += (int32_t)(xrand() % (uint32_t)(2 * j + 1)) - j;
    }
    vo->per8 = per;
    per += (int32_t)vo->facc;
    vo->facc = (uint32_t)per & 255u;
    per >>= 8;
    return per < 1 ? 1 : per;
}

/* 1.8, RISONATORE: each voice a string (Karplus-Strong) tuned to its note, the grains plucking it: a low-pass
 * (TIMBRO) and an all-pass (DISPERSIONE: stretched partials, bells) in the loop, DECADIMENTO its feedback.
 * 512 frames a string: notes under F2 sound an octave up. */
#define RS_N 512
static int16_t rs_line[NV][RS_N];
static void rs_reset(uint32_t v)
{
    uint32_t i;
    for (i = 0; i < RS_N; i++) rs_line[v][i] = 0;
    voice[v].rs_w = 0; voice[v].rs_lp = 0; voice[v].rs_ax = 0; voice[v].rs_ay = 0;
}
static void rs_block(voice_t *vo, uint32_t v, uint32_t n)
{
    int16_t *L = rs_line[v];
    int32_t w = vo->rs_w, lp = vo->rs_lp, ax = vo->rs_ax, ay = vo->rs_ay, d8, comp;
    int32_t fb = eng.rs_fb, dm = eng.rs_damp, a = eng.rs_ap, mx = eng.rs_mix, dry = 32767 - mx, gin = eng.rs_gin;
    uint32_t i;
    d8 = note_per8(eng.pitch_ct + vo->base_ct);
    while (d8 > ((RS_N - 8) << 8)) d8 >>= 1;
    comp = ((32768 - dm) << 8) / dm + ((32768 - a) << 8) / (32768 + a);   /* the loop's own delay, Q8 */
    d8 -= comp;
    if (d8 < (2 << 8)) d8 = 2 << 8;
    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)(((int64_t)(vbl[i] + vbr[i]) * gin) >> 16), r = w - (d8 >> 8), f = d8 & 255, s0, s1, y, o;
        s0 = L[r & (RS_N - 1)];
        s1 = L[(r - 1) & (RS_N - 1)];
        y = s0 + (((s1 - s0) * f) >> 8);
        lp += ((y - lp) * dm) >> 15;
        y = ((-a * lp) >> 15) + ax + ((a * ay) >> 15);
        ax = lp; ay = y;
        y = ((y * fb) >> 15) + x;
        y = y > 32767 ? 32767 : y < -32767 ? -32767 : y;
        L[w] = (int16_t)y;
        w = (w + 1) & (RS_N - 1);
        o = y;
        vbl[i] = (int32_t)(((int64_t)vbl[i] * dry + (int64_t)o * mx) >> 15);
        vbr[i] = (int32_t)(((int64_t)vbr[i] * dry + (int64_t)o * mx) >> 15);
    }
    vo->rs_w = w; vo->rs_lp = lp; vo->rs_ax = ax; vo->rs_ay = ay;
}
static uint32_t crush_hold;
static int32_t crush_l, crush_r;

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t len = 0, v, k, i, t0 = fm1_ticks();
    int32_t gain = eng.gain;
    const int16_t *win = WINDOWS + eng.win;
    if (n > HALF_FRAMES) n = HALF_FRAMES;
    for (k = 0; k < NL; k++) if (lay[k].len >= 64u) len = lay[k].len;
    if (len < 64u) {                        /* nothing loaded: everything stops */
        for (v = 0; v < NV; v++) {
            for (k = 0; k < NG; k++) voice[v].g[k].on = 0;
            voice[v].active = 0;
            voice[v].env = 0;
            vis_head[v] = -1;
        }
        for (k = 0; k < NV * NG; k++) vis_pos[k] = -1;
        return;
    }
    if (!eng.freeze) ghead += (uint32_t)eng.scan_ph * n;   /* the shared scan, around the area */
    vis_head[NV] = eng.dir == 3u ? -1 : (int32_t)(scan_at(ghead) >> 8);
    clock_block(n, len);

    for (v = 0; v < NV; v++) {
        voice_t *vo = &voice[v];
        int32_t env = vo->env, vel = vo->vel;
        if (vo->restart) { vo->restart = 0; vo->active = 0; }   /* the arpeggiator's or the sequencer's next note */
        if (vo->gate && !vo->active) {      /* a new note: its first grain on the frame it falls on */
            vo->active = 1;
            vo->head = (eng.retrig ? 0 : ghead) + salto_acc;   /* SALTO: each new note a step further on */
            salto_acc += eng.salto;
            vo->timer = vo->start;
            vo->start = 0;
            if (eng.retrig) env = 0;
            if (eng.mot == MO_FORMANTI) { vo->facc = 0; vo->vph = 0; (void)fo_next(vo); vo->facc = 0; }   /* 1.8 */
            if (eng.mot == MO_RISON) rs_reset(v);
        }
        if (!vo->active) { vis_head[v] = -1; continue; }

        /* the grains starting in this block */
        while (vo->gate && vo->timer < (int32_t)n) {
            spawn(vo, (uint32_t)(vo->timer < 0 ? 0 : vo->timer));
            if (eng.mot == MO_FORMANTI) vo->timer += fo_next(vo);   /* 1.8: the note's period */
            else vo->timer += eng.period + (int32_t)(xrand() % (uint32_t)(eng.period / 5 + 1)) - eng.period / 10;
        }
        vo->timer -= (int32_t)n;
        vo->vph += (uint32_t)eng.fo_vinc * n;
        if (!eng.freeze) vo->head += (uint32_t)eng.scan_ph * n;
        vis_head[v] = eng.dir == 3u ? -1 : (int32_t)(scan_at(vo->head) >> 8);

        for (i = 0; i < n; i++) vbl[i] = vbr[i] = 0;
        for (k = 0; k < NG; k++) {
            grain_t *g = &vo->g[k];
            layer_t *ly = &lay[g->lay];
            int32_t pos, step, gl, gr, lenq;
            uint32_t wph, winc, lofi;
            const int16_t *s16;
            const int8_t *s8;
            if (g->on && (ly->len < 64u || g->gen != ly->gen)) g->on = 0;   /* its layer went or is loading */
            if (!g->on) { vis_pos[v * NG + k] = -1; continue; }
            lenq = (int32_t)ly->len << 12;
            lofi = ly->lofi;
            s16 = (const int16_t *)((const uint8_t *)smp_mem + ly->off);
            s8 = (const int8_t *)s16;
            pos = g->pos; step = g->step; wph = g->wph; winc = g->winc; gl = g->gl; gr = g->gr;
            for (i = g->delay; i < n; i++) {
                int32_t a, b, s, fr = pos & 4095, ix = pos >> 12;
                uint32_t prev = wph;
                if (lofi) { a = s8[ix] * 256; b = s8[ix + 1] * 256; }
                else { a = s16[ix]; b = s16[ix + 1]; }
                s = a + (((b - a) * fr) >> 12);
                s = (s * win[wph >> 22]) >> 15;
                vbl[i] += (s * gl) >> 15;
                vbr[i] += (s * gr) >> 15;
                pos += step;
                if (pos >= lenq) pos -= lenq;
                else if (pos < 0) pos += lenq;
                wph += winc;
                if (wph < prev) { g->on = 0; break; }
            }
            g->delay = 0;
            g->pos = pos;
            g->wph = wph;
            vis_pos[v * NG + k] = g->on ? ly->wstart + (pos >> 12) : -1;
            vis_lay[v * NG + k] = g->lay;
            vis_amp[v * NG + k] = g->on ? (uint8_t)(win[wph >> 22] >> 7) : 0;
        }

        if (eng.mot == MO_RISON) rs_block(vo, v, n);   /* 1.8: the grains pluck the voice's string */
        /* the envelope, per frame, and into the mix */
        for (i = 0; i < n; i++) {
            int32_t lv;
            if (vo->gate) { env += eng.atk; if (env > (32767 << 8)) env = 32767 << 8; }
            else { env -= eng.rel; if (env < 0) env = 0; }
            lv = (((env >> 8) * vel) >> 15) * gain >> 15;
            out[2u * i] += ((vbl[i] >> 3) * lv) >> 12;         /* >> 3 first: 8 grains at full scale x 1.5 fit 32 bits */
            out[2u * i + 1u] += ((vbr[i] >> 3) * lv) >> 12;
        }
        vo->env = env;
        if (!vo->gate && env == 0) {        /* released and silent: the voice is free */
            vo->active = 0;
            for (k = 0; k < NG; k++) { vo->g[k].on = 0; vis_pos[v * NG + k] = -1; }
        }
    }

    filter_block(out, n);
    mod_block(out, n);

    /* the crush on the grains, the effects at half rate (each pair of frames in, one out, drawn back up
     * between the last and this one), then a soft knee above -2.5 dB */
    for (i = 0; i < 2u * n; i += 4u) {
        int32_t yl, yr, l0, r0, l1, r1, dk;
        uint32_t j;
        for (j = i; j < i + 4u && j < 2u * n; j += 2u)
            if (eng.crush_div > 1u || eng.crush_mask != 0xFFFFFFFFu) {
                if (++crush_hold >= eng.crush_div) {
                    crush_hold = 0;
                    crush_l = out[j] & (int32_t)eng.crush_mask;
                    crush_r = out[j + 1u] & (int32_t)eng.crush_mask;
                }
                out[j] = crush_l; out[j + 1u] = crush_r;
            }
        l0 = out[i]; r0 = out[i + 1u];
        l1 = i + 2u < 2u * n ? out[i + 2u] : l0;
        r1 = i + 2u < 2u * n ? out[i + 3u] : r0;
        fx_frame(sat16((l0 + l1) >> 1), sat16((r0 + r1) >> 1), &yl, &yr, &dk);
        if (dk != 32768) {                  /* NASTRO RIPETI: the sound gives way to its repeat */
            l0 = (l0 * dk) >> 15; r0 = (r0 * dk) >> 15; l1 = (l1 * dk) >> 15; r1 = (r1 * dk) >> 15;
        }
        out[i] = l0 + ((fx_prevl + yl) >> 1);
        out[i + 1u] = r0 + ((fx_prevr + yr) >> 1);
        if (i + 2u < 2u * n) { out[i + 2u] = l1 + yl; out[i + 3u] = r1 + yr; }
        fx_prevl = yl; fx_prevr = yr;
    }
    comp_block(out, n);
    for (i = 0; i < 2u * n; i += 2u) {
        int32_t l = out[i], r = out[i + 1u];
        if (l > 24576) l = 24576 + ((l - 24576) >> 2); else if (l < -24576) l = -24576 + ((l + 24576) >> 2);
        if (r > 24576) r = 24576 + ((r - 24576) >> 2); else if (r < -24576) r = -24576 + ((r + 24576) >> 2);
        if (eng.bit12) { l &= ~15; r &= ~15; }   /* 1.6: USCITA 12 BIT, cut without dither (the old samplers' grain) */
        out[i] = l;
        out[i + 1u] = r;
    }
    audio_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
}

/* ---- the two LFOs (main loop, every 4 ms): shape, rate (free or in time with the sequencer), depth, destination.
 * LFO 2 runs first: it may move LFO 1's rate or depth. lfo_m: what they add to each destination, Q15 of its range */
static uint32_t lfo_ph[2], lfo_t, lfo_live;
static int32_t lfo_r0[2], lfo_r1[2], lfo_m[LD_N];
static uint32_t bpm10_now(void) { return clk.ext ? ext_bpm10 : ((uint32_t)par[P_TEMPO] + 20u) * 10u; }   /* tenths of BPM */
#define LR_SYNC 127u                        /* 1.5: VELOCITA' 0..126 free (half the old steps), 127..133 in time */
static uint32_t lfo_inc(uint32_t r)         /* phase per ms */
{
    if (r < LR_SYNC) return r & 1u ? (LFO_INC[r >> 1] >> 1) + (LFO_INC[(r >> 1) + 1u] >> 1) : LFO_INC[r >> 1];
    return 71583u * bpm10_now() * 4u / (10u * SYNC_Q4[(r - LR_SYNC) % 7u]);   /* 2^32 / 60000 per BPM, a cycle in sixteenths */
}
static int32_t lfo_depth(int32_t d)         /* 1.5: INTENSITA' -100..100 -> -10000..10000, fine near 0 (its square) */
{
    if (d > 100) d = 100;
    if (d < -100) d = -100;
    return d < 0 ? -d * d : d * d;
}
static int32_t lfo_shape(uint32_t i, uint32_t w)
{
    uint32_t ph = lfo_ph[i], x = ph >> 16;
    switch (w) {
    case 1: return x < 32768u ? (int32_t)(x * 2u) - 32767 : (int32_t)((65535u - x) * 2u) - 32767;   /* triangle */
    case 2: return (int32_t)x - 32768;                                                                 /* saw up */
    case 3: return 32767 - (int32_t)x;                                                                 /* saw down */
    case 4: return ph < 0x80000000u ? 32767 : -32767;                                                  /* square */
    case 5: return lfo_r1[i];                                                                          /* random steps */
    case 6: return lfo_r0[i] + (int32_t)(((int64_t)(lfo_r1[i] - lfo_r0[i]) * (int32_t)x) >> 16);       /* random, smooth */
    default: return lfo_sin(ph);                                                                       /* sine */
    }
}
static void lfo_run(uint32_t i, uint32_t rate, int32_t depth, uint32_t dt)   /* depth -10000..10000 */
{
    uint32_t prev = lfo_ph[i], k = i ? P_L2W : P_L1W, dst = par[k + 3u];
    lfo_ph[i] += lfo_inc(rate) * dt;
    if (lfo_ph[i] < prev) {                 /* a new cycle: the next random value */
        lfo_r0[i] = lfo_r1[i];
        lfo_r1[i] = (int32_t)(xrand_main() & 0xFFFFu) - 32768;
    }
    if (depth && dst < LD_N) lfo_m[dst] += lfo_shape(i, par[k]) * depth / 10000;
}
static int lfo_tick(void)                   /* 1: the destinations moved */
{
    uint32_t dt = fm1_ms - lfo_t, k;
    int32_t r1, d1;
    if (dt < 4u) return 0;
    lfo_t = fm1_ms;
    if (dt > 100u) dt = 100u;
    for (k = 0; k < LD_N; k++) lfo_m[k] = 0;
    lfo_run(1, par[P_L2R], lfo_depth((int32_t)par[P_L2D] - 100), dt);
    r1 = (int32_t)par[P_L1R] + ((lfo_m[LD_L1R] * (int32_t)PMAX[P_L1R]) >> 15);
    d1 = (int32_t)par[P_L1D] - 100 + ((lfo_m[LD_L1D] * 100) >> 15);
    if (r1 < 0) r1 = 0;
    if (r1 > (int32_t)PMAX[P_L1R]) r1 = (int32_t)PMAX[P_L1R];
    lfo_run(0, (uint32_t)r1, lfo_depth(d1), dt);
    for (k = 0; k < LD_N; k++) { if (lfo_m[k] > 32767) lfo_m[k] = 32767; if (lfo_m[k] < -32767) lfo_m[k] = -32767; }
    k = par[P_L1D] != 100u || par[P_L2D] != 100u;
    if (!k && !lfo_live) return 0;
    lfo_live = k;
    return 1;
}
static void lfo_restart(void) { lfo_ph[0] = lfo_ph[1] = 0; }   /* PLAY: the LFOs in time with the sequence */
static uint8_t epar[NP];                    /* the values with the LFOs on them, for the screen */

/* ---- from the parameters to the engine (main loop) */
static uint32_t isqrt(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* 1.5: AREA 0..200, its square: 1 % of the sound at 20, 25 % at 100, all of it at 200 (fine where the cloud is small) */
static int32_t area_of(uint32_t v, uint32_t share, uint32_t full)
{
    uint32_t fq = v * v * 16384u / 10000u * share / 100u;      /* Q16 of the sound: v^2 / 40000, x share / 100 (no 64-bit division) */
    return (int32_t)(((uint64_t)fq * full) >> 16);
}
static int32_t res_tab(const int32_t *t, uint32_t v)   /* 1.5: RISONANZA 0..200, between the table's 101 values */
{
    if (v >= 200u) return t[100];
    return v & 1u ? (t[v >> 1] + t[(v >> 1) + 1u]) / 2 : t[v >> 1];
}
static int32_t lfo_coarse(uint8_t *q, uint32_t p, uint32_t ld)   /* the LFOs on a parameter, in its steps */
{
    int32_t v = (int32_t)par[p] + ((lfo_m[ld] * (int32_t)PMAX[p]) >> 15);
    if (v < 0) v = 0;
    if (v > (int32_t)PMAX[p]) v = (int32_t)PMAX[p];
    q[p] = (uint8_t)v;
    return lfo_m[ld];
}
static void engine_update(void)
{
    uint32_t ms, d, ov, a, gf, L, cum = 0, k;
    int32_t sq, m12, posq;
    uint8_t q[NP];
    for (k = 0; k < NP; k++) q[k] = par[k];
    lfo_coarse(q, P_POS, LD_POS); lfo_coarse(q, P_AREA, LD_AREA); lfo_coarse(q, P_SCAN, LD_SCAN);
    lfo_coarse(q, P_SIZE, LD_SIZE); lfo_coarse(q, P_DENS, LD_DENS); lfo_coarse(q, P_SPRAY, LD_SPRAY);
    lfo_coarse(q, P_PITCH, LD_PITCH); lfo_coarse(q, P_FCUT, LD_FCUT); lfo_coarse(q, P_FRES, LD_FRES);
    lfo_coarse(q, P_VOL, LD_VOL); lfo_coarse(q, P_PAN, LD_PAN); lfo_coarse(q, P_DMIX, LD_DMIX);
    lfo_coarse(q, P_DTIME, LD_DTIME); lfo_coarse(q, P_RMIX, LD_RMIX); lfo_coarse(q, P_RDEC, LD_RDEC);
    lfo_coarse(q, P_SHIM, LD_SHIM); lfo_coarse(q, P_MRATE, LD_MRATE); lfo_coarse(q, P_MDEPTH, LD_MDEPTH);
    lfo_coarse(q, P_MMIX, LD_MMIX);
    m12 = lfo_m[LD_MIX12];
    posq = (int32_t)par[P_POS] * 256 + ((lfo_m[LD_POS] * 200) >> 7);   /* POSIZIONE finer: Q8 */
    if (posq < 0) posq = 0;
    if (posq > 200 * 256) posq = 200 * 256;
    for (L = 0; L < NL; L++) {              /* where each layer reads, in its own sound's frames */
        layer_t *ly = &lay[L];
        uint32_t full = ly->full < 64u ? 64u : ly->full;
        int32_t area = area_of(q[P_AREA], q[P_LA1 + L], full);   /* AREA STRATI: its share */
        int32_t pl = posq + ((int32_t)par[P_LS1 + L] - 100) * 256;   /* INIZIO STRATI: +-50 % of the sound, around */
        while (pl < 0) pl += 200 * 256;
        while (pl >= 200 * 256) pl -= 200 * 256;
        ly->ct = lp_cents(par[P_LP1 + L]) + ((lfo_m[LD_LP1 + L] * 2400) >> 15);
        {
            int32_t pn = ((int32_t)par[P_PN1 + L] - 100) * 328 + lfo_m[LD_PN1 + L];   /* x 328, not 32767 / 100: the folded
                                                                                     * constant would look like a ROM address to the build's check */
            ly->pan = pn > 32767 ? 32767 : pn < -32767 ? -32767 : pn;
        }
        ly->pos0 = (int32_t)(((uint64_t)(uint32_t)pl * full * 1311u) >> 26);   /* / 51200 */
        ly->area = area < 64 ? 64 : area;
        ly->spray = q[P_MOTORE] == MO_STIRA ? (int32_t)(q[P_M2] * q[P_M2] * 11025u / 10000u)   /* 1.8, SFOCATURA: up to 0.5 s */
                  : q[P_MOTORE] == MO_FORMANTI ? (int32_t)(q[P_M3] * 66u / 100u)   /* FORMANTI: each grain the same spot (RESPIRO: 3 ms) */
                  : (int32_t)(q[P_SPRAY] * full / 200u);
        {   /* VOLUMI (1.5): a level's square, the way a fader sounds; MUTE and SOLO silence it */
            uint32_t vo = q[P_VO1 + L];
            if ((lay_mute >> L & 1u) || (lay_solo && !(lay_solo >> L & 1u))) vo = 0;
            ly->vol = (int32_t)(vo * vo * 32767u / 10000u);
        }
        if (ly->len >= 64u) {
            uint32_t lv = q[P_LV1 + L];
            if ((lay_mute >> L & 1u) || (lay_solo && !(lay_solo >> L & 1u))) lv = 0;
            if (L == 0u && m12 > 0) lv = lv * (uint32_t)(32768 - m12) >> 15;   /* STRATI 1-2: one up, the other down */
            if (L == 1u && m12 < 0) lv = lv * (uint32_t)(32768 + m12) >> 15;
            cum += lv;
        }
        lay_cum[L] = cum;
    }
    {   /* SPLIT: the zones, one a layer with a sound (the first always has one), at the split notes */
        uint32_t n = 0;
        for (L = 0; L < NL; L++) if (L == 0u || par[P_SND + L]) zone_lay[n++] = (uint8_t)L;
        for (L = 0; L + 1u < NL; L++) zone_pt[L] = par[P_SP1 + L];
        zone_n = par[P_DISP] ? (uint8_t)n : 0u;
    }
    sq = (int32_t)q[P_SCAN] * 2048 / 100;          /* 0..200 % of the sound's speed, Q12 (1.5: a step 1 %) */
    if (q[P_MOTORE] == MO_STIRA) sq = (int32_t)(q[P_M1] * q[P_M1] * 2048u / 10000u);   /* 1.8, VELOCITA': 0 still .. the sound's own pace */
    if (q[P_MOTORE] == MO_FORMANTI) sq = 0;    /* FORMANTI: the grains alike, period after period (a vowel held): a moving read
                                                * would shift the pitch by its speed; POSIZIONE (and its LFO) choose the vowel */
    eng.dir = q[P_MOTORE] == MO_STIRA ? 0u : q[P_DIR] & 3u;
    eng.salto = (uint32_t)q[P_SALTO] * q[P_SALTO] * 429497u;   /* 1.5: its square, hundredths of the area: fine near 0 */
    {   /* the scan as a fraction of the area: of the first layer's frames */
        uint32_t aq = (uint32_t)(sq < 0 ? -sq : sq), ar = (uint32_t)lay[0].area;
        if (aq > 4095u) aq = 4095u;
        eng.scan_ph = (int32_t)((aq << 20) / (ar ? ar : 64u));
        if (lay[0].r11) eng.scan_ph >>= 1;               /* 1.6: a sound at 11025 Hz goes by at half the frames */
        if (eng.dir == 1u) eng.scan_ph = -eng.scan_ph;    /* INDIETRO */
        if (eng.dir == 2u) eng.scan_ph >>= 1;             /* ALTERNATO: there and back in the time of one way */
    }
    ms = SIZE_MS[q[P_SIZE] & 63u];
    if (q[P_MOTORE] == MO_STIRA) ms = ms < 80u ? 80u : ms > 800u ? 800u : ms;   /* 1.8: long grains, always overlapping */
    gf = ms * 441u / 10u;
    if (q[P_MOTORE] == MO_FORMANTI) {       /* 1.8, LARGHEZZA: 2 .. 30 ms, the formant's width */
        gf = (20u + q[P_M2] * q[P_M2] * 280u / 10000u) * 441u / 100u;
        ms = gf * 10u / 441u;
    }
    eng.winc = 0xFFFFFFFFu / gf;
    eng.gframes = gf;
    d = DENS_DHZ[q[P_DENS] & 63u];
    eng.period = q[P_MOTORE] == MO_STIRA ? (int32_t)(gf / 4u) : (int32_t)(441000u / d);   /* STIRAMENTO: four at once */
    eng.pitch_ct = ((int32_t)par[P_PITCH] - 24) * 100 + ((lfo_m[LD_PITCH] * 2400) >> 15);   /* finer: cents */
    eng.jit_ct = q[P_MOTORE] == MO_STIRA ? (int32_t)(q[P_M4] * 40u / 100u) : q[P_MOTORE] == MO_FORMANTI ? 0 : (int32_t)q[P_JIT] * 12;   /* DERIVA */
    eng.win = q[P_MOTORE] == MO_STIRA ? 0u : (q[P_WIN] & 3u) * (WIN_N + 1u);   /* STIRAMENTO: soft, always */
    eng.rev = q[P_MOTORE] == MO_STIRA ? 0u : q[P_REV] * 65536u / 100u;
    a = ENV_MS[q[P_ATK] & 63u] * 441u / 10u;
    eng.atk = (int32_t)((32767u << 8) / a);
    a = ENV_MS[q[P_REL] & 63u] * 441u / 10u;
    eng.rel = (int32_t)((32767u << 8) / a);
    eng.pan = (int32_t)(q[P_PAN] * 32767u / 100u);
    if (q[P_CRUSH]) {
        eng.crush_div = 1u + q[P_CRUSH] / 3u;
        eng.crush_mask = ~((1u << (q[P_CRUSH] * 10u / 15u)) - 1u);
    } else {
        eng.crush_div = 1u;
        eng.crush_mask = 0xFFFFFFFFu;
    }
    eng.retrig = q[P_RETRIG];
    scale_sel = q[P_SCALA]; scale_ton = q[P_TONICA] % 12u;
    eng.bit12 = q[P_BIT12];
    {   /* the cutoff: the low passes over all 9 octaves; BANDA, ALTO and NOTCH over 80 Hz .. 8 kHz, where they are heard */
        int32_t lo = 0, span = 1023;
        if (q[P_FMODE] >= 3u) { lo = FC_LO; span = FC_SPAN; }
        eng.f_lo = lo; eng.f_span = span;
        eng.f_cut = lo + (int32_t)par[P_FCUT] * span / 200 + ((lfo_m[LD_FCUT] * span) >> 15);   /* 1.5: 200 steps */
        if (eng.f_cut < lo) eng.f_cut = lo;
        if (eng.f_cut > lo + span) eng.f_cut = lo + span;
    }
    eng.f_k = res_tab(RES_K, q[P_FRES]);                              /* SVF: 2.0 .. 0.1 (Q 0.5 .. 10) */
    eng.f_lk = res_tab(LAD_K, q[P_FRES]);                             /* ladder: 0 .. 3.95 (Q16), 4 would sing by itself */
    eng.f_in = eng.f_k >= 45875 ? 65536 : 32768 + (eng.f_k - 6554) * 54613 / 65536;   /* Q16: 1 up to Q 1.4, then down to 0.5 at Q 10 */
    eng.f_lfo = (int32_t)q[P_FLFO] * 341 / 100;                      /* up to 3 octaves either way */
    {   /* 1.2: the band's gain (the square root of the damping: its peak grows as Q^0.5), the high pass's input (Q^-0.25),
         * the notch's damping (more resonance, a wider notch) and its second notch's distance (a quarter to a whole octave) */
        int32_t kq = eng.f_k, bg = (int32_t)isqrt((uint32_t)kq << 8) * 16;
        eng.f_bg = bg;
        eng.f_hin = kq >= 65536 ? 65536 : (int32_t)isqrt((uint32_t)bg << 16);
        eng.f_kn = res_tab(RES_K, 200u - q[P_FRES]);
        eng.f_n2 = 28 + (int32_t)q[P_FRES] * 86 / 200;
    }
    eng.f_mode = q[P_FMODE];
    eng.freeze = q[P_FREEZE];
    {   /* MODULAZIONE, the compressor, the shimmer */
        static const int32_t SH_NUM[5] = {65536, 32657, 130850, 196608, -32768};   /* (ratio - 1) x 65536 */
        uint32_t r = q[P_MRATE], b10 = bpm10_now();
        int32_t tv = par[P_MTYPE], dp = (int32_t)q[P_MDEPTH];
        if (tv >= (int32_t)MT_VEL && tv < (int32_t)MT_MET) {   /* an LFO on CARATTERE stays in VELVET, on LATO in METALLO */
            tv += (lfo_m[LD_MTYPE] * 100) >> 15;
            tv = tv < (int32_t)MT_VEL ? (int32_t)MT_VEL : tv > (int32_t)MT_MET - 1 ? (int32_t)MT_MET - 1 : tv;
        } else if (tv >= (int32_t)MT_MET) {
            tv += (lfo_m[LD_MTYPE] * 100) >> 15;
            tv = tv < (int32_t)MT_MET ? (int32_t)MT_MET : tv > (int32_t)MT_MET + 100 ? (int32_t)MT_MET + 100 : tv;
        }
        q[P_MTYPE] = (uint8_t)tv;
        eng.m_depth = dp * 32767 / 100;
        eng.m_mix = (int32_t)q[P_MMIX] * 32767 / 100;
        eng.m_fb2 = 6554 + dp * 18022 / 100;          /* the phaser's feedback: 0.2 .. 0.75 */
        eng.m_fb = 0;
        if (!tv) eng.m_type = 0;
        else if (tv < (int32_t)MT_MET) {    /* VELVET: CARATTERE 0..100 */
            int32_t c = tv - (int32_t)MT_VEL, t;
            uint32_t tk = r >= MR_SYNC ? MSYNC_T[(r - MR_SYNC) % 14u] : 0u;
            eng.m_inc = tk ? 15584u * b10 / tk : MRATE_INC[r];   /* free, or a turn in tk ticks of the clock */
            if (c <= 33) {                  /* wide chorus -> chorus: 15 -> 8 ms, +-6 -> +-3, the second voice fading */
                eng.v_d = (662 - 309 * c / 33) << 8; eng.v_s = (265 - 133 * c / 33) << 8;
                eng.v_t2 = 32767 * (33 - c) / 33; eng.v_fb = 0; eng.v_ph = 0;
            } else if (c <= 66) {           /* chorus -> flanger: 8 -> 4 ms, +-3 -> +-3.8, the feedback to 0.7 */
                t = c - 33;
                eng.v_d = (353 - 177 * t / 33) << 8; eng.v_s = (132 + 36 * t / 33) << 8;
                eng.v_t2 = 0; eng.v_fb = 22938 * t / 33; eng.v_ph = 0;
            } else {                        /* flanger -> phaser */
                eng.v_d = 176 << 8; eng.v_s = 168 << 8; eng.v_t2 = 0; eng.v_fb = 22938;
                eng.v_ph = 32767 * (c - 66) / 34;
            }
            eng.m_type = 1;
        } else {                            /* METALLO: LATO -100..+100 */
            metal_init();
            eng.m_k = (tv - (int32_t)MT_MET - 50) * 655;
            eng.m_inc = METAL_INC[r > 100u ? 100u : r];
            eng.m_fb = dp * 29490 / 100;
            eng.m_type = 2;
        }
        {   /* METALLO's feedback grows the level (x 3.8 at the top): 1 / (1 + 2 fb^2) */
            int32_t f = eng.m_fb, f2 = (int32_t)(((int64_t)f * f) >> 15);
            eng.m_fsc = (int32_t)(((int64_t)32768 << 15) / (32768 + 2 * f2));
        }
        eng.comp = (int32_t)q[P_COMP];
        eng.sh_win = 512 + (int32_t)q[P_SHDIFF] * 15;
        eng.sh_step = SH_NUM[q[P_SHINT] % 5u] * 256 / eng.sh_win;   /* Q24 of the window a sample */
        eng.sh_tone = 3277 + (int32_t)q[P_SHTONE] * 262;
        eng.sh_amt = (int32_t)q[P_SHIM] * 9830 / 100;   /* up to 0.3: with a long tail it still climbs, but never runs away */
    }
    clk.inc = clk.ext ? ext_bpm10 * 15584u : ((uint32_t)q[P_TEMPO] + 20u) * 155835u;   /* BPM x 96 ticks x 2^32 / (60 x 44100) */
    clk.sdiv = q[P_SDIV];
    clk.slen = (uint32_t)q[P_SLEN] + 1u;
    clk.swing = q[P_SWING];
    clk.adiv = q[P_ADIV];
    clk.amode = q[P_AMODE];
    clk.aoct = q[P_AOCT];
    clk.agate = q[P_AGATE] < 5u ? 5u : q[P_AGATE];
    clk.ogni = OGNI_N[q[P_OGNI] & 7u];
    eng.d_mix = (int32_t)(q[P_DMIX] * 32767u / 100u);
    {   /* NASTRO's TEMPO: free (between the table's steps, for the LFO: the line glides to it, a tape's wow) or on the clock */
        int32_t nl8, md = q[P_NMODE], lim;
        if (par[P_DTIME] >= NT_SYNC) {
            uint32_t tk = MSYNC_T[(par[P_DTIME] - NT_SYNC) % 14u];
            nl8 = (int32_t)(((uint64_t)tk * 275625u / (2u * bpm10_now())) << 8);
        } else {
            int32_t i8 = (int32_t)par[P_DTIME] * 256 + ((lfo_m[LD_DTIME] * 63) >> 7), i, f, ms0;
            if (i8 < 0) i8 = 0;
            if (i8 > 63 * 256) i8 = 63 * 256;
            i = i8 >> 8; f = i8 & 255;
            ms0 = (int32_t)DLY_MS[i] * 256 + (i < 63 ? ((int32_t)DLY_MS[i + 1] - (int32_t)DLY_MS[i]) * f : 0);
            nl8 = (int32_t)((uint32_t)ms0 * 2205u / 100u);            /* ms Q8 -> samples at 22050, Q8 */
        }
        lim = md == 1 || md == 2 ? (int32_t)DLY_N / 2 - 4 : md == 4 ? (int32_t)DLY_N - 1100 : (int32_t)DLY_N - 2;
        while (nl8 > (lim << 8)) nl8 >>= 1;   /* longer than the line holds: half, or a quarter */
        if (nl8 < (64 << 8)) nl8 = 64 << 8;
        eng.d_time = nl8;
        eng.n_len = nl8 >> 8;
        eng.n_mode = md;
        eng.n_var = (int32_t)q[P_NVAR] * 65535 / 100;
        eng.n_caso = q[P_NCASO];
        eng.n_reps = 1 + (int32_t)q[P_DFB] * 15 / 95;                     /* RIPETI: 1 .. 16 repeats */
        eng.n_short = q[P_NPASSO] < 40u;
        eng.n_pdrop = q[P_NPASSO] > 60u ? (int32_t)cents_step(-(int32_t)(q[P_NPASSO] - 60u) * 30) * 32 : 65536;   /* to -12 st a repeat */
        {   /* NASTRO and CONGELA: the repeats' pitch, in semitones */
            int32_t st = ((int32_t)q[P_NPASSO] - 50) / 4;
            eng.n_ratio = (md == 3 || md == 4) && st ? (int32_t)cents_step(st * 100) * 32 : 65536;
        }
    }
    eng.d_fb = (int32_t)(q[P_DFB] * 32767u / 100u);
    eng.d_tone = (int32_t)(3000u + q[P_DTONE] * 297u);                /* the loop's low-pass: dark .. open */
    eng.r_mix = (int32_t)(q[P_RMIX] * 360u);                          /* 100 -> x 1.1 */
    eng.r_dec = RDEC_Q15[q[P_RDEC] > 100u ? 100u : q[P_RDEC]];
    eng.r_tone = (int32_t)(2500u + q[P_RTONE] * 302u);
    eng.r_mod = (int32_t)(q[P_RMOD] * 28u * 256u / 100u) >> 1;        /* +- up to 14 samples */
    {   /* 1.8: the engine's own knobs */
        uint32_t m1 = q[P_M1], m2 = q[P_M2], m3 = q[P_M3], m4 = q[P_M4], loss;
        eng.st_oct = m3 * 65536u / 100u;
        eng.fo_ct = ((int32_t)m1 - 50) * 24;            /* FORMANTE: +-12 semitones */
        eng.fo_breath = (int32_t)(m3 * 65536u / 100u);
        eng.fo_vib = (int32_t)(m4 * 50u / 100u);        /* VIBRATO: up to 50 cents */
        eng.fo_vinc = 535670;                            /* 5.5 Hz */
        loss = 16u + (100u - m1) * (100u - m1) * 3277u / 10000u;
        eng.rs_fb = (int32_t)(32768u - loss);           /* DECADIMENTO: 0.9 .. 0.9995 a pass */
        eng.rs_damp = (int32_t)(4000u + m2 * 287u);     /* TIMBRO: the loop's low-pass */
        eng.rs_ap = (int32_t)(m3 * m3 * 24576u / 10000u);   /* DISPERSIONE: the loop's all-pass, 0 .. 0.75 */
        eng.rs_mix = (int32_t)(m4 * 32767u / 100u);
        {   /* the pluck: as loud as a grain when the string dies before the next one; less as they pile up
             * (the string's power ~ what comes in / what it loses a second): sqrt(loss a second / grain-seconds a second) */
            uint32_t R = loss * 262u * 305u / ((d ? d : 1u) * (ms ? ms : 1u));   /* x 1000 (1e7 / 32768 = 305); no 64-bit division here */
            if (R > 1000u) R = 1000u;
            eng.rs_gin = (int32_t)isqrt(R * 1073741u);
            if (eng.rs_gin > 32767) eng.rs_gin = 32767;
            if (eng.rs_gin < 256) eng.rs_gin = 256;
        }
        eng.mot = q[P_MOTORE] < NMOT ? q[P_MOTORE] : 0u;
        if (eng.mot == MO_STIRA) d = 4u * 441000u / (gf ? gf : 1u);
        if (eng.mot == MO_FORMANTI) d = 2u * 441000u / (gf ? gf : 1u);   /* about two at once: spawn() evens them out */
    }
    ov = d * ms / 100u;                     /* grains overlapping, x100 */
    if (ov > NG * 100u) ov = NG * 100u;
    if (ov < 100u) ov = 100u;
    eng.gain = (int32_t)(q[P_VOL] * 327u * 10u / isqrt(ov)) * 3 / 2;
    for (k = 0; k < NP; k++) epar[k] = q[k];
}

/* ---- the sample bank: after the code in the flash (tools/make_samples.py bank_build, or the editor), read
 * in place. Each sound is decoded into RAM when chosen: 16 bit (HIFI) or 8 bit (LOFI). */
#define BANK_MAX 48u                         /* 1.6: the factory bank's and the user bank's together */
typedef struct { char name[13]; uint32_t frames; const uint8_t *adpcm; uint8_t lofi, r11; } snd_t;   /* r11: 11025 Hz (1.6) */
static snd_t snd[BANK_MAX];
static uint32_t nsnd, nfact;                /* all the sounds; those of the factory bank (the user bank's follow) */
static uint32_t ub_busy;                    /* 1.6: the editor is writing the user bank: only the factory one plays */
static uint32_t ub_diag = 2, ub_total, ub_got;   /* 1.6.1: 0 no bank in the flash, 1 there but not read, 2 read; the blocks */
static const uint8_t *bank;
#if defined(GRANA_WEB)
static uint8_t web_bank[600000];            /* the page writes the bank here before init */
#elif defined(DINGHY_HOST)
#include "grana_bank.h"
#else
extern const uint8_t _img_end[];            /* app.ld: the end of the code; the bank starts at the next 4 KiB */
#endif
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t bank_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}
static void bank_add(const uint8_t *b, uint32_t room)   /* a bank's sounds after those already there (a bad one: none) */
{
    uint32_t i, cnt, total;
    if (b[0] != 'G' || b[1] != 'R' || b[2] != 'N' || b[3] != 'B') return;
    cnt = b[6] | (uint32_t)b[7] << 8;
    total = rd32(b + 8);
    if (cnt > BANK_MAX - nsnd || total < 16u + 24u * cnt || total > room) return;
    if (bank_crc(b + 16, total - 16u) != rd32(b + 12)) return;
    for (i = 0; i < cnt; i++) {
        const uint8_t *e = b + 16u + 24u * i;
        snd_t *d = &snd[nsnd];
        uint32_t k, off = rd32(e + 12), fr = rd32(e + 16);
        if (off + (fr + 1u) / 2u > total) break;
        for (k = 0; k < 12u; k++) d->name[k] = (char)(e[k] >= 32u && e[k] < 127u ? e[k] : 0);
        d->name[12] = 0;
        d->frames = fr;
        d->adpcm = b + off;
        d->lofi = e[20] ? 1u : 0u;
        d->r11 = e[21] == 1u ? 1u : 0u;      /* 1.6: 11025 Hz (0, as the older banks have it: 22050) */
        nsnd++;
    }
}
static void bank_open(void)
{
#if defined(GRANA_WEB)
    bank = web_bank;
#elif defined(DINGHY_HOST)
    bank = GRANA_BANK_DEFAULT;
#else
    bank = (const uint8_t *)(((uint32_t)_img_end + 4095u) & ~4095u);
#endif
    nsnd = 0;
    bank_add(bank, 0x90000u);
    nfact = nsnd;
#if defined(GRANA_WEB)
    if (!ub_busy) { web_ub_init(); bank_add(web_ubank, UB_LEN); }
#elif !defined(DINGHY_HOST)
    {   /* 1.6: the user bank, read in place. 1.6.1: the XIP window above 0x93000 set as not ciphered first (1.6 read it
         * through the deciphering, so it never found the bank); ub_diag tells what the flash itself holds */
        static uint32_t plain;
        uint8_t h[4];
        if (!plain) { plain = 1; fl_plain_window_init(); }
        ub_diag = 0;
        if (flash_ok && !st_read(UB_AT, h, 4) && h[0] == 'G' && h[1] == 'R' && h[2] == 'N' && h[3] == 'B') ub_diag = 1;
        if (!ub_busy) bank_add(fm1_xip_ptr(UB_AT), UB_LEN);
        if (ub_diag && nsnd > nfact) ub_diag = 2;
    }
#endif
}

static uint8_t peaks[NL][240];              /* each layer's whole sound, for the screen (1.2: a lane each) */
static uint8_t *peak;                       /* the one adpcm_run fills */
static const char *snd_name(uint32_t s) { return s < nsnd ? snd[s].name : "---"; }
static uint32_t nsounds(void) { return nsnd ? nsnd : 1u; }
static uint32_t peak_snd[NL] = {NOSND, NOSND, NOSND, NOSND};

/* IMA ADPCM from the bank: frames [from, from + n) into dst (16 or 8 bit); every frame before is decoded too,
 * for the predictor. With dst 0 it only fills peak[] for the whole sound. */
static void adpcm_run(uint32_t s, uint32_t from, uint32_t n, uint8_t *dst, uint32_t lofi)
{
    uint32_t i, idx = 0, end = dst ? from + n : snd[s].frames, full = snd[s].frames, x = 0, nx, m = 0;
    int32_t pred = 0;
    const uint8_t *d = snd[s].adpcm;
    if (!dst) for (i = 0; i < 240u; i++) peak[i] = 0;
    nx = full / 240u;
    for (i = 0; i < end; i++) {
        uint32_t code = (d[i >> 1] >> ((i & 1u) * 4u)) & 15u;
        int32_t st = ADPCM_STEP[idx], diff = st >> 3;
        if (code & 4u) diff += st;
        if (code & 2u) diff += st >> 1;
        if (code & 1u) diff += st >> 2;
        pred = (code & 8u) ? pred - diff : pred + diff;
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        idx = (uint32_t)((int32_t)idx + ADPCM_IDX[code & 7u]);
        if ((int32_t)idx < 0) idx = 0;
        if (idx > 88u) idx = 88u;
        if (dst) {
            if (i < from) continue;
            if (lofi) ((int8_t *)dst)[i - from] = (int8_t)(pred >> 8);   /* 8 bit, truncated: the grit */
            else ((int16_t *)dst)[i - from] = (int16_t)pred;
        } else {                            /* the outline: the loudest of each column */
            uint32_t q = (uint32_t)(lofi ? ((pred >> 8) * 256 < 0 ? -((pred >> 8) * 256) : (pred >> 8) * 256)
                                         : (pred < 0 ? -pred : pred));
            if (q > m) m = q;
            if (i + 1u >= (x + 1u) * full / 240u || i + 1u == end) {
                if (x < 240u) peak[x] = (uint8_t)(m >> 7);
                x++; m = 0;
            }
        }
    }
    (void)nx;
}

static uint32_t lay_want(uint32_t L)        /* the sound layer L should hold, or NOSND */
{
    uint32_t v = L == 0u ? par[P_SND] : par[P_SND2 + L - 1u];
    if (L) { if (!v) return NOSND; v -= 1u; }
    return v < nsnd ? v : NOSND;
}
static int32_t lay_wstart(uint32_t L, uint32_t s, uint32_t cap)   /* a window of cap frames from s, around where layer L reads */
{
    uint32_t full = snd[s].frames, pp = (par[P_POS] + 200u + par[P_LS1 + L] - 100u) % 200u;
    int32_t pos = (int32_t)(pp * full / 200u), sp = (int32_t)(par[P_SPRAY] * full / 200u);
    int32_t area = area_of(par[P_AREA], par[P_LA1 + L], full), reach = area + 2 * sp + (int32_t)eng.gframes, st;
    st = reach < (int32_t)cap ? pos - sp - ((int32_t)cap - reach) / 2 : pos - sp;   /* centred if it all fits */
    if (st > (int32_t)(full - cap)) st = (int32_t)(full - cap);
    if (st < 0) st = 0;
    return st;
}
/* share the RAM between the layers that sound: the short ones whole, the rest evenly; reload what moved.
 * force: reload all. A layer reloading is silent (its grains stop) for the time it takes to decode. */
static uint32_t lay_tload;
static void layers_sync(int force)
{
    uint32_t L, want[NL], need[NL], share[NL], cap, ln, off = 0, left = SMP_BYTES, rem = 0, done = 0, ch = 1, eq, dirty = 0;
    int32_t ws[NL];
    for (L = 0; L < NL; L++) {
        want[L] = lay_want(L);
        need[L] = want[L] == NOSND ? 0u : ((snd[want[L]].frames + 1u) * (snd[want[L]].lofi ? 1u : 2u) + 1u) & ~1u;
        share[L] = 0;
        if (need[L]) rem++;
    }
    while (ch && rem) {                     /* whoever needs less than an even share gets it all */
        ch = 0;
        for (L = 0; L < NL; L++)
            if (need[L] && !(done >> L & 1u) && need[L] <= left / rem) {
                share[L] = need[L]; left -= need[L]; rem--; done |= 1u << L; ch = 1;
            }
    }
    eq = rem ? (left / rem) & ~1u : 0u;
    for (L = 0; L < NL; L++) if (need[L] && !(done >> L & 1u)) share[L] = eq;
    for (L = 0; L < NL; L++) {              /* what changes: silenced first, all together */
        layer_t *ly = &lay[L];
        if (want[L] == NOSND) { ws[L] = 0; if (ly->snd != NOSND || ly->len) { ly->len = 0; ly->gen++; ly->snd = NOSND; ly->full = 0; } continue; }
        cap = share[L] / (snd[want[L]].lofi ? 1u : 2u) - 1u;   /* a frame kept for the interpolation's guard */
        ln = snd[want[L]].frames < cap ? snd[want[L]].frames : cap;
        ws[L] = ln < snd[want[L]].frames ? lay_wstart(L, want[L], ln) : 0;
        if (force || ly->snd != want[L] || ly->off != off || ly->len != ln || ly->wstart != ws[L]) {
            ly->len = 0; ly->gen++; ly->snd = NOSND;
            dirty |= 1u << L;
        }
        off += share[L];
    }
    if (dirty) fm1_delay_ms(7);             /* the ISR is past its block: the RAM is ours */
    for (off = 0, L = 0; L < NL; L++) {
        layer_t *ly = &lay[L];
        if (dirty >> L & 1u) {
            uint32_t s = want[L], lofi = snd[s].lofi, n;
            uint8_t *dst = (uint8_t *)smp_mem + off;
            cap = share[L] / (lofi ? 1u : 2u) - 1u;
            n = snd[s].frames < cap ? snd[s].frames : cap;
            adpcm_run(s, (uint32_t)ws[L], n, dst, lofi);
            if (lofi) ((int8_t *)dst)[n] = ((int8_t *)dst)[0];   /* the guard: the window loops */
            else ((int16_t *)dst)[n] = ((int16_t *)dst)[0];
            ly->off = off; ly->lofi = lofi; ly->full = snd[s].frames; ly->wstart = ws[L]; ly->snd = s; ly->r11 = snd[s].r11;
            ly->len = n;                    /* last: the ISR may read it now */
        }
        off += share[L];
    }
    for (L = 0; L < NL; L++)                /* each layer's outline, of its whole sound */
        if (want[L] != peak_snd[L]) {
            uint32_t x;
            peak_snd[L] = want[L];
            peak = peaks[L];
            if (want[L] == NOSND) { for (x = 0; x < 240u; x++) peak[x] = 0; }
            else adpcm_run(want[L], 0, 0, 0, snd[want[L]].lofi);
        }
    if (dirty) { ghead = 0; lay_tload = fm1_ms; }
    engine_update();
}
static uint32_t layers_active(void) { uint32_t L, c = 0; for (L = 1; L < NL; L++) if (lay_want(L) != NOSND) c++; return c; }
static uint32_t layers_partial(void) { uint32_t L; for (L = 0; L < NL; L++) if (lay[L].len && lay[L].len < lay[L].full) return 1; return 0; }

/* ---- notes (main loop) */
static uint32_t ages;
static int32_t octave;
static uint32_t hold, last_note = 60;
static uint32_t drone_on, drone_cur = 255;
static uint32_t reserved(void)              /* the voices the keys must leave alone */
{
    return (drone_on ? 1u << V_DRONE : 0u) | (clk.arp ? 1u << V_ARP : 0u) | (clk.run ? 1u << V_SEQ : 0u);
}
static void play(uint32_t note, uint32_t vel)
{
    uint32_t v, pick = NV, best = 0xFFFFFFFFu, res = reserved();
    if (note > 127u || !vel) return;
    last_note = note;
    for (v = 0; v < NV; v++) {              /* the same note, else a free voice, else the oldest */
        uint32_t score;
        if (res >> v & 1u) continue;
        score = voice[v].note == note && voice[v].gate ? 0u
              : voice[v].gate ? 0x80000000u + voice[v].age : (uint32_t)(voice[v].env >> 8) + 1u;
        if (score < best) { best = score; pick = v; }
    }
    if (pick >= NV) return;                 /* every voice taken by the drone, the arpeggio, the sequence */
    if (voice[pick].gate && voice[pick].note != note) {   /* stealing a held voice: let it restart */
        voice[pick].gate = 0;
        voice[pick].active = 0;
    }
    voice[pick].note = (uint8_t)note;
    voice[pick].age = ++ages;
    voice[pick].base_ct = ((int32_t)snap(note) - 60) * 100;
    voice[pick].pos_lock = -1;
    voice[pick].zone = (int8_t)zone_of(note);
    voice[pick].vel = (int32_t)(vel * 258u);  /* 127 -> 32766 */
    voice[pick].gate = 1;
}
static void stop(uint32_t note)
{
    uint32_t v, res = reserved();
    for (v = 0; v < NV; v++)
        if (!(res >> v & 1u) && voice[v].note == note && voice[v].gate) voice[v].gate = 0;
}
static void stop_all(void)
{
    uint32_t v, res = reserved();
    for (v = 0; v < NV; v++) if (!(res >> v & 1u)) voice[v].gate = 0;
}
static int quiet(void)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].gate || voice[v].active) return 0;
    return 1;
}

/* the drone: voice 0, on the note of its knob (or the last one played) */
static void drone_update(void)
{
    uint32_t d = par[P_DRONE], note;
    if (!d) {
        if (drone_on) { voice[V_DRONE].gate = 0; drone_on = 0; drone_cur = 255; }
        return;
    }
    note = d == 1u ? last_note : 36u + d - 2u;
    if (drone_on && note == drone_cur && voice[V_DRONE].gate) return;
    if (!drone_on) { voice[V_DRONE].gate = 0; voice[V_DRONE].active = 0; }   /* take the voice over */
    drone_on = 1;
    drone_cur = note;
    voice[V_DRONE].note = (uint8_t)note;
    voice[V_DRONE].base_ct = ((int32_t)snap(note) - 60) * 100;
    voice[V_DRONE].pos_lock = -1;
    voice[V_DRONE].zone = (int8_t)zone_of(note);
    voice[V_DRONE].vel = 25800;
    if (voice[V_DRONE].gate) voice[V_DRONE].restart = 1;
    voice[V_DRONE].gate = 1;
}

/* the arpeggiator's notes: held keys and MIDI notes; with HOLD the chord stays until a new one starts */
static uint8_t arp_list[16];
static uint32_t arp_cnt, arp_fingers;
static void arp_publish(void)
{
    uint32_t i, j, n = arp_cnt;
    uint8_t up[16];
    for (i = 0; i < n; i++) up[i] = arp_list[i];
    for (i = 1; i < n; i++)                  /* sorted, for up / down */
        for (j = i; j > 0 && up[j - 1] > up[j]; j--) { uint8_t t = up[j]; up[j] = up[j - 1]; up[j - 1] = t; }
    arp_n = 0;
    for (i = 0; i < n; i++) { arp_ord[i] = arp_list[i]; arp_up[i] = up[i]; }
    arp_n = n;
    arp_gen++;
}
static void arp_add(uint32_t note)
{
    uint32_t i;
    if (hold && par[P_TASTO]) {             /* TASTO SPEGNE: a key already in the held chord takes it out, a new one joins */
        arp_fingers++;
        last_note = note;
        for (i = 0; i < arp_cnt; i++)
            if (arp_list[i] == note) {
                for (; i + 1u < arp_cnt; i++) arp_list[i] = arp_list[i + 1u];
                arp_cnt--;
                arp_publish();
                return;
            }
        if (arp_cnt < 16u) arp_list[arp_cnt++] = (uint8_t)note;
        arp_publish();
        return;
    }
    if (hold && arp_fingers == 0u) arp_cnt = 0;   /* a new chord replaces the held one */
    arp_fingers++;
    last_note = note;
    for (i = 0; i < arp_cnt; i++) if (arp_list[i] == note) { arp_publish(); return; }
    if (arp_cnt < 16u) arp_list[arp_cnt++] = (uint8_t)note;
    arp_publish();
}
static void arp_remove(uint32_t note)
{
    uint32_t i;
    if (arp_fingers) arp_fingers--;
    if (hold) return;
    for (i = 0; i < arp_cnt; i++)
        if (arp_list[i] == note) {
            for (; i + 1u < arp_cnt; i++) arp_list[i] = arp_list[i + 1u];
            arp_cnt--;
            break;
        }
    arp_publish();
}
static int held_note(uint32_t note)         /* a note sounding on a voice of the keys */
{
    uint32_t v, res = reserved();
    for (v = 0; v < NV; v++) if (!(res >> v & 1u) && voice[v].note == note && voice[v].gate) return 1;
    return 0;
}
static void note_on(uint32_t note, uint32_t vel)
{
    if (clk.arp) arp_add(note);
    else if (hold && par[P_TASTO] && held_note(note)) stop(note);   /* TASTO SPEGNE: pressed again, it lets go */
    else play(note, vel);
}
static void note_off(uint32_t note) { if (clk.arp) arp_remove(note); else if (!hold) stop(note); }

/* ---- the presets: 64 records of 256 B in the platform's user area (user_read / user_erase / user_write,
 * 4 sectors of 16), each with its sequence and a checksum; the settings sector keeps the one in use.
 * A record that does not check out is an empty place, except the first 8: those are the factory presets. */
#define SET_VER0 0x4Bu                       /* 0.8: the settings sector held only the current preset */
#define SET_VER1 0x4Cu                       /* 1.2: and the MIDI page: the clock, MIDI LEARN */
#define SET_VER2 0x4Du                       /* 1.4: and the BLUETOOTH page (kept by both packages) */
#define SET_VER 0x4Eu                        /* 1.4.1: and the remembered controller's bond (a pairing's keys) */
#define NLEARN 32u
#define PREC_MAGIC 0xA7u
#define PREC_VER 9u                          /* 1.8: 104 parameters (MOTORE and its four) in 104; 8 (1.6..1.7: 100), 7, 6, 5, 4 and 3 converted */
#define PREC_NP 104u
#define PREC_SIZE 256u
typedef struct {
    uint8_t magic, ver, np, used;
    char name[12];                          /* set by the editor; empty: the sound's name */
    uint8_t p[PREC_NP];
    uint8_t sn[NSTEP], sp[NSTEP];
    uint32_t sum;
} prec_t;
typedef char prec_fits[(sizeof(prec_t) <= PREC_SIZE && NP <= PREC_NP) ? 1 : -1];
static prec_t pr[NPRESET];
typedef struct { uint8_t st, num, par, pad; } learn_t;   /* a message learnt: its status (kind and channel), number, parameter */
typedef struct {
    uint8_t ver, cur, clk_in, clk_out, nlearn, pad[3];
    learn_t lr[NLEARN];
} set12_t;
static struct {
    uint8_t ver, cur, clk_in, clk_out, nlearn, rx, pad[2];   /* rx (1.5): RICEVI, 0 TUTTO, 1 SOLO CLOCK, 2..17 CANALE 1..16 */
    learn_t lr[NLEARN];
    uint8_t bt_auto, bt_mode, bt_dev_ok, bt_dev_rand;   /* 1.4: AVVIO, MODO (AUTO MIDI SMC), the remembered controller */
    uint8_t bt_dev_addr[6], bt_pad[2];
    char bt_dev_name[20];
    uint8_t bt_kept[112];                   /* the random address made once [0..6], the radio's trims copy [8..107] */
    uint8_t bt_bonded, bt_ediv[2], bt_pad2;  /* 1.4.1: the controller's bond, when it asked for a pairing */
    uint8_t bt_ltk[16], bt_rand[8];
} store;
#define SET14_SIZE (sizeof store - 28u)      /* 1.4's record: without the bond */
static uint32_t set_dirty, t_set;            /* the MIDI page changed: kept a little later, when nothing plays */
static uint32_t save_pending, save_sectors, t_cur, t_pend;
static int32_t pend_preset = -1;              /* PRESETS turned: the preset it points at, loaded when it rests */

static void for_zero(void) { uint32_t k; for (k = 0; k < sizeof store; k++) ((uint8_t *)&store)[k] = 0; }
static uint32_t prec_sum(const prec_t *r)
{
    const uint8_t *b = (const uint8_t *)r;
    uint32_t i, h = 2166136261u;
    for (i = 0; i < (uint32_t)((const uint8_t *)&r->sum - b); i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
static void seq_default(uint32_t i, uint8_t *sn, uint8_t *sp)   /* a 16-step start for each preset */
{
    static const uint16_t PAT[NFACT] = {0x1111u, 0x5555u, 0xFFFFu, 0x9249u, 0x0101u, 0xFFFFu, 0x6DB6u, 0xB5ADu};
    static const int8_t SCALE[8] = {0, 3, 5, 7, 10, 12, 15, 19};
    uint32_t k, r = 0x2545F491u * (i + 1u);
    for (k = 0; k < NSTEP; k++) { sn[k] = 0; sp[k] = 255; }
    for (k = 0; k < 16u; k++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        if (!(PAT[i] >> k & 1u)) continue;
        if (i == 5u) { sn[k] = 60; sp[k] = (uint8_t)(k * 25u / 3u); continue; }   /* RITMO: the loop sliced, a step a sixteenth */
        sn[k] = (uint8_t)(48 + SCALE[r % 8u]);
        sp[k] = (uint8_t)(k * 200u / 16u);  /* each step from its own place in the sound */
    }
}
/* from 1.3 to 1.4: DELAY is NASTRO (its knobs moved: MODO, TEMPO, RITORNO, MIX; TONO on NASTRO 2), MODULAZIONE's seven types
 * are VELVET or METALLO (STUTTER goes to NASTRO's RIPETI), the LFOs' new destinations before VEL. and INT. LFO 1, the
 * layers in the centre */
static uint32_t isqrt_r(uint32_t x) { uint32_t r = isqrt(x); return x - r * r > r ? r + 1u : r; }   /* rounded */
/* from 1.4 to 1.5: the finer scales (SCAN a step 1 %; AREA, SALTO and the LFOs' INTENSITA' squared; FREQUENZA and RISONANZA 200
 * steps; the LFOs' VELOCITA' in half steps), the layers at full volume, no split */
static void prec_new16(prec_t *r);
static void prec_new15(prec_t *r)
{
    uint8_t *p = r->p;
    uint32_t k;
    p[P_SCAN] = (uint8_t)(p[P_SCAN] * 5u > 200u ? 200u : p[P_SCAN] * 5u);
    p[P_AREA] = (uint8_t)isqrt_r(p[P_AREA] * 400u);
    p[P_SALTO] = (uint8_t)isqrt_r(p[P_SALTO] * 100u);
    p[P_FCUT] = (uint8_t)((p[P_FCUT] * 200u + 63u) / 127u);
    p[P_FRES] = (uint8_t)(p[P_FRES] * 2u);
    for (k = 0; k < 2u; k++) {
        uint32_t rt = p[P_L1R + 4u * k], d = p[P_L1D + 4u * k];
        p[P_L1R + 4u * k] = (uint8_t)(rt < 64u ? rt * 2u : rt + 63u);
        p[P_L1D + 4u * k] = (uint8_t)(d >= 100u ? 100u + isqrt_r((d - 100u) * 100u) : 100u - isqrt_r((100u - d) * 100u));
    }
    p[P_NCASO] = 0;
    for (k = 0; k < NL; k++) p[P_VO1 + k] = 100;
    p[P_DISP] = 0; p[P_SP1] = 60; p[P_SP2] = 66; p[P_SP3] = 72;
    prec_new16(r);                           /* and on to 1.6 */
}
static void prec_new16(prec_t *r)           /* from 1.5 to 1.6: chromatic, the 12-bit output on */
{
    r->p[P_SCALA] = 0; r->p[P_TONICA] = 0; r->p[P_BIT12] = 1; r->p[P_MOTORE] = 0; r->p[P_M1] = r->p[P_M2] = r->p[P_M3] = r->p[P_M4] = 0;
}
static const uint8_t MOT_DEF[4][4] = {{0, 0, 0, 0},           /* CLASSICO: none */
    {20, 30, 15, 20},                       /* STIRAMENTO: VELOCITA', SFOCATURA, ARMONICI, DERIVA */
    {50, 40, 10, 25},                       /* FORMANTI: FORMANTE, LARGHEZZA, RESPIRO, VIBRATO */
    {75, 55, 0, 80}};                       /* RISONATORE: DECADIMENTO, TIMBRO, DISPERSIONE, MIX */
static int prec_up18(prec_t *r)             /* 1.8: a record of 1.6 .. 1.7 (100 parameters, the same layout) to this one */
{
    uint32_t k;
    if (r->magic != PREC_MAGIC || r->ver != 8u || r->np != 100u || r->sum != prec_sum(r)) return 0;
    r->p[P_MOTORE] = 0;
    for (k = 0; k < 4u; k++) r->p[P_M1 + k] = 0;
    r->ver = PREC_VER; r->np = NP; r->sum = prec_sum(r);
    return 1;
}
static int prec_from_14(prec_t *r)         /* a record of 1.4 or 1.5 (88 or 96 parameters in 96, its sum at 240): to this one */
{
    static uint8_t o[PREC_SIZE];
    uint32_t k, h = 2166136261u, sum, np;
    if (user_read((uint32_t)(r - pr) * PREC_SIZE, o, PREC_SIZE) || o[0] != PREC_MAGIC ||
        !((o[1] == 6u && o[2] == 88u) || (o[1] == 7u && o[2] == 96u))) return 0;
    for (k = 0; k < 240u; k++) h = (h ^ o[k]) * 16777619u;
    sum = o[240] | (uint32_t)o[241] << 8 | (uint32_t)o[242] << 16 | (uint32_t)o[243] << 24;
    if (sum != h) return 0;
    np = o[2];
    for (k = 0; k < sizeof *r; k++) ((uint8_t *)r)[k] = 0;
    for (k = 0; k < 12u; k++) r->name[k] = (char)o[4 + k];
    for (k = 0; k < np; k++) r->p[k] = o[16 + k];
    for (k = 0; k < NSTEP; k++) { r->sn[k] = o[112 + k]; r->sp[k] = o[176 + k]; }
    if (o[1] == 6u) prec_new15(r);           /* 1.4: and its scales, volumes, split */
    else prec_new16(r);
    r->used = 1;
    return 1;
}
static void prec_new14(prec_t *r)
{
    uint8_t *p = r->p, mix = p[16], tone = p[19];
    uint32_t t = p[P_MTYPE], mr = p[P_MRATE], k;
    p[P_NMODE] = 0; p[P_DMIX] = mix; p[P_DTONE] = tone; p[P_NVAR] = 0; p[P_NPASSO] = 50; p[P_NCASO] = 0;
    for (k = 0; k < NL; k++) p[P_PN1 + k] = 100;
    if (p[P_L2T] >= 24u) p[P_L2T] += 5u;
    switch (t) {
    case 1: p[P_MTYPE] = MT_MET + 50u; p[P_MRATE] = (uint8_t)metal_at(RING_HZ[mr > 100u ? 100u : mr] * 10u); p[P_MDEPTH] = 0; break;
    case 2: p[P_MTYPE] = MT_VEL + 100u; break;
    case 3: p[P_MTYPE] = MT_VEL + 66u; break;
    case 4: p[P_MTYPE] = mr >= 50u ? MT_MET + 100u : MT_MET;
            p[P_MRATE] = (uint8_t)metal_at(SHIFT_DHZ[mr >= 50u ? (mr - 50u > 50u ? 50u : mr - 50u) : 50u - mr]); break;
    case 5: p[P_MTYPE] = MT_VEL; break;
    case 6: {                                /* STUTTER: NASTRO's RIPETI */
        uint32_t i, ms, best = 0;
        p[P_MTYPE] = 0;
        p[P_NMODE] = 1;
        if (mr >= MR_SYNC) p[P_DTIME] = (uint8_t)(NT_SYNC + (mr - MR_SYNC) % 14u);
        else {
            ms = (441u + mr * mr * 7718u / 7396u) * 10u / 441u;
            for (i = 0; i < 64u; i++) if (DLY_MS[i] <= ms) best = i;
            p[P_DTIME] = (uint8_t)best;
        }
        p[P_NVAR] = p[P_MDEPTH];
        p[P_DMIX] = p[P_MMIX];
        p[P_DFB] = 44;                       /* 8 repeats, as the stutter did */
        p[P_MDEPTH] = 50; p[P_MMIX] = 50;
        break; }
    default: p[P_MTYPE] = 0; break;
    }
    prec_new15(r);                           /* and on to 1.5 */
}
static int prec_from_13(prec_t *r)         /* a record of 1.3 (80 parameters in 96, its sum at 240): to this one */
{
    static uint8_t o[PREC_SIZE];
    uint32_t k, h = 2166136261u, sum;
    if (user_read((uint32_t)(r - pr) * PREC_SIZE, o, PREC_SIZE) || o[0] != PREC_MAGIC || o[1] != 5u || o[2] != 80u) return 0;
    for (k = 0; k < 240u; k++) h = (h ^ o[k]) * 16777619u;
    sum = o[240] | (uint32_t)o[241] << 8 | (uint32_t)o[242] << 16 | (uint32_t)o[243] << 24;
    if (sum != h) return 0;
    for (k = 0; k < sizeof *r; k++) ((uint8_t *)r)[k] = 0;
    for (k = 0; k < 12u; k++) r->name[k] = (char)o[4 + k];
    for (k = 0; k < 80u; k++) r->p[k] = o[16 + k];
    for (k = 0; k < NSTEP; k++) { r->sn[k] = o[112 + k]; r->sp[k] = o[176 + k]; }
    prec_new14(r);
    r->used = 1;
    return 1;
}
/* 1.8: two factory presets an engine, in places 9..14, each on one of the eight above */
#define NFACT18 14u
typedef struct { uint8_t base, snd, mot, m[4]; char name[12]; uint8_t ov[8][2]; } fact18_t;
static const fact18_t FACT18[NFACT18 - NFACT] = {
    {4, 4, MO_STIRA, {12, 45, 10, 25}, "MARE FERMO", {{P_SIZE, 44}, {P_FREEZE, 0}, {P_RMIX, 70}, {P_RDEC, 95}, {P_ATK, 40}, {P_REL, 52}, {P_VOL, 100}, {255, 0}}},
    {0, 0, MO_STIRA, {25, 25, 45, 15}, "CATTEDRALE", {{P_SIZE, 46}, {P_SHIM, 40}, {P_RMIX, 65}, {P_ATK, 36}, {P_REL, 50}, {255, 0}}},
    {0, 0, MO_FORMANTI, {42, 45, 15, 30}, "CORO BASSO", {{P_RMIX, 45}, {P_ATK, 24}, {P_REL, 40}, {P_L1D, 100}, {P_L2D, 100}, {P_SND2, 0}, {255, 0}}},
    {3, 3, MO_FORMANTI, {58, 35, 45, 10}, "SUSSURRO", {{P_RMIX, 50}, {P_ATK, 20}, {P_REL, 38}, {P_L1D, 100}, {P_L2D, 100}, {P_SND2, 0}, {P_VOL, 100}, {255, 0}}},
    {2, 5, MO_RISON, {70, 60, 0, 85}, "CORDE", {{P_SIZE, 6}, {P_DENS, 8}, {P_ATK, 0}, {P_REL, 44}, {P_RMIX, 30}, {P_DMIX, 20}, {255, 0}}},
    {1, 6, MO_RISON, {92, 80, 70, 90}, "CAMPANE", {{P_SIZE, 12}, {P_DENS, 4}, {P_ATK, 0}, {P_REL, 54}, {P_RMIX, 55}, {255, 0}}},
};
static void prec_factory(uint32_t i)
{
    uint32_t k, b = i;
    prec_t *r = &pr[i];
    const fact18_t *f = i >= NFACT && i < NFACT18 ? &FACT18[i - NFACT] : 0;
    for (k = 0; k < sizeof *r; k++) ((uint8_t *)r)[k] = 0;
    if (i >= NFACT18) return;
    if (f) b = f->base;
    for (k = 0; k < NP5; k++) r->p[k] = PRESET0[b][k];
    prec_new14(r);
    if (f) {
        r->p[P_SND] = f->snd; r->p[P_MOTORE] = f->mot;
        for (k = 0; k < 4u; k++) r->p[P_M1 + k] = f->m[k];
        for (k = 0; k < 8u && f->ov[k][0] != 255u; k++) r->p[f->ov[k][0]] = f->ov[k][1];
        for (k = 0; k < 11u && f->name[k]; k++) r->name[k] = f->name[k];
    }
    seq_default(b, r->sn, r->sp);
    r->used = 1;
}
static void prec_new13(prec_t *r)           /* from before 1.3: the layers as before, LFO 2's last destinations moved on */
{
    uint32_t L;
    if (r->p[P_L2T] >= 20u) r->p[P_L2T] += 4u;   /* VEL. and INT. LFO 1: after PITCH 1..4 */
    for (L = 0; L < NL; L++) { r->p[P_LP1 + L] = 120; r->p[P_LS1 + L] = 100; r->p[P_LA1 + L] = 100; }
}
static int prec_from_09(prec_t *r)         /* a record of 0.9 (60 parameters in 64, its sum at 208): to this one */
{
    static uint8_t o[PREC_SIZE];
    uint32_t k, h = 2166136261u, sum;
    if (user_read((uint32_t)(r - pr) * PREC_SIZE, o, PREC_SIZE) || o[0] != PREC_MAGIC || o[1] != 3u || o[2] != 60u) return 0;
    for (k = 0; k < 208u; k++) h = (h ^ o[k]) * 16777619u;
    sum = o[208] | (uint32_t)o[209] << 8 | (uint32_t)o[210] << 16 | (uint32_t)o[211] << 24;
    if (sum != h) return 0;
    for (k = 0; k < sizeof *r; k++) ((uint8_t *)r)[k] = 0;
    for (k = 0; k < 12u; k++) r->name[k] = (char)o[4 + k];
    for (k = 0; k < 60u; k++) r->p[k] = o[16 + k];
    r->p[P_CRUSH] = o[16 + 13]; r->p[P_COMP] = 0;            /* CRUSH moved to SPECIALI, the compressor off */
    if (r->p[P_L2T] >= 16u) r->p[P_L2T] += 4u;                 /* LFO 2 on LFO 1: after the four new destinations */
    r->p[P_MTYPE] = 0; r->p[P_MRATE] = 30; r->p[P_MDEPTH] = 50; r->p[P_MMIX] = 50;
    r->p[P_SHIM] = 0; r->p[P_SHINT] = 0; r->p[P_SHTONE] = 60; r->p[P_SHDIFF] = 50;
    for (k = 0; k < NSTEP; k++) { r->sn[k] = o[80 + k]; r->sp[k] = o[144 + k]; }
    prec_new13(r);
    prec_new14(r);
    r->used = 1;
    return 1;
}
static int prec_from_10(prec_t *r)         /* a record of 1.0..1.2 (68 parameters in 72, its sum at 216): to this one */
{
    static uint8_t o[PREC_SIZE];
    uint32_t k, h = 2166136261u, sum;
    if (user_read((uint32_t)(r - pr) * PREC_SIZE, o, PREC_SIZE) || o[0] != PREC_MAGIC || o[1] != 4u || o[2] != 68u) return 0;
    for (k = 0; k < 216u; k++) h = (h ^ o[k]) * 16777619u;
    sum = o[216] | (uint32_t)o[217] << 8 | (uint32_t)o[218] << 16 | (uint32_t)o[219] << 24;
    if (sum != h) return 0;
    for (k = 0; k < sizeof *r; k++) ((uint8_t *)r)[k] = 0;
    for (k = 0; k < 12u; k++) r->name[k] = (char)o[4 + k];
    for (k = 0; k < 68u; k++) r->p[k] = o[16 + k];
    for (k = 0; k < NSTEP; k++) { r->sn[k] = o[88 + k]; r->sp[k] = o[152 + k]; }
    prec_new13(r);
    prec_new14(r);
    r->used = 1;
    return 1;
}
static void presets_load(void)
{
    uint32_t i, k;
    for (i = 0; i < NPRESET; i++) {
        prec_t *r = &pr[i];
        if (user_read(i * PREC_SIZE, r, sizeof *r) || r->magic != PREC_MAGIC || r->ver != PREC_VER || r->np != NP ||
            r->sum != prec_sum(r)) {
            if (prec_up18(r) || prec_from_14(r) || prec_from_13(r) || prec_from_10(r) || prec_from_09(r)) save_sectors |= 1u << (i / 16u);   /* converted: written again at the next save */
            else { prec_factory(i); continue; }
        }
        r->used = 1;
        r->name[11] = 0;
        for (k = 0; k < NP; k++) if (r->p[k] > PMAX[k]) r->p[k] = PMAX[k];
        for (k = 0; k < NSTEP; k++) {
            if (r->sn[k] > 127u) r->sn[k] = 0;
            if (r->sp[k] > 200u) r->sp[k] = 255;
        }
    }
    if (settings_load(&store, sizeof store) || store.ver != SET_VER) {   /* 1.1's settings: only the preset in use */
        struct { uint8_t ver, cur, pad[2]; } old;
        static set12_t o12;
        for (k = 0; k < sizeof store; k++) ((uint8_t *)&store)[k] = 0;
        if (!settings_load(&store, SET14_SIZE) && store.ver == SET_VER2) { /* 1.4: all kept, no bond yet */ }
        else if (for_zero(), !settings_load(&o12, sizeof o12) && o12.ver == SET_VER1)   /* 1.2 .. 1.3: the MIDI page kept */
            for (k = 0; k < sizeof o12; k++) ((uint8_t *)&store)[k] = ((uint8_t *)&o12)[k];
        else if (!settings_load(&old, sizeof old) && old.ver == SET_VER0) store.cur = old.cur;
        store.ver = SET_VER;
    }
    if (store.nlearn > NLEARN) store.nlearn = 0;
    if (store.rx > 17u) store.rx = 0;
    clk.ext = store.clk_in & 1u;
    clk.out = store.clk_out & 1u;
    cur_preset = store.cur % NPRESET;
    if (!pr[cur_preset].used) cur_preset = 0;
}
static void presets_flush(void)             /* the sectors with a saved preset: erased, then their records */
{
    uint32_t sct, j;
    for (sct = 0; sct < NPRESET / 16u; sct++) {
        if (!(save_sectors >> sct & 1u)) continue;
        if (user_erase(sct * 4096u)) continue;
        for (j = sct * 16u; j < sct * 16u + 16u; j++) {
            prec_t *r = &pr[j];
            if (!r->used) continue;
            r->magic = PREC_MAGIC; r->ver = PREC_VER; r->np = NP;
            r->sum = prec_sum(r);
            user_write(j * PREC_SIZE, r, sizeof *r);
        }
    }
    save_sectors = 0;
}
static const char *preset_name(uint32_t i)
{
    if (!pr[i].used) return "VUOTO";
    return pr[i].name[0] ? pr[i].name : snd_name(pr[i].p[P_SND]);   /* the first layer's */
}
static uint8_t base[NP];                    /* the knobs' own values, under what the dice did */
static uint32_t dice_on;
static void seq_recall(uint32_t i)
{
    uint32_t k;
    for (k = 0; k < NSTEP; k++) { seq_pos[k] = pr[i].sp[k]; seq_note[k] = pr[i].sn[k]; }
}
static void preset_recall(uint32_t i)
{
    uint32_t k;
    cur_preset = i % NPRESET;
    t_cur = fm1_ms;                         /* the one in use, kept a few seconds later */
    if (!pr[cur_preset].used) { edited = 1; return; }   /* an empty place: what plays stays, ready to be saved there */
    for (k = 0; k < NP; k++) par[k] = base[k] = pr[cur_preset].p[k];
    if (par[P_SND] >= nsounds()) par[P_SND] = base[P_SND] = 0;
    for (k = P_SND2; k <= P_SND4; k++) if (par[k] > nsnd) par[k] = base[k] = 0;
    seq_recall(cur_preset);
    edited = 0;
    layers_sync(0);
    engine_update();
    drone_update();
}

/* ---- the screen */
#define C_ACC RGB(255, 176, 64)             /* amber grains */
#define C_ACC2 RGB(255, 236, 190)
#define C_WAV RGB(120, 150, 170)
#define C_WAVD RGB(48, 60, 70)
#define C_AREA RGB(26, 34, 44)
#define C_HALO RGB(13, 17, 23)                  /* the spray's and the grain's reach */
#define C_WAVH RGB(80, 100, 116)
#define C_LAB RGB(130, 130, 140)
#define C_TAB RGB(70, 70, 80)
/* 1.7: the panels the knobs' values sit on, the header's band */
#define C_CARD RGB(14, 15, 19)
#define C_EDGE RGB(34, 36, 44)
#define C_TRACK RGB(32, 34, 42)
#define C_BAND RGB(16, 15, 14)
#define C_RULE RGB(54, 40, 22)
static void cell_begin(void) { cv_begin(118, 50, C_BLACK); cv_card(0, 0, 118, 50, C_CARD, C_EDGE, C_BLACK); }
static void cell_value(const char *s, uint16_t col)   /* the value, big unless it does not fit */
{
    const felucca_font_t *f = &FONT_L;
    if (text_w(f, s) > 112) f = &FONT_S;
    cv_text(6, f == &FONT_L ? 14 : 21, f, s, col);
}
#define WAV_Y 28
#define WAV_H 86
static uint32_t page, dirty_hdr = 1, dirty_tabs = 1, dirty_cells = 0xF;
static uint32_t learn_on, learn_have, learn_st, learn_num, t_learn, t_lmsg, del_all;   /* MIDI LEARN (1.2) */
static int32_t learn_par = -1;
static char lmsg[40];                       /* what LEARN says in the tabs' strip */

static void fmt_u(char *s, uint32_t v)      /* decimal, no printf */
{
    char t[12];
    uint32_t n = 0, i;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 11u);
    for (i = 0; i < n; i++) s[i] = t[n - 1u - i];
    s[n] = 0;
}
static char *cat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
#ifdef FL_JEDEC_OK
/* which flash this FM-1 has (1.7.1): PUYA, UC25 (newer FM-1s) or FLASH ?, then L n for late operations */
static char *flash_tag(char *e)
{
    e = cat(e, flash_id == 0x856014u ? "PUYA" : flash_id == 0xB36014u ? "UC25" : "FLASH ?");
    if (fl_late_ops) { e = cat(e, " L"); fmt_u(e, fl_late_ops); while (*e) e++; }
    return e;
}
#endif
static void fmt_s(char *s, int32_t v)
{
    if (v > 0) *s++ = '+';
    else if (v < 0) { *s++ = '-'; v = -v; }
    fmt_u(s, (uint32_t)v);
}

static void note_name(char *s, uint32_t n)
{
    static const char *const NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char *e = cat(s, NM[n % 12u]);
    fmt_u(e, n / 12u - 1u);
}
static uint32_t fcut_hz(uint32_t v)         /* the cutoff's knob in Hz, as the filter's mode reads it */
{
    uint32_t idx, at, f;
    idx = par[P_FMODE] < 3u ? v * 1023u / 200u : FC_LO + v * FC_SPAN / 200u;   /* a step of the 1024, then in the 128 by interpolation */
    at = idx * 127u * 256u / 1023u; f = at & 255u; at >>= 8;
    if (at >= 127u) return FCUT_HZ[127];
    return FCUT_HZ[at] + (((uint32_t)FCUT_HZ[at + 1u] - FCUT_HZ[at]) * f >> 8);
}
static void mod_rate_text(char *s, uint32_t v)   /* MODULAZIONE's second knob: VELVET's speed, METALLO's frequency */
{
    uint32_t t = par[P_MTYPE];
    char *e;
    if (t >= MT_MET) {
        uint32_t d;
        metal_init();
        d = METAL_DHZ[v > 100u ? 100u : v];   /* tenths of Hz */
        fmt_u(s, d / 10u); e = s; while (*e) e++;
        if (d < 1000u) { *e++ = '.'; *e++ = (char)('0' + d % 10u); }
        cat(e, " HZ");
        return;
    }
    if (v >= MR_SYNC) { cat(cat(s, "SYNC "), MSYNCN[(v - MR_SYNC) % 14u]); return; }
    {
        uint32_t h = MRATE_DHZ[v];             /* hundredths of Hz */
        fmt_u(s, h / 100u); e = s; while (*e) e++;
        if (h < 1000u) { *e++ = '.'; *e++ = (char)('0' + h / 10u % 10u); if (h < 100u) *e++ = (char)('0' + h % 10u); }
        cat(e, " HZ");
    }
}
static const char *pname_of(uint32_t p)      /* MODULAZIONE and NASTRO rename their knobs by type and mode */
{
    uint32_t t = par[P_MTYPE], m = par[P_NMODE];
    if (p >= P_M1 && p <= P_M4) return MOT_KNOB[par[P_MOTORE] & 3u][p - P_M1];   /* 1.8: the engine's own */
    if (p == P_MTYPE) return !t ? "TIPO" : t < MT_MET ? "VELVET" : "METALLO";
    if (p == P_MRATE) return t >= MT_MET ? "FREQUENZA" : "VELOCITA'";
    if (p == P_MDEPTH) return t >= MT_MET ? "RITORNO" : "PROFONDITA'";
    if (p == P_DTIME) return m == 1u ? "FETTA" : m == 4u ? "LUNGHEZZA" : "TEMPO";
    if (p == P_DFB) return m == 1u ? "RIPETIZIONI" : "RITORNO";
    if (p == P_NVAR) return m == 1u ? "PROBABILITA'" : m == 4u ? "DERIVA" : m == 2u ? "VARIAZIONE" : "WOW";
    return PNAME[p];
}
static void fmt_hund(char *s, uint32_t h)  /* 1.5: hundredths of a percent: "25%", "2.5%", "0.25%" */
{
    char *e = s;
    if (h >= 1000u) { fmt_u(e, (h + 50u) / 100u); while (*e) e++; }
    else if (h >= 100u) { fmt_u(e, h / 100u); while (*e) e++; *e++ = '.'; *e++ = (char)('0' + h / 10u % 10u); }
    else { *e++ = '0'; *e++ = '.'; *e++ = (char)('0' + h / 10u); *e++ = (char)('0' + h % 10u); }
    cat(e, "%");
}
static int mot_ignores(uint32_t p)          /* 1.8: a knob the engine does without */
{
    uint32_t m = par[P_MOTORE];
    if (m == MO_STIRA) return p == P_SCAN || p == P_DIR || p == P_SPRAY || p == P_DENS || p == P_JIT || p == P_WIN || p == P_REV;
    if (m == MO_FORMANTI) return p == P_SIZE || p == P_DENS || p == P_JIT || p == P_SPRAY || p == P_SCAN || p == P_DIR;
    return 0;
}
static void value_text(uint32_t p, char *s)
{
    uint32_t v = par[p];
    char *e = s;
    *s = 0;
    if (mot_ignores(p)) { cat(s, "-"); return; }
    if (p == P_MOTORE) { cat(s, MOT_NAME[v & 3u]); return; }
    if (p >= P_M1 && p <= P_M4) {           /* 1.8: the engine's own */
        uint32_t m = par[P_MOTORE] & 3u, k = p - P_M1;
        if (m == MO_CLASSICO) { cat(s, "-"); return; }
        if (m == MO_STIRA && k == 0u) { if (!v) cat(s, "FERMO"); else fmt_hund(s, v * v); return; }
        if (m == MO_STIRA && k == 1u) { fmt_u(s, v * v * 500u / 10000u); e = s; while (*e) e++; cat(e, "MS"); return; }
        if (m == MO_STIRA && k == 3u) { fmt_u(s, v * 40u / 100u); e = s; while (*e) e++; cat(e, " CENT"); return; }
        if (m == MO_FORMANTI && k == 0u) {   /* semitones, a decimal */
            int32_t c = ((int32_t)v - 50) * 24, a = c < 0 ? -c : c;
            *e++ = c < 0 ? '-' : '+';
            fmt_u(e, (uint32_t)a / 100u); while (*e) e++; *e++ = '.'; *e++ = (char)('0' + (uint32_t)a / 10u % 10u);
            cat(e, " ST"); return;
        }
        if (m == MO_FORMANTI && k == 1u) {   /* milliseconds, a decimal */
            uint32_t t = 20u + v * v * 280u / 10000u;
            fmt_u(s, t / 10u); e = s; while (*e) e++; *e++ = '.'; *e++ = (char)('0' + t % 10u); cat(e, "MS"); return;
        }
        if (m == MO_FORMANTI && k == 3u) { if (!v) cat(s, "OFF"); else { fmt_u(s, v * 50u / 100u); e = s; while (*e) e++; cat(e, " CENT"); } return; }
        fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); return;
    }
    switch (p) {
    case P_POS: fmt_u(s, v / 2u); e = s; while (*e) e++; cat(e, v & 1u ? ".5%" : "%"); break;
    case P_SIZE: { uint32_t ms = SIZE_MS[v]; if (ms >= 1000u) { fmt_u(s, ms / 1000u); e = s; while (*e) e++; *e++ = '.'; *e++ = (char)('0' + ms / 100u % 10u); cat(e, "S"); } else { fmt_u(s, ms); e = s; while (*e) e++; cat(e, "MS"); } break; }
    case P_DENS: { uint32_t d = DENS_DHZ[v]; fmt_u(s, d / 10u); e = s; while (*e) e++; if (d < 100u) { *e++ = '.'; *e++ = (char)('0' + d % 10u); } cat(e, "/S"); break; }
    case P_SPRAY: case P_REV: case P_PAN: case P_VOL:   /* 1.5: no longer through P_DFB (in RIPETI they showed "X n") */
        fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); break;
    case P_AREA: fmt_hund(s, v * v / 4u); break;          /* 1.5: its square */
    case P_VO1: case P_VO2: case P_VO3: case P_VO4: if (!v) cat(s, "OFF"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_DISP: cat(s, v ? "SPLIT" : "SOVRAPPOSTI"); break;
    case P_SP1: case P_SP2: case P_SP3:
        if (!par[P_DISP]) { cat(s, "-"); break; }
        if ((uint32_t)(p - P_SP1) + 1u >= zone_n) { e = cat(s, "("); note_name(e, v); while (*e) e++; cat(e, ")"); break; }   /* unused: too few layers */
        note_name(s, v);
        break;
    case P_NCASO: if (par[P_NMODE] != 1u) cat(s, "-"); else cat(s, v ? "SI" : "NO"); break;
    case P_DFB:
        if (par[P_NMODE] == 1u) { cat(s, "X "); fmt_u(s + 2, 1u + v * 15u / 95u); break; }
        if (par[P_NMODE] == 4u) { cat(s, "-"); break; }
        fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); break;
    case P_DTONE: case P_RTONE: case P_RMOD: fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); break;
    case P_DMIX: case P_RMIX: if (!v) cat(s, "OFF"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_RDEC: if (v >= 100u) cat(s, "INFINITA"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_DTIME:
        if (v >= NT_SYNC) { cat(cat(s, "SYNC "), MSYNCN[(v - NT_SYNC) % 14u]); break; }
        { uint32_t ms = DLY_MS[v]; if (ms >= 1000u) cat(s, "1.0S"); else { fmt_u(s, ms); e = s; while (*e) e++; cat(e, "MS"); } break; }
    case P_NMODE: cat(s, NMODEN[v % 5u]); break;
    case P_NVAR: if (!v) cat(s, "OFF"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_NPASSO: {
        uint32_t m = par[P_NMODE];
        if (m == 1u) {                       /* RIPETI: each repeat shorter, or lower */
            if (v < 40u) cat(s, "ACCORCIA");
            else if (v <= 60u) cat(s, "0");
            else { e = s; *e++ = '-'; fmt_u(e, (v - 60u) * 30u / 100u); while (*e) e++;
                   if ((v - 60u) * 30u % 100u) { *e++ = '.'; *e++ = (char)('0' + (v - 60u) * 30u % 100u / 10u); } cat(e, " ST/RIP"); }
        } else if (m == 3u || m == 4u) {     /* NASTRO, CONGELA: semitones */
            int32_t st = ((int32_t)v - 50) / 4;
            if (!st) cat(s, "0"); else { fmt_s(s, st); e = s; while (*e) e++; cat(e, " ST"); }
        } else cat(s, "-");
        break; }
    case P_PN1: case P_PN2: case P_PN3: case P_PN4:
        if (v == 100u) cat(s, "CENTRO");
        else { e = cat(s, v < 100u ? "SX " : "DX "); fmt_u(e, v < 100u ? 100u - v : v - 100u); }
        break;
    case P_SCAN: if (!v) cat(s, "FERMO"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_DIR: cat(s, DIRN[v & 3u]); break;
    case P_SALTO: if (!v) cat(s, "OFF"); else fmt_hund(s, v * v); break;
    case P_L1W: case P_L2W: cat(s, LFOW[v % 7u]); break;
    case P_L1R: case P_L2R:
        if (v >= LR_SYNC) { cat(s, SYNCN[(v - LR_SYNC) % 7u]); break; }
        {   /* slow: seconds a cycle; fast: hertz */
            uint32_t per = 0xFFFFFFFFu / lfo_inc(v);             /* ms */
            if (per >= 1000u) { fmt_u(s, per / 1000u); e = s; while (*e) e++; if (per < 10000u) { *e++ = '.'; *e++ = (char)('0' + per / 100u % 10u); } cat(e, " S"); }
            else { uint32_t dh = 10000u / per; fmt_u(s, dh / 10u); e = s; while (*e) e++; if (dh < 100u) { *e++ = '.'; *e++ = (char)('0' + dh % 10u); } cat(e, " HZ"); }
        }
        break;
    case P_L1D: case P_L2D: {
        int32_t d = lfo_depth((int32_t)v - 100);
        if (!d) { cat(s, "OFF"); break; }
        *s = d > 0 ? '+' : '-';
        fmt_hund(s + 1, (uint32_t)(d < 0 ? -d : d));
        break; }
    case P_L1T: case P_L2T: cat(s, LFOT[v % 31u]); break;   /* 1.5: % 31 (was 26: PAN 2..4 and LFO 1 showed other names) */
    case P_PITCH: if (v == 24u) cat(s, "0"); else fmt_s(s, (int32_t)v - 24); e = s; while (*e) e++; cat(e, " ST"); break;
    case P_JIT: if (!v) cat(s, "OFF"); else { fmt_u(s, v * 12u); e = s; while (*e) e++; cat(e, " CT"); } break;
    case P_WIN: cat(s, WNAME[v & 3u]); break;
    case P_ATK: case P_REL: { uint32_t ms = ENV_MS[v]; if (ms >= 1000u) { fmt_u(s, ms / 1000u); e = s; while (*e) e++; *e++ = '.'; *e++ = (char)('0' + ms / 100u % 10u); cat(e, "S"); } else { fmt_u(s, ms); e = s; while (*e) e++; cat(e, "MS"); } break; }
    case P_CRUSH: if (!v) cat(s, "OFF"); else fmt_u(s, v); break;
    case P_COMP: case P_SHIM: case P_MMIX: if (!v) cat(s, "OFF"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_MTYPE:
        if (!v) cat(s, "OFF");
        else if (v < MT_MET) {               /* VELVET: where CARATTERE stands */
            uint32_t c = v - MT_VEL;
            e = cat(s, c < 17u ? "CHORUS L " : c < 34u ? "CHORUS " : c < 50u ? "CHOR/FLA " : c < 67u ? "FLANGER " : c < 84u ? "FLA/PHA " : "PHASER ");
            fmt_u(e, c);
        } else {                             /* METALLO: LATO */
            int32_t l = ((int32_t)v - (int32_t)MT_MET) * 2 - 100;
            if (!l) cat(s, "RING");
            else { e = cat(s, l < 0 ? "GIU' " : "SU "); fmt_u(e, (uint32_t)(l < 0 ? -l : l)); }
        }
        break;
    case P_MRATE: mod_rate_text(s, v); break;
    case P_MDEPTH: case P_SHTONE: case P_SHDIFF: fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); break;
    case P_SHINT: cat(s, SHINTN[v % 5u]); break;
    case P_AMODE: cat(s, AMODE[v % 5u]); break;
    case P_ADIV: case P_SDIV: cat(s, DIVN[v % 5u]); break;
    case P_AOCT: fmt_u(s, v + 1u); e = s; while (*e) e++; cat(e, " OTT"); break;
    case P_AGATE: fmt_u(s, v < 5u ? 5u : v); e = s; while (*e) e++; cat(e, "%"); break;
    case P_TEMPO: fmt_u(s, v + 20u); e = s; while (*e) e++; cat(e, " BPM"); break;
    case P_SWING: fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); break;
    case P_SLEN: fmt_u(s, v + 1u); break;
    case P_DRONE: if (!v) cat(s, "OFF"); else if (v == 1u) cat(s, "ULTIMA"); else note_name(s, 36u + v - 2u); break;
    case P_FREEZE: cat(s, v ? "ON" : "OFF"); break;
    case P_FMODE: cat(s, FMODE[v % 7u]); break;
    case P_SND: fmt_u(s, v + 1u); e = s; while (*e) e++; *e++ = ' '; cat(e, snd_name(v)); break;
    case P_SND2: case P_SND3: case P_SND4:
        if (!v) cat(s, "OFF"); else { fmt_u(s, v); e = s; while (*e) e++; *e++ = ' '; cat(e, snd_name(v - 1u)); }
        break;
    case P_LV1: case P_LV2: case P_LV3: case P_LV4: if (!v) cat(s, "OFF"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_FCUT: { uint32_t hz = fcut_hz(v); if (hz >= 1000u) { fmt_u(s, hz / 1000u); e = s; while (*e) e++; *e++ = '.'; *e++ = (char)('0' + hz / 100u % 10u); cat(e, "K"); } else { fmt_u(s, hz); e = s; while (*e) e++; cat(e, " HZ"); } break; }
    case P_FRES: fmt_u(s, v / 2u); e = s; while (*e) e++; cat(e, v & 1u ? ".5%" : "%"); break;
    case P_FLFO: if (!v) cat(s, "OFF"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_DADO: if (!v) cat(s, "OFF"); else { fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); } break;
    case P_OGNI: if (!v) cat(s, "A MANO"); else { fmt_u(s, OGNI_N[v & 7u]); e = s; while (*e) e++; cat(e, "/16"); } break;
    case P_RETRIG: cat(s, v ? "SI" : "NO"); break;
    case P_SCALA: cat(s, SCALEN[v % NSCALE]); break;
    case P_TONICA: { static const char *const NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        if (!par[P_SCALA]) cat(s, "-"); else cat(s, NM[v % 12u]); break; }
    case P_BIT12: cat(s, v ? "12 BIT" : "16 BIT"); break;
    case P_TASTO: cat(s, v ? "SPEGNE" : "RIPARTE"); break;
    case P_LP1: case P_LP2: case P_LP3: case P_LP4: {   /* cents near 0, then semitones (and quarters) */
        int32_t c = lp_cents(v), a = c < 0 ? -c : c;
        e = s;
        if (c) *e++ = c > 0 ? '+' : '-';
        if (!a) { cat(s, "0"); break; }
        if (a < 100) { fmt_u(e, (uint32_t)a); while (*e) e++; cat(e, " CT"); break; }
        fmt_u(e, (uint32_t)a / 100u); while (*e) e++;
        if (a % 100u) { *e++ = '.'; *e++ = (char)('0' + a % 100 / 10); if (a % 10) *e++ = (char)('0' + a % 10); }
        cat(e, " ST");
        break; }
    case P_LS1: case P_LS2: case P_LS3: case P_LS4:
        if (v == 100u) cat(s, "0"); else { int32_t d = (int32_t)v - 100; e = s; *e++ = d > 0 ? '+' : '-'; d = d < 0 ? -d : d;
            fmt_u(e, (uint32_t)d / 2u); while (*e) e++; cat(e, d & 1 ? ".5%" : "%"); }
        break;
    case P_LA1: case P_LA2: case P_LA3: case P_LA4: fmt_u(s, v); e = s; while (*e) e++; cat(e, "%"); break;
    default: break;
    }
}

static uint8_t bt_link_mark;                /* GRANA + Bluetooth: 1 on, 2 linked (the header's mark) */
#if GRANA_BT
static void bt_strip(char *t);
#endif
static void fit_text(char *s, int32_t w)   /* 1.5: cut a text to w pixels (a dot where it was cut) */
{
    uint32_t n = 0;
    while (s[n]) n++;
    if (text_w(&FONT_S, s) <= w) return;
    while (n > 1u) {
        s[--n] = 0;
        s[n - 1u] = '.';
        if (text_w(&FONT_S, s) <= w) return;
    }
}
/* 1.8: the battery. The stock firmware's levels on ADC ch3 (531 / 561 / 591, as Felucca read them from it): 4 bars at 591,
 * 3 at 561, 2 (yellow) at 531, under it 1 red, blinking. USB power (a host): a green bolt instead */
static uint32_t batt_level(void)
{
    int32_t b = BATT_RAW();
    if (BATT_USB()) return 5u;
    return b >= 591 ? 4u : b >= 561 ? 3u : b >= 531 ? 2u : 1u;
}
static uint32_t batt_shown(void) { uint32_t l = batt_level(); return l == 1u && (fm1_ms >> 9 & 1u) ? 0u : l; }   /* 0: the blink's off half */
static int32_t batt_draw(void)               /* on the header's canvas, right-aligned; returns its left edge */
{
    uint32_t l = batt_shown(), k;
    int32_t x = 210, y = 6;
    uint16_t c = l >= 5u ? RGB(80, 220, 110) : l >= 3u ? RGB(80, 210, 100) : l == 2u ? RGB(240, 200, 60) : RGB(255, 70, 60);
    uint16_t o = l >= 5u ? c : RGB(150, 146, 140);
    cv_rect(x, y, 24, 1, o); cv_rect(x, y + 11, 24, 1, o); cv_rect(x, y, 1, 12, o); cv_rect(x + 23, y, 1, 12, o);
    cv_rect(x + 24, y + 3, 2, 6, o);         /* the cap */
    if (l >= 5u) {                           /* USB: a bolt */
        static const int8_t B[9][2] = {{13, 2}, {12, 3}, {11, 4}, {10, 5}, {9, 6}, {13, 5}, {12, 6}, {11, 7}, {10, 8}};
        for (k = 0; k < 9u; k++) cv_rect(x + B[k][0] - 1, y + B[k][1] + 1, 4, 1, c);
    } else
        for (k = 0; k < l && k < 4u; k++) cv_rect(x + 2 + 5 * (int32_t)k, y + 2, 4, 8, c);
    return x;
}
static uint32_t batt_seen;                   /* what the header shows now: redrawn when it changes */
static void draw_header(void)
{
    char s[24], nm[16], *e;
    int32_t x, lx, rx, k;
    cv_begin(240, 24, C_BAND);               /* 1.7: a band, a warm rule under it */
    cv_rect(0, 23, 240, 1, C_RULE);
    {   /* 1.5: the preset in use where "GRANA" was, then the scan's direction and the Bluetooth mark */
        static const char *const DI[4] = {">", "<", "<>", "?"};
        const char *pn = preset_name(cur_preset);
        for (k = 0; k < 12 && pn[k]; k++) nm[k] = pn[k];
        nm[k] = 0;
        fit_text(nm, 84);
        lx = cv_text(6, 4, &FONT_S, nm, C_ACC);
        lx = cv_text(lx + 6, 4, &FONT_S, DI[par[P_DIR] & 3u], C_LAB);   /* the scan's direction */
        if (bt_link_mark) lx = cv_text(lx + 4, 4, &FONT_S, "BT", bt_link_mark == 2u ? RGB(110, 200, 255) : C_TAB);
    }
    rx = batt_draw() - 6;                    /* 1.8: the battery on the right, as the stock firmware has it */
    {   /* 1.8, the middle: the preset's number in its tag and the engine (the sound is on HOME's STRATI) */
        char t[24];
        int32_t w, tw;
        e = t; *e++ = 'P'; fmt_u(e, (pend_preset >= 0 ? (uint32_t)pend_preset : cur_preset) + 1u); while (*e) e++;
        if (edited && pend_preset < 0) *e++ = '*';
        *e = 0;
        e = s;
        if (pend_preset >= 0) cat(e, preset_name((uint32_t)pend_preset));   /* the preset the knob points at */
        else cat(e, MOT_NAME[par[P_MOTORE] & 3u]);
        lx += 10;
        tw = text_w(&FONT_S, t);
        fit_text(s, rx - lx - tw - 18);
        w = tw + 10 + 6 + text_w(&FONT_S, s);
        x = 128 - w / 2;
        if (x < lx) x = lx;
        if (x + w > rx) x = rx - w;
        cv_card(x, 3, tw + 10, 18, pend_preset >= 0 ? RGB(60, 40, 14) : RGB(30, 28, 26),
                pend_preset >= 0 ? C_ACC : RGB(58, 54, 50), C_BAND);
        cv_text(x + 5, 4, &FONT_S, t, pend_preset >= 0 ? C_ACC : edited ? C_ACC2 : C_LAB);
        cv_text(x + tw + 16, 4, &FONT_S, s, pend_preset >= 0 ? C_ACC : par[P_MOTORE] ? C_WHITE : C_LAB);
    }
    cv_blit(0, 0);
}

static uint32_t rec, cursor;                /* REC: the sequencer's step entry, at step `cursor` */
static uint32_t seq_clear_t;                /* 1.4: SEQ held in REC, ms: the sequence is emptied at 1000 */
static uint32_t save_mode, save_dest, t_savemode, t_savehold;   /* SAVE asked: where to (see save_to) */
static void draw_tabs(void)                 /* the page's name, then a mark a page: where it is among them */
{
    uint32_t i;
    int32_t x;
    cv_begin(240, 20, C_BLACK);
    if (save_mode) {                        /* where to: an empty place takes a press, a taken one SAVE held 1 s */
        char q[32], *e = q;
        uint32_t taken = pr[save_dest].used;
        *e++ = 'P'; fmt_u(e, save_dest + 1u); while (*e) e++; *e++ = ' ';
        cat(e, taken ? preset_name(save_dest) : "VUOTO");
        x = cv_text(6, 1, &FONT_S, q, taken ? RGB(255, 80, 70) : C_ACC);
        if (!taken) cv_text(234 - text_w(&FONT_S, "SAVE: SALVA"), 1, &FONT_S, "SAVE: SALVA", C_LAB);
        else cv_text(234 - text_w(&FONT_S, "TIENI SAVE"), 1, &FONT_S, "TIENI SAVE", C_WHITE);
        cv_rect(0, 18, 240, 2, C_AREA);
        if (taken && t_savehold) {
            int32_t el = (int32_t)(fm1_ms - t_savehold); uint32_t w = el > 0 ? (uint32_t)el * 240u / 1000u : 0u;
            cv_rect(0, 17, (int32_t)(w > 240u ? 240u : w), 3, RGB(255, 80, 70));
        } else cv_rect(0, 19, 240, 1, taken ? RGB(255, 80, 70) : C_ACC);
        cv_blit(0, 118);
        return;
    }
    if (lmsg[0]) {                          /* MIDI LEARN: waiting, or what it just did */
        cv_text(6, 1, &FONT_S, lmsg, learn_on ? C_ACC : C_WHITE);
        cv_rect(0, 19, 240, 1, learn_on ? C_ACC : C_AREA);
        cv_blit(0, 118);
        return;
    }
    x = cv_text(6, 1, &FONT_S, PAGE[page], C_WHITE);
    if (page == PG_SEQ && rec) cv_text(x + 8, 1, &FONT_S, "REC", RGB(255, 80, 70));
    if (page == PG_ARP || page == PG_SEQ) { /* 1.4: what they are doing, instead of the marks */
        char t[32], *e = t;
        uint16_t col = C_LAB;
        if (page == PG_ARP) {
            e = cat(e, clk.arp ? "ON" : "OFF");
            if (hold) { e = cat(e, " HOLD TASTO: "); e = cat(e, par[P_TASTO] ? "SPEGNE" : "RIPARTE"); }
            if (clk.arp) col = C_ACC;
        } else if (seq_clear_t) {
            e = cat(e, "TIENI: CANCELLA");
            col = C_WHITE;
        } else { e = cat(e, clk.run ? "PLAY" : "STOP"); if (clk.run) col = C_ACC; }
        if (page == PG_ARP && hold) cv_text(6, 1, &FONT_S, "ARP", C_WHITE);   /* the long line needs the room */
        cv_text(234 - text_w(&FONT_S, t), 1, &FONT_S, t, col);
        if (seq_clear_t) cv_rect(0, 17, (int32_t)(seq_clear_t > 1000u ? 240u : seq_clear_t * 240u / 1000u), 3, RGB(255, 80, 70));
        else cv_rect(0, 19, 240, 1, C_AREA);
        cv_blit(0, 118);
        return;
    }
#if GRANA_BT
    if (page == PG_BT) {
        char t[48];
        t[0] = 0;
        bt_strip(t);
        cv_text(234 - text_w(&FONT_S, t), 1, &FONT_S, t, bt_link_mark == 2u ? C_ACC : C_LAB);
        cv_rect(0, 19, 240, 1, C_AREA);
        cv_blit(0, 118);
        return;
    }
#endif
    if (page == PG_MIDI) {                  /* not among SELECT's pages: what it holds instead of the marks */
        /* the version GRANA runs and its flash chip, always to hand; shorter when it would cover MIDI */
        char t[48], *e;
        int32_t room = 234 - 8 - text_w(&FONT_S, "MIDI"), k;
        for (k = 0; k < 3; k++) {
            e = cat(t, "v" APP_VER " ");
#ifdef FL_JEDEC_OK
            e = k < 2 ? flash_tag(e) : cat(e, flash_id == 0xB36014u ? "UC25" : flash_id == 0x856014u ? "PUYA" : "FLASH ?");
#endif
            e = cat(e, k ? " " : "  ");
            if (k == 0) e = cat(e, "LEARN ");
            fmt_u(e, store.nlearn); while (*e) e++; *e++ = '/'; fmt_u(e, NLEARN);
            if (text_w(&FONT_S, t) <= room) break;
        }
        cv_text(234 - text_w(&FONT_S, t), 1, &FONT_S, t, C_LAB);
        cv_rect(0, 19, 240, 1, C_AREA);
        cv_blit(0, 118);
        return;
    }
    {   /* the pages SELECT goes through, in its order, grouped by their buttons (1.7): HOME, FX, SEL, ENV, LFO, EDIT, GLO */
        static const uint8_t GRP[7] = {6, 6, 2, 2, 3, 1, 3};
        int32_t g, j, mx = 240 - 6 - ((int32_t)sizeof PG_ORDER * 4 + 6 * 4) + 1;
        for (g = 0, i = 0; g < 7; g++, mx += 4)
            for (j = 0; j < GRP[g]; j++, i++, mx += 4) {
                int on = PG_ORDER[i] == page;
                if (on) { cv_rect(mx - 1, 6, 4, 7, C_ACC); cv_rect(mx, 7, 2, 5, C_ACC2); }
                else cv_rect(mx, 8, 2, 3, RGB(84, 84, 96));
            }
    }
    cv_rect(0, 19, 240, 1, C_EDGE);
    cv_blit(0, 118);
}

static uint8_t cell_drawn[4];
static void draw_cell(uint32_t c)
{
    uint32_t p = pp(page, c), bar;
    char s[16];
    const felucca_font_t *f = &FONT_L;
    (void)f;
    if (!PMAX[p]) {                          /* a free place: an empty panel, dimmer */
        cv_begin(118, 50, C_BLACK); cv_card(0, 0, 118, 50, C_BLACK, RGB(22, 23, 28), C_BLACK);
        cv_blit(c & 1u ? 121 : 1, c < 2u ? 140 : 190); return;
    }
    cell_begin();                            /* 1.7: on a panel, the bar in a track with a bright tip */
    cv_text(6, 2, &FONT_S, pname_of(p), C_LAB);
    value_text(p, s);
    {   /* 1.7: a knob that does nothing now ("-"): dim, no bar */
        int idle = (s[0] == '-' && !s[1]) || (s[0] == '(');
        cell_value(s, idle ? C_TAB : C_WHITE);
        bar = idle ? 0u : (p == P_FMODE ? fm_pos(par[p]) : par[p]) * 106u / PMAX[p];
    }
    cv_rect(6, 44, 106, 3, C_TRACK);
    if (bar) { cv_rect(6, 44, (int32_t)bar, 3, C_ACC); cv_rect(6 + (int32_t)bar - (bar > 1u ? 2 : 1), 44, bar > 1u ? 2 : 1, 3, C_ACC2); }
    if (epar[p] != par[p]) {                /* an LFO on it: where it is now */
        uint32_t mx = epar[p] * 106u / PMAX[p];
        int32_t xm = 6 + (int32_t)(mx > 104u ? 104u : mx);
        cv_rect(xm, 40, 2, 8, C_WHITE);
        cv_pset(xm - 1, 40, C_WHITE); cv_pset(xm + 2, 40, C_WHITE);
    }
    cell_drawn[c] = epar[p];
    cv_blit(c & 1u ? 121 : 1, c < 2u ? 140 : 190);
}

/* the sound: one lane, or (1.2) a lane a layer that sounds, each its own outline, area, spray, scans and grains in its colour */
#define RGBD(r, g, b, n) RGB((r) * (n) / 100, (g) * (n) / 100, (b) * (n) / 100)
#define LANE_COLS(r, g, b) {RGB(r, g, b), RGBD(r, g, b, 62), RGBD(r, g, b, 22), RGBD(r, g, b, 34), RGBD(r, g, b, 14), RGBD(r, g, b, 7)}
static void draw_lane(uint32_t L, int32_t y0, int32_t lh, uint32_t one)
{
    static const uint16_t LC[NL][6] = {LANE_COLS(255, 176, 64), LANE_COLS(110, 200, 255), LANE_COLS(140, 230, 110),
                                       LANE_COLS(240, 120, 220)};   /* grains, wave in the area, outside, halo, area, halo shade */
    const layer_t *ly = &lay[L];
    const uint8_t *pk = peaks[L];
    uint32_t len = ly->full, x, k;
    uint16_t cw = one ? C_WAV : LC[L][1], cd = one ? C_WAVD : LC[L][2], ch = one ? C_WAVH : LC[L][3];
    uint16_t ca = one ? C_AREA : LC[L][4], cs = one ? C_HALO : LC[L][5];
    int32_t mid = y0 + lh / 2, a0, a1, h0, h1;
    if (len < 64u || !ly->len) return;
    a0 = (int32_t)((uint32_t)ly->pos0 * 240u / len);
    a1 = a0 + (int32_t)((uint32_t)ly->area * 240u / len);
    {   /* around the area, dimmer: how far the spray throws a grain, and (to the right) how much of the sound a
         * grain reads, its length at the sound's own pitch */
        int32_t sp = (int32_t)((uint32_t)ly->spray * 240u / len), gl = (int32_t)((eng.gframes / 2u) * 240u / len);
        h0 = a0 - sp;
        h1 = a1 + sp + gl;
        if (h1 - h0 > 240) h1 = h0 + 240;
    }
    for (x = 0; x < 240u; x++) {             /* the area shaded, the outline in it brighter */
        int32_t xi = (int32_t)x, in = 0, halo = 0, w;
        for (w = -240; w <= 240; w += 240) {
            if (xi + w >= a0 && xi + w < a1) in = 1;
            if (xi + w >= h0 && xi + w < h1) halo = 1;
        }
        {
            int32_t h = (int32_t)(pk[x] * (uint32_t)(lh / 2 - 2) / 255u);
            if (in) cv_rect(xi, y0, 1, lh, ca);
            else if (halo) cv_rect(xi, y0, 1, lh, cs);
            cv_rect(xi, mid - h, 1, 2 * h + 1, in ? cw : halo ? ch : cd);
        }
    }
    for (k = 0; k < NV + 1u; k++) {          /* the scans: where the next grains come from */
        int32_t h = vis_head[k];
        if (h < 0) continue;
        if (k == NV && !quiet()) continue;
        {   /* as spawn() reads: inside the window when only part of the sound is in RAM */
            int32_t f = ly->pos0 + (int32_t)(((uint64_t)((uint32_t)h << 8) * (uint32_t)ly->area) >> 32) - ly->wstart;
            f %= (int32_t)ly->len;
            if (f < 0) f += (int32_t)ly->len;
            x = (uint32_t)(f + ly->wstart) * 240u / len;
        }
        cv_rect((int32_t)x, y0, 1, lh, k == NV ? C_TAB : C_ACC2);
        if (k < NV) { cv_rect((int32_t)x - 1, y0, 3, 1, C_ACC2); cv_rect((int32_t)x - 1, y0 + lh - 1, 3, 1, C_ACC2); }   /* 1.7: capped */
    }
    if (a1 - a0 < 240) {                     /* 1.7: the area's two edges, a thin brighter line */
        int32_t e0 = ((a0 % 240) + 240) % 240, e1 = ((a1 % 240) + 240) % 240;
        cv_rect(e0, y0, 1, lh, cw); cv_rect(e1 > 0 ? e1 - 1 : 239, y0, 1, lh, cw);
    }
    if (ly->len < len) {                     /* only part of the sound in RAM: the part, marked on top */
        int32_t w0 = (int32_t)((uint32_t)ly->wstart * 240u / len), w1 = (int32_t)(((uint32_t)ly->wstart + ly->len) * 240u / len);
        cv_rect(w0, y0, w1 - w0, 2, C_LAB);
    }
    for (k = 0; k < NV * NG; k++) {          /* its grains, as tall as they are loud */
        int32_t p = vis_pos[k], h;
        if (p < 0 || vis_lay[k] % NL != L || (uint32_t)p >= len) continue;
        x = (uint32_t)p * 240u / len;
        h = (int32_t)(vis_amp[k] * (uint32_t)(lh / 2 - (one ? 4 : 2)) / 255u) + (one ? 3 : 2);
        cv_rect((int32_t)x - 1, mid - h, 3, 2 * h, LC[L][0]);
    }
}
static void draw_wave(void)
{
    uint32_t L, n = 0, k;
    uint8_t ln[NL];
    int32_t seq = page == PG_SEQ || rec, ah, lh;
    cv_begin(240, WAV_H, C_BLACK);
    for (L = 0; L < NL; L++) if (lay[L].snd != NOSND && lay[L].full >= 64u) ln[n++] = (uint8_t)L;
    ah = seq && n > 1u ? WAV_H - 12 : WAV_H;  /* the sequence below the lanes */
    for (k = 0; k < n; k++) {
        int32_t y0 = (int32_t)k * ah / (int32_t)n;
        lh = ((int32_t)k + 1) * ah / (int32_t)n - y0;
        draw_lane(ln[k], y0, k + 1u < n ? lh - 1 : lh, n == 1u);
        if (k + 1u < n) cv_rect(0, y0 + lh - 1, 240, 1, C_TAB);   /* between two lanes */
    }
    if (seq) {                               /* the sequence: 16 steps of the 64, the quarter being played or written */
        int32_t sp = seq_play, at = rec ? (int32_t)cursor : sp < 0 ? 0 : sp, q = at / 16, s;
        uint32_t slen = (uint32_t)par[P_SLEN] + 1u;
        for (s = 0; s < 16; s++) {
            uint32_t st = (uint32_t)(q * 16 + s), nt = seq_note[st];
            int32_t x0 = s * 15;
            uint16_t c = st >= slen ? C_AREA : nt >= 2u ? C_WAV : nt == 1u ? C_WAVD : C_TAB;
            if ((int32_t)st == sp) c = C_ACC;
            cv_rect(x0 + 1, WAV_H - 11, 13, 10, C_BLACK);
            if (nt == 1u && st < slen) cv_rect(x0, WAV_H - 7, 15, 3, c);     /* a tie: joined to the last */
            else cv_rect(x0 + 2, WAV_H - 10, 11, nt >= 2u ? 8 : 2, c);
            if (rec && (int32_t)st == (int32_t)cursor) cv_rect(x0 + 1, WAV_H - 1, 13, 1, C_WHITE);
            if (nt >= 2u && seq_pos[st] != 255u && st < slen) {   /* where the step takes its grains */
                uint32_t px = (uint32_t)seq_pos[st] * 239u / 200u;
                cv_rect((int32_t)px, 0, 1, 5, (int32_t)st == sp ? C_ACC : C_LAB);
            }
        }
        for (s = 1; s < 4; s++) cv_rect(s * 60 - 1, WAV_H - 12, 1, 12, C_LAB);
    }
    cv_blit(0, WAV_Y);
}

/* ---- the opening: Mario Texture's GRANA, a wheel of grana cheese with a wedge cut out. It appears whole and solid
 * (dense grains on a 2 px grid, each with its own shade), its crystals glint, then it comes apart into grains. All of
 * it lives in the sound's RAM (no sound is loaded yet): the screen, the grains, the two titles' masks. */
#define IN_DOTS 4800u
#define IN_END 3600u
#define W_CX 120
#define W_CY 80                              /* the top face's centre */
#define W_RX 74
#define W_RY 21
#define W_H 40                               /* the wheel's height */
#define T1_Y 2                               /* "Mario Texture's": its mask's top, 44 rows */
#define T1_H 44
#define T2_Y 150                             /* "GRANA": 74 rows */
#define T2_H 74
static uint32_t intro = 1, intro_t0, intro_last, in_n;
#define IN_BASE ((uint8_t *)smp_mem + 240u * 240u * 2u)
#define in_hx ((int16_t *)IN_BASE)
#define in_hy (in_hx + IN_DOTS)
#define in_sx (in_hy + IN_DOTS)
#define in_sy (in_sx + IN_DOTS)
#define in_dl ((uint16_t *)(in_sy + IN_DOTS))
#define in_rgb ((uint8_t *)(in_dl + IN_DOTS))      /* r g b a grain */
#define in_m1 (in_rgb + 3u * IN_DOTS)              /* the titles: 0 nothing, 1..15 the letter, 16 its outline */
#define in_m2 (in_m1 + 240u * T1_H)
typedef char in_fits[(240u * 240u * 2u + 13u * IN_DOTS + 240u * (T1_H + T2_H) <= SMP_BYTES + MOD_BYTES) ? 1 : -1];
static uint16_t *in_fb(void) { return (uint16_t *)smp_mem; }
static inline uint16_t in_col(uint32_t r, uint32_t g, uint32_t b, uint32_t a)   /* a 0..256, byte-swapped as cv_px */
{
    uint32_t c = RGB(r * a >> 8, g * a >> 8, b * a >> 8);
    return (uint16_t)((c >> 8) | (c << 8 & 0xFF00u));
}
static void in_dot(int32_t x, int32_t y, int32_t r, uint16_t c)
{
    int32_t i, j;
    uint16_t *fb = in_fb();
    for (j = -r; j <= r; j++) {
        int32_t yy = y + j;
        if ((uint32_t)yy >= 240u) continue;
        for (i = -r; i <= r; i++) {
            int32_t xx = x + i;
            if ((uint32_t)xx < 240u && i * i + j * j <= r * r + r) fb[yy * 240 + xx] = c;
        }
    }
}
static int32_t in_ease(int32_t u)            /* 0..256 -> 0..256, slow in and out */
{
    if (u <= 0) return 0;
    if (u >= 256) return 256;
    return u * u * (768 - 2 * u) >> 16;
}
static uint32_t in_hash(int32_t x, int32_t y) { uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u; h ^= h >> 13; h *= 0x5bd1e995u; return h ^ (h >> 15); }

/* the titles, drawn once into masks: thick (each letter stamped a few pixels across and down), slanted for the
 * first, a little wave for the second, and a dark outline around both */
static void in_mask(uint8_t *m, int32_t mh, const felucca_font_t *f, const char *str, int32_t sc, int32_t bx, int32_t by,
                    int32_t slant, int32_t wave, int32_t track)
{
    const char *p;
    int32_t w = 0, x, i, k, yo = 3;
    for (k = 0; k < 240 * mh; k++) m[k] = 0;
    for (p = str; *p; p++) w += (f->adv[glyph(f, (uint8_t)*p)] + track) * sc;
    x = 120 - (w - track * sc + bx) / 2;
    for (i = 0; *str; str++, i++) {
        uint32_t gi = glyph(f, (uint8_t)*str), gx, gy, gw = f->bw[gi], bpr = (gw + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        int32_t dy = wave ? (lfo_sin((uint32_t)i * 0x30000000u) * wave) >> 15 : 0;
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < gw; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                int32_t px, py, a, b2;
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v < 4u) continue;
                px = x + (-(int32_t)f->pad + (int32_t)gx) * sc + ((int32_t)f->h - (int32_t)gy) * slant / 8;
                py = yo + (int32_t)gy * sc + dy;
                for (b2 = 0; b2 < sc + by; b2++)
                    for (a = 0; a < sc + bx; a++) {
                        int32_t xx = px + a, yy = py + b2;
                        if ((uint32_t)xx < 240u && (uint32_t)yy < (uint32_t)mh && m[yy * 240 + xx] < v) m[yy * 240 + xx] = (uint8_t)v;
                    }
            }
        x += (f->adv[gi] + track) * sc;
    }
    for (k = 0; k < 240 * mh; k++) {         /* the outline: 2 px round the letters */
        int32_t yy = k / 240, xx = k % 240, j, q;
        if (m[k]) continue;
        for (j = -2; j <= 2 && !m[k]; j++)
            for (q = -2; q <= 2; q++) {
                int32_t y2 = yy + j, x2 = xx + q;
                if ((uint32_t)y2 < (uint32_t)mh && (uint32_t)x2 < 240u && m[y2 * 240 + x2] >= 1u && m[y2 * 240 + x2] <= 15u &&
                    j * j + q * q <= 5) { m[k] = 16u; break; }
            }
    }
}
/* the titles onto the screen: a shadow, the outline, the letters with their colours top to bottom and a crumb */
static void in_title(const uint8_t *m, int32_t y0, int32_t mh, uint32_t a, const uint8_t (*grad)[3])
{
    uint16_t *fb = in_fb();
    int32_t k;
    for (k = 0; k < 240 * mh; k++) {         /* the shadow, 3 px down and right, a quarter as bright; 1.7: a softer
                                              * half-shade one pixel further */
        int32_t yy = k / 240, xx = k % 240, sy = y0 + yy + 3, sx = xx + 3;
        if (m[k] && (uint32_t)sy < 240u && sx < 240 && a > 128u) {
            uint16_t c = fb[sy * 240 + sx];
            uint16_t u = (uint16_t)((c >> 8) | (c << 8)), d = (uint16_t)((u >> 2) & 0x39E7u);
            fb[sy * 240 + sx] = (uint16_t)((d >> 8) | (d << 8));
            if (sy + 1 < 240 && sx + 1 < 240 && !m[k + 241 < 240 * mh ? k + 241 : k]) {
                uint16_t c2 = fb[(sy + 1) * 240 + sx + 1], u2 = (uint16_t)((c2 >> 8) | (c2 << 8)), d2 = (uint16_t)((u2 >> 1) & 0x7BEFu);
                fb[(sy + 1) * 240 + sx + 1] = (uint16_t)((d2 >> 8) | (d2 << 8));
            }
        }
    }
    for (k = 0; k < 240 * mh; k++) {
        int32_t yy = k / 240, xx = k % 240, sy = y0 + yy, t, r, g, b;
        uint32_t v = m[k], hsh;
        if (!v || (uint32_t)sy >= 240u) continue;
        if (v == 16u) { fb[sy * 240 + xx] = in_col(46, 22, 6, a); continue; }
        t = yy * 256 / mh;                   /* the gradient: three colours down the letters */
        if (t < 128) { r = grad[0][0] + (grad[1][0] - grad[0][0]) * t / 128; g = grad[0][1] + (grad[1][1] - grad[0][1]) * t / 128; b = grad[0][2] + (grad[1][2] - grad[0][2]) * t / 128; }
        else { t -= 128; r = grad[1][0] + (grad[2][0] - grad[1][0]) * t / 128; g = grad[1][1] + (grad[2][1] - grad[1][1]) * t / 128; b = grad[1][2] + (grad[2][2] - grad[1][2]) * t / 128; }
        hsh = in_hash(xx, yy);
        if ((hsh & 15u) == 0u) { r = r * 3 / 4; g = g * 3 / 4; b = b * 3 / 4; }          /* crumbs */
        else if ((hsh & 31u) == 1u) { r = 255; g = 252; b = 236; }                          /* crystals */
        fb[sy * 240 + xx] = in_col((uint32_t)r, (uint32_t)g, (uint32_t)b, a * (v * 17u) >> 8);
    }
}
/* the wedge: from angle 290 to 340 degrees (front right), as phases of a turn */
#define WEDGE_A 0xCE38E38Eu                  /* 290 / 360 x 2^32 */
#define WEDGE_B 0xF1C71C71u                  /* 340 / 360 x 2^32 */
static int in_wedge(uint32_t a) { return a >= WEDGE_A && a < WEDGE_B; }
static void in_add(int32_t x, int32_t y, int32_t r, int32_t g, int32_t b)
{
    uint32_t n = in_n;
    if (n >= IN_DOTS) return;
    in_hx[n] = (int16_t)x; in_hy[n] = (int16_t)y;
    in_rgb[3u * n] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    in_rgb[3u * n + 1u] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    in_rgb[3u * n + 2u] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    in_n = n + 1u;
}
static uint32_t in_angle(int32_t x, int32_t y)   /* the angle of a point of the top face (y down, squashed) */
{
    int32_t yy = -y * W_RX / W_RY, best = 0x7FFFFFFF;
    uint32_t t, bt = 0;
    for (t = 0; t < 128u; t++) {
        uint32_t ph = t << 25;
        int32_t dx = lfo_sin(ph + 0x40000000u) >> 9, dy = lfo_sin(ph) >> 9, cr = dx * yy - dy * x, dot = dx * x + dy * yy;
        if (cr < 0) cr = -cr;
        if (dot > 0 && cr < best) { best = cr; bt = ph; }
    }
    return bt;
}
/* the wheel (slice 0) or the wedge out of it (slice 1), solid: the rind, the top, the two cut faces */
static void in_wheel(int slice, int32_t ox, int32_t oy)
{
    int32_t x, y, h, k;
    for (x = -W_RX; x <= W_RX; x += 2) {     /* the rind: the front of the side, round in the light from the left */
        int32_t yf = (int32_t)isqrt((uint32_t)(W_RX * W_RX - x * x)) * W_RY / W_RX, lit;
        if (in_wedge(in_angle(x, yf)) != slice) continue;
        lit = 256 - (x * x * 150) / (W_RX * W_RX) - x * 40 / W_RX;
        for (h = 0; h <= W_H; h += 2) {
            int32_t l = lit - (h > W_H - 6 ? 50 : 0) + (h < 4 ? 30 : 0);
            uint32_t hs = in_hash(x, h);
            if ((h % 10) == 4 && (x & 3) == 0 && h > 6 && h < W_H - 6) l -= 70;   /* the dotted brand on the rind */
            l += (int32_t)(hs & 15u) - 8;
            in_add(ox + W_CX + x, oy + W_CY + yf + h, 200 * l >> 8, 128 * l >> 8, 44 * l >> 8);
        }
    }
    for (y = -W_RY; y <= W_RY; y += 2)       /* the top: straw, its rim darker, flecked */
        for (x = -W_RX; x <= W_RX; x += 2) {
            int32_t e = x * x * W_RY * W_RY + y * y * W_RX * W_RX, lim = W_RX * W_RX * W_RY * W_RY, l;
            uint32_t hs;
            if (e > lim || in_wedge(in_angle(x, y)) != slice || (slice && x * x + y * y < 16)) continue;
            hs = in_hash(x + 1000, y);
            l = e > lim - lim / 6 ? 205 : 256 - (x + W_RX) * 20 / (2 * W_RX);
            l += (int32_t)(hs & 15u) - 8;
            if ((hs >> 8) % 41u == 0u) in_add(ox + W_CX + x, oy + W_CY + y, 255, 255, 236);   /* a crystal */
            else in_add(ox + W_CX + x, oy + W_CY + y, 240 * l >> 8, 214 * l >> 8, 138 * l >> 8);
        }
    for (k = 0; k < 2; k++) {                /* the cut faces: from the centre to the rim, down the height; the wedge
                                              * shows only the one turned to us, the wheel's are in the gap's shade */
        uint32_t a = k ? WEDGE_B : WEDGE_A;
        int32_t rx = lfo_sin(a + 0x40000000u) * W_RX >> 15, ry = -(lfo_sin(a) * W_RY >> 15), n, i;
        int32_t sh = slice ? 262 : (k ? 205 : 180);
        if (slice && k) continue;
        n = (rx < 0 ? -rx : rx) > (ry < 0 ? -ry : ry) ? (rx < 0 ? -rx : rx) : (ry < 0 ? -ry : ry);
        n /= 2;
        if (n < 1) n = 1;
        for (i = 0; i <= n; i++)
            for (h = 0; h <= W_H; h += 2) {
                uint32_t hs = in_hash(i * 7 + k, h);
                int32_t px = ox + W_CX + rx * i / n, py = oy + W_CY + ry * i / n + h, l = sh + (int32_t)(hs & 15u) - 8;
                if ((hs >> 8) % 17u == 0u) in_add(px, py, 255, 255, 240);
                else in_add(px, py, 250 * l >> 8, 234 * l >> 8, 184 * l >> 8);
            }
    }
}
static void intro_start(void)
{
    static const char *const T2 = "GRANA";
    uint32_t k;
    intro_t0 = fm1_ms;
    in_n = 0;
    in_wheel(0, -6, -4);                     /* the wheel, a little up and left to make room */
    in_wheel(1, 30, 15);                     /* the wedge, pulled out */
    for (k = 0; k < in_n; k++) {             /* where each grain goes, and when */
        uint32_t a = xrand_main();
        int32_t r = 150 + (int32_t)(xrand_main() % 70u);
        in_sx[k] = (int16_t)(120 + (lfo_sin(a + 0x40000000u) * r >> 15));
        in_sy[k] = (int16_t)(100 + (lfo_sin(a) * r >> 15));
        in_dl[k] = (uint16_t)((in_n - 1u - k) * 300u / in_n + xrand_main() % 160u);   /* the wedge (added last) leaves first */
    }
    in_mask(in_m1, T1_H, &FONT_L, "Mario Texture's", 1, 1, 0, 2, 0, -1);   /* 1.7: lighter, less slanted, a little apart */
    in_mask(in_m2, T2_H, &FONT_L, T2, 2, 2, 1, 0, 2, 2);                  /* less heavy, the wave gentler */
}
static void intro_frame(void)                /* the wheel fades in whole, glints, then comes apart into grains */
{
    static const uint8_t G1[3][3] = {{255, 246, 214}, {255, 206, 110}, {226, 140, 46}};    /* cream, gold, amber */
    static const uint8_t G2[3][3] = {{255, 250, 226}, {255, 196, 84}, {176, 98, 26}};
    uint16_t *fb = in_fb();
    int32_t t = (int32_t)(fm1_ms - intro_t0), k, fin = t < 700 ? t * 256 / 700 : 256, fout = t > 3200 ? 256 - (t - 3200) * 256 / 400 : 256;
    lcd_sync();
    for (k = 0; k < 240 * 240; k++) fb[k] = 0;
    if (fout < 0) fout = 0;
    {   /* 1.7: the wheel's shadow on the ground, soft, gone as the grains leave */
        int32_t sa = fin * (t < 1800 ? 256 : t < 2600 ? 256 - (t - 1800) * 256 / 800 : 0) >> 8, yy, xx;
        for (yy = -12; yy <= 12 && sa > 0; yy++)
            for (xx = -96; xx <= 96; xx++) {
                int32_t e = xx * xx * 144 + yy * yy * 9216, lim = 96 * 96 * 144;   /* an ellipse 96 x 12 */
                int32_t px = W_CX + 6 + xx, py = W_CY + W_H + 12 + yy, l;
                if (e >= lim || (uint32_t)px >= 240u || (uint32_t)py >= 240u) continue;
                l = (256 - e * 256 / lim) * sa >> 8;
                fb[py * 240 + px] = in_col(90, 52, 18, (uint32_t)l);
            }
    }
    for (k = 0; k < (int32_t)in_n; k++) {
        /* 1.8 s .. 3.4 s: each grain leaves at its own moment (the wedge first), drifts out and fades */
        int32_t u = in_ease((t - 1800 - (int32_t)in_dl[k] * 2) * 256 / 900), px, py, a;
        const uint8_t *c = in_rgb + 3 * k;
        px = in_hx[k] + ((in_sx[k] - in_hx[k]) * u >> 9);
        py = in_hy[k] + ((in_sy[k] - in_hy[k]) * u >> 9) + (u * u >> 12);   /* a little gravity */
        a = fin * (256 - u * 3 / 4) >> 8;
        if (u == 0 && t > 800 && c[2] >= 236u && ((uint32_t)(t >> 6) + (uint32_t)k * 7u) % 19u == 0u) {   /* the crystals glint:
                                              * 1.7, a little four-pointed star */
            int32_t r2;
            in_dot(px, py, 1, in_col(255, 255, 255, (uint32_t)fin));
            for (r2 = 2; r2 <= 4; r2++) {
                uint16_t cc = in_col(255, 250, 220, (uint32_t)fin * (5 - r2) / 4);
                if ((uint32_t)(px + r2) < 240u) fb[py * 240 + px + r2] = cc;
                if (px - r2 >= 0) fb[py * 240 + px - r2] = cc;
                if ((uint32_t)(py + r2) < 240u) fb[(py + r2) * 240 + px] = cc;
                if (py - r2 >= 0) fb[(py - r2) * 240 + px] = cc;
            }
            continue;
        }
        in_dot(px, py, u < 40 ? 1 : u < 140 ? 1 : 0, in_col(c[0], c[1], c[2], (uint32_t)a));
    }
    {
        int32_t a = t < 300 ? 0 : t < 1000 ? (t - 300) * 256 / 700 : 256;
        a = a * fout >> 8;
        in_title(in_m1, T1_Y, T1_H, (uint32_t)a, G1);
        in_title(in_m2, T2_Y, T2_H, (uint32_t)a, G2);
        {   /* the version, small, and the flash chip (1.7.1) */
            char vb[32];
            const char *v = vb;
#ifdef FL_JEDEC_OK
            flash_tag(cat(vb, "v" APP_VER "  "));
#else
            cat(vb, "v" APP_VER);
#endif
            int32_t w = 0, x;
            const char *p;
            for (p = v; *p; p++) w += FONT_S.adv[glyph(&FONT_S, (uint8_t)*p)];
            x = 234 - w;
            for (; *v; v++) {
                uint32_t gi = glyph(&FONT_S, (uint8_t)*v), gx, gy, gw = FONT_S.bw[gi], bpr = (gw + 1u) / 2u;
                const uint8_t *gd = FONT_S.data + FONT_S.off[gi];
                for (gy = 0; gy < FONT_S.h; gy++)
                    for (gx = 0; gx < gw; gx++) {
                        uint32_t vv = gd[gy * bpr + gx / 2u];
                        int32_t px = x - (int32_t)FONT_S.pad + (int32_t)gx, py = 222 + (int32_t)gy;
                        vv = (gx & 1u) ? (vv & 15u) : (vv >> 4);
                        if (vv && (uint32_t)px < 240u && (uint32_t)py < 240u) fb[py * 240 + px] = in_col(170, 150, 120, (uint32_t)a * vv * 17u >> 9);
                    }
                x += FONT_S.adv[gi];
            }
        }
    }
    lcd_blit(0, 0, 240, 240, fb);
}
static void intro_finish(void);

/* ---- the panel */
static uint32_t t_posmove;                  /* the last move of POSIZIONE, AREA, SPRAY: windows follow after */
static void param_change(uint32_t p, int32_t d)
{
    int32_t v = (int32_t)par[p] + d, mx = (int32_t)PMAX[p];
    if (p == P_FMODE) {                     /* the filter's modes in the knob's order */
        v = (int32_t)fm_pos(par[p]) + d;
        v = FM_ORDER[v < 0 ? 0 : v > 6 ? 6 : v];
    }
    if (p == P_SND) mx = (int32_t)nsounds() - 1;
    if (p >= P_SND2 && p <= P_SND4) mx = (int32_t)nsnd;
    if (v < 0) v = 0;
    if (v > mx) v = mx;
    if ((uint32_t)v == par[p]) return;
    par[p] = base[p] = (uint8_t)v;          /* a knob moves the value under the dice too */
    edited = 1;
    dirty_hdr = 1;
    if (p >= P_SND && p <= P_SND4) layers_sync(0);
    if (p == P_POS || p == P_AREA || p == P_SPRAY || (p >= P_LS1 && p <= P_LA4)) t_posmove = fm1_ms;
    if (p == P_DADO && !v && dice_on) {     /* the dice off: back to the knobs' values */
        static const uint8_t DP[8] = {P_POS, P_AREA, P_SCAN, P_SIZE, P_DENS, P_SPRAY, P_PITCH, P_JIT};
        uint32_t k;
        for (k = 0; k < 8u; k++) par[DP[k]] = base[DP[k]];
        dice_on = 0;
        if (page < 2u || page == PG_MOTO2) dirty_cells = 0xF;
    }
    engine_update();
    if (p == P_DRONE) drone_update();
    if (p < NPAR && cell_of(page, p) >= 0) dirty_cells |= 1u << cell_of(page, p);
    if (p == P_MTYPE || p == P_NMODE) dirty_cells = 0xF;     /* the other knobs' names follow the type and the mode */
}
static void param_set(uint32_t p, uint32_t pos)   /* a position on the parameter's scale (MIDI): the filter's in its order */
{
    if (p == P_FMODE) param_change(p, (int32_t)pos - (int32_t)fm_pos(par[p]));
    else param_change(p, (int32_t)pos - (int32_t)par[p]);
}
static int32_t preset_accel(int32_t d) { return (d > 2 || d < -2) ? 2 * d : d; }   /* gentle: every preset can be reached */
static int32_t accel(int32_t d, uint32_t range)
{
    int32_t a = d < 0 ? -d : d;
    if (range > 60u && a > 1) d *= a > 4 ? 4 : a;   /* fast turns go further */
    return d;
}

/* the dice: GRANI and MOTO move around the knobs' values, as far as DADO says */
static void dice_throw(void)
{
    uint32_t k, amt = par[P_DADO];
    if (!amt) return;
    static const uint8_t DP[8] = {P_POS, P_AREA, P_SCAN, P_SIZE, P_DENS, P_SPRAY, P_PITCH, P_JIT};   /* MOTO, GRANI, JITTER */
    uint32_t j;
    for (j = 0; j < 8u; j++) {
        int32_t r, v;
        k = DP[j];
        r = (int32_t)(PMAX[k] * amt / 100u);
        if (k == P_POS || k == P_SCAN) r = r / 2;           /* these two jump less: the cloud stays recognisable */
        v = (int32_t)base[k] + (r ? (int32_t)(xrand_main() % (uint32_t)(2 * r + 1)) - r : 0);
        if (v < 0) v = 0;
        if (v > (int32_t)PMAX[k]) v = (int32_t)PMAX[k];
        par[k] = (uint8_t)v;
    }
    dice_on = 1;
    engine_update();
    if (page < 2u || page == PG_MOTO2) dirty_cells = 0xF;
}

static uint32_t prev_keys, t_save;
static uint32_t split_arm, t_split;         /* 1.5: the split point a key will set (SPLIT page), when */
static void learn_say(const char *a, const char *b);
static void split_key(uint32_t note)
{
    char t[8], *e;
    param_set(split_arm, note);
    note_name(t, note);
    learn_say(PNAME[split_arm], " = ");
    e = lmsg; while (*e) e++;
    cat(e, t);
    split_arm = 0;
    dirty_cells = 0xF;
}
static uint8_t held[27];

static void app_setup(void)
{
    uint32_t k;
    presets_load();
    reverb_init();
    for (k = 0; k < NP; k++) par[k] = base[k] = pr[cur_preset].p[k];
    seq_recall(cur_preset);
    t_cur = 0;
    bank_open();
    if (par[P_SND] >= nsounds()) par[P_SND] = base[P_SND] = 0;
    engine_update();
    intro_start();                          /* the sound waits: the opening draws in its RAM */
}
static void intro_finish(void)
{
    uint32_t k;
    intro = 0;
    fm1_led_bg_ns = 5000;                   /* every button and key faintly lit: the labels read in the dark */
    for (k = 0; k < 41u; k++) fm1_led_back(k, 1);
    lcd_sync();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    { uint32_t k2; for (k2 = 0; k2 < MOD_BYTES / 2u; k2++) mod_line[k2] = 0; }   /* the opening drew there */
    layers_sync(1);
    drone_update();
    dirty_hdr = dirty_tabs = 1;
    dirty_cells = 0xF;
}

static uint32_t t_flash;                    /* SELECT turned: the page's name, big, for a moment */
static void goto_page(uint32_t p) { page = p; dirty_tabs = 1; dirty_cells = 0xF; t_flash = 0; }
static void draw_flash(void)
{
    uint32_t i, n = sizeof PG_ORDER;
    int32_t at = 0, w;
    for (i = 0; i < n; i++) if (PG_ORDER[i] == page) at = (int32_t)i;
    lcd_fill(0, 140, 240, 6, C_BLACK);
    cv_begin(240, 88, C_BLACK);
    if (page == PG_MOT) {                   /* 1.8, ALGORITHM: the engine, big, and its neighbours */
        uint32_t m = par[P_MOTORE] & 3u;
        w = text_w(&FONT_L, MOT_NAME[m]);
        cv_text(120 - w / 2, 22, &FONT_L, MOT_NAME[m], C_ACC);
        cv_rect(120 - w / 2, 56, w, 2, C_RULE);
        if (m) cv_text(6, 66, &FONT_S, MOT_NAME[m - 1u], C_LAB);
        if (m + 1u < NMOT) { w = text_w(&FONT_S, MOT_NAME[m + 1u]); cv_text(234 - w, 66, &FONT_S, MOT_NAME[m + 1u], C_LAB); }
        w = text_w(&FONT_S, "MOTORE");
        cv_text(120 - w / 2, 2, &FONT_S, "MOTORE", C_LAB);
    } else {
    w = text_w(&FONT_L, PAGE[page]);
    cv_text(120 - w / 2, 22, &FONT_L, PAGE[page], C_ACC);
    cv_rect(120 - w / 2, 56, w, 2, C_RULE);   /* 1.7: underlined */
    cv_text(6, 66, &FONT_S, PAGE[PG_ORDER[(at + (int32_t)n - 1) % (int32_t)n]], C_LAB);   /* the one before, the one after */
    w = text_w(&FONT_S, PAGE[PG_ORDER[(at + 1) % (int32_t)n]]);
    cv_text(234 - w, 66, &FONT_S, PAGE[PG_ORDER[(at + 1) % (int32_t)n]], C_LAB);
    }
    cv_blit(0, 146);
    lcd_fill(0, 234, 240, 6, C_BLACK);
}

static void step_write(uint32_t note)       /* REC: the step under the cursor, then the next */
{
    uint32_t slen = (uint32_t)par[P_SLEN] + 1u;
    seq_pos[cursor] = note >= 2u ? par[P_POS] : 255u;
    seq_note[cursor] = (uint8_t)note;
    cursor = (cursor + 1u) % slen;
    edited = 1;
    dirty_hdr = 1;
}

static void hold_toggle(uint32_t keys)
{
    uint32_t k;
    hold = !hold;
    if (!hold) {
        uint32_t res = reserved();
        if (clk.arp) {                      /* the arpeggio keeps only the keys still down */
            arp_cnt = 0;
            for (k = 0; k < 27u; k++) if (keys >> k & 1u) { arp_list[arp_cnt++] = held[k]; if (arp_cnt >= 16u) break; }
            arp_publish();
        }
        for (k = 0; k < NV; k++) {          /* let go of what is not held by a finger */
            uint32_t kk, pressed = 0;
            if (res >> k & 1u) continue;
            for (kk = 0; kk < 27u; kk++) if ((keys >> kk & 1u) && held[kk] == voice[k].note) pressed = 1;
            if (!pressed) voice[k].gate = 0;
        }
    }
}

/* SAVE: the first press asks where (PRESETS turns the slot, nothing is recalled), the second keeps it there */
/* the name being written while SAVE asks where: KNOB 1 the cursor, KNOB 2 the letter, KNOB 3 a name at random,
 * KNOB 4 deletes. An empty place gets the sound's name and a number not yet taken ("VOCI 2"). */
static char save_name[13];
static uint32_t name_cur, name_touched, name_dirty;
static const char NAME_CH[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-'";
static const char *const NAME_DICE[40] = {"NEBBIA", "POLVERE", "VETRI", "MAREA", "SCIAME", "BRINA", "CENERE", "ECO",
    "RISACCA", "FRUSCIO", "NUVOLA", "SABBIA", "SPECCHI", "GHIACCIO", "LUCCIOLE", "VENTO", "CORALLO", "FUMO", "ORBITA",
    "LANTERNA", "ABISSO", "ALBA", "CREPUSCOLO", "FILO", "TRAMA", "VELLUTO", "ROVO", "MUSCHIO", "SALE", "TEMPESTA",
    "BRUSIO", "RONZIO", "POLLINE", "SCINTILLE", "AURORA", "RUGGINE", "CRISTALLI", "MIELE", "LAGUNA", "DERIVA"};
static uint32_t name_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static uint32_t name_len(void) { uint32_t n = 0; while (n < 12u && save_name[n]) n++; return n; }
static void name_auto(void)                 /* the first layer's sound and the first free number */
{
    uint32_t n, i, k;
    const char *b = snd_name(par[P_SND]);
    for (n = 2; n < 100u; n++) {
        char t[13];
        for (k = 0; k < 9u && b[k]; k++) t[k] = b[k];
        while (k && t[k - 1] == ' ') k--;
        t[k++] = ' ';
        if (n >= 10u) t[k++] = (char)('0' + n / 10u);
        t[k++] = (char)('0' + n % 10u);
        t[k] = 0;
        for (i = 0; i < NPRESET; i++) if (pr[i].used && name_eq(preset_name(i), t)) break;
        if (i == NPRESET) { for (k = 0; k < 13u; k++) save_name[k] = t[k]; return; }
    }
}
static void name_start(void)                /* SAVE pressed, or the place changed and the name was not touched */
{
    uint32_t k;
    if (pr[save_dest].used) {
        const char *n = preset_name(save_dest);
        for (k = 0; k < 12u && n[k]; k++) save_name[k] = n[k];
        save_name[k] = 0;
    } else name_auto();
    name_cur = name_len();
    if (name_cur > 11u) name_cur = 11u;
    name_touched = 0;
    name_dirty = 1;
}
static void name_knob(uint32_t k, int32_t d)
{
    uint32_t n = name_len(), i;
    name_touched = 1;
    name_dirty = 1;
    t_savemode = fm1_ms;
    if (k == 0u) {                          /* the cursor: to one past the end */
        int32_t c = (int32_t)name_cur + (d > 0 ? 1 : -1), mx = (int32_t)(n < 12u ? n : 11u);
        name_cur = (uint32_t)(c < 0 ? 0 : c > mx ? mx : c);
    } else if (k == 1u) {                   /* the letter under it */
        int32_t at = 0, nc = (int32_t)sizeof NAME_CH - 1;
        char ch = name_cur < n ? save_name[name_cur] : ' ';
        for (i = 0; i < (uint32_t)nc; i++) if (NAME_CH[i] == ch) at = (int32_t)i;
        at = (at + d % nc + nc) % nc;
        save_name[name_cur] = NAME_CH[at];
        if (name_cur >= n) save_name[name_cur + 1u] = 0;
        n = name_len();
        while (n && save_name[n - 1u] == ' ' && n - 1u != name_cur) save_name[--n] = 0;
    } else if (k == 2u) {                   /* a name from the dice */
        const char *w = NAME_DICE[xrand_main() % 40u];
        for (i = 0; i < 12u && w[i]; i++) save_name[i] = w[i];
        save_name[i] = 0;
        name_cur = i < 12u ? i : 11u;
    } else if (name_cur < n) {              /* delete the letter under the cursor */
        for (i = name_cur; i < n; i++) save_name[i] = save_name[i + 1u];
    }
}
static void draw_name(void)                 /* the name editor, where the knobs were */
{
    static const char *const HINT[4] = {"CURSORE", "LETTERA", "DADO", "CANC."};
    const felucca_font_t *f = &FONT_L;
    uint32_t i, n = name_len();
    int32_t x0, cw, w;
    char one[2] = {0, 0};
    lcd_fill(0, 140, 240, 6, C_BLACK);
    cv_begin(240, 88, C_BLACK);
    cv_text(6, 0, &FONT_S, "NOME", C_LAB);
    if (text_w(f, "MMMMMMMMMMMM") > 236) f = &FONT_S;
    cw = text_w(f, "M");
    w = cw * (int32_t)(n > name_cur ? n : name_cur + 1u);
    x0 = 120 - w / 2;
    if (x0 < 2) x0 = 2;
    for (i = 0; i < 12u; i++) {             /* a fixed width a letter, the cursor under its own */
        one[0] = i < n ? save_name[i] : ' ';
        if (i < n) cv_text(x0 + (int32_t)i * cw, 20, f, one, C_WHITE);
        if (i == name_cur) cv_rect(x0 + (int32_t)i * cw, 20 + (f == &FONT_L ? 34 : 18), cw - 2, 3, C_ACC);
    }
    for (i = 0; i < 4u; i++) cv_text(6 + (int32_t)i * 59, 70, &FONT_S, HINT[i], C_TAB);
    cv_blit(0, 146);
    lcd_fill(0, 234, 240, 6, C_BLACK);
}
static void save_to(uint32_t dst)
{
    uint32_t k;
    dst %= NPRESET;
    for (k = 0; k < 12u; k++) pr[dst].name[k] = save_name[k];   /* as written (an empty name: the sound's) */
    pr[dst].name[11] = 0;
    { uint32_t n = 11u; while (n && (pr[dst].name[n - 1u] == ' ' || !pr[dst].name[n - 1u])) pr[dst].name[--n] = 0; }
    for (k = 0; k < NP; k++) pr[dst].p[k] = base[k];
    for (k = 0; k < NSTEP; k++) { pr[dst].sn[k] = seq_note[k]; pr[dst].sp[k] = seq_pos[k]; }
    pr[dst].used = 1;
    save_sectors |= 1u << (dst / 16u);
    cur_preset = dst;
    t_cur = fm1_ms;
    edited = 0;
    save_pending = 1;
    t_save = fm1_ms;
}


/* ---- MIDI (1.2): USB and the TRS jack arrive together. The clock: in (ESTERNO: ARP, SEQ and the LFOs in time, START and
 * STOP play the sequence) and out (24 a quarter note to USB). MIDI LEARN: a CC, a note or the pitch bend of any controller
 * on any parameter; turn LEARN on (MIDI page, KNOB 3), then move the controller and a knob of the FM-1, in either order. */
static uint32_t ext_last_ms, ext_cnt, ext_t0;
static const char *const MIDI_PN[4] = {"CLOCK", "RICEVI", "LEARN", "CANCELLA"};   /* 1.5: CLOCK IN and OUT on one knob, RICEVI */

static char *msg_text(char *e, uint32_t st, uint32_t num)   /* "CC 21 CH1", "NOTA C3 CH10", "BEND CH2" */
{
    uint32_t hi = st & 0xF0u;
    if (hi == 0xB0u) { e = cat(e, "CC "); fmt_u(e, num); while (*e) e++; }
    else if (hi == 0x90u) { e = cat(e, "NOTA "); note_name(e, num); while (*e) e++; }
    else e = cat(e, "BEND");
    e = cat(e, " CH"); fmt_u(e, (st & 15u) + 1u); while (*e) e++;
    return e;
}
static void settings_touch(void) { set_dirty = 1; t_set = fm1_ms; dirty_tabs = 1; }
static void learn_say(const char *a, const char *b)
{
    char *e = cat(lmsg, a);
    if (b) cat(e, b);
    t_lmsg = fm1_ms ? fm1_ms : 1u;
    dirty_tabs = 1;
}
static void learn_status(void)              /* while LEARN waits: what it has, what it still needs */
{
    char *e = lmsg;
    if (!learn_on) return;
    e = cat(e, "LEARN: ");
    if (learn_have) e = msg_text(e, learn_st, learn_num); else e = cat(e, "?");
    e = cat(e, " > ");
    cat(e, learn_par >= 0 ? pname_of((uint32_t)learn_par) : "?");
    t_lmsg = 0;
    dirty_tabs = 1;
}
static void learn_start(int on)
{
    learn_on = on ? 1u : 0u;
    learn_have = 0;
    learn_par = -1;
    t_learn = fm1_ms;
    if (on) learn_status(); else { lmsg[0] = 0; dirty_tabs = 1; }
}
static void learn_try(void)                 /* both halves: kept (the same message again takes the new parameter) */
{
    uint32_t i;
    char *e;
    if (!learn_have || learn_par < 0) { learn_status(); return; }
    for (i = 0; i < store.nlearn; i++) if (store.lr[i].st == learn_st && store.lr[i].num == learn_num) break;
    if (i == store.nlearn) {
        if (store.nlearn >= NLEARN) { learn_on = 0; learn_say("LEARN: PIENO (32)", 0); return; }
        store.nlearn++;
    }
    store.lr[i].st = (uint8_t)learn_st; store.lr[i].num = (uint8_t)learn_num; store.lr[i].par = (uint8_t)learn_par; store.lr[i].pad = 0;
    settings_touch();
    learn_on = 0;
    e = msg_text(cat(lmsg, "OK: "), learn_st, learn_num);
    e = cat(e, " > ");
    cat(e, pname_of((uint32_t)learn_par));
    t_lmsg = fm1_ms ? fm1_ms : 1u;
    dirty_tabs = 1;
    if (page == PG_MIDI) dirty_cells = 0xF;
}
static uint32_t par_top(uint32_t p)         /* the highest value a parameter takes */
{
    if (p == P_SND) return nsounds() - 1u;
    if (p >= P_SND2 && p <= P_SND4) return nsnd;
    return PMAX[p];
}
static void note_button(uint32_t p)         /* a learnt note: a choice steps on, a level jumps between its ends */
{
    uint32_t top = par_top(p), pos = p == P_FMODE ? fm_pos(par[p]) : par[p];
    if (top <= 7u) param_set(p, pos >= top ? 0u : pos + 1u);
    else param_set(p, pos < top / 2u ? top : 0u);
}
static int learnt(uint32_t st, uint32_t d1, uint32_t d2)   /* 1: a message LEARN knows, done */
{
    uint32_t i, hi = st & 0xF0u, key = hi == 0x80u ? 0x90u | (st & 15u) : st;
    for (i = 0; i < store.nlearn; i++) {
        const learn_t *l = &store.lr[i];
        if (l->st != key || l->par >= NP) continue;
        if (hi == 0xE0u) { param_set(l->par, (d1 | d2 << 7) * par_top(l->par) / 16383u); return 1; }
        if (l->num != d1) continue;
        if (hi == 0xB0u) param_set(l->par, d2 * par_top(l->par) / 127u);
        else if (hi == 0x90u && d2) note_button(l->par);
        return 1;                           /* its note off too: the note no longer plays */
    }
    return 0;
}
static void clock_mode(uint32_t ext)
{
    ext_ticks = clk_int;                    /* in step from here */
    clk.ext = ext;
    store.clk_in = (uint8_t)ext;
    engine_update();
    settings_touch();
}
static void midi_in(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t hi = st & 0xF0u;
    if (st >= 0xF8u) {                      /* the clock and the transport */
        if (!clk.ext) return;
        if (st == 0xF8u) {
            uint32_t dt = fm1_ms - ext_last_ms;
            ext_last_ms = fm1_ms;
            ext_ticks += 4u;
            if (dt > 250u) { ext_cnt = 0; ext_t0 = fm1_ms; }   /* a new start: the tempo measured again */
            else if (++ext_cnt >= 24u) {     /* a quarter note: the tempo, smoothed */
                uint32_t el = fm1_ms - ext_t0, b = el ? 600000u / el : ext_bpm10;
                if (b < 200u) b = 200u;
                if (b > 3000u) b = 3000u;
                ext_bpm10 = (ext_bpm10 * 3u + b) / 4u;
                if (b > ext_bpm10 + 50u || b + 50u < ext_bpm10) ext_bpm10 = b;   /* a jump: follow at once */
                ext_cnt = 0; ext_t0 = fm1_ms;
                engine_update();
                if (page == PG_SEQ || page == PG_MIDI) dirty_cells = 0xF;
            }
        } else if (st == 0xFAu) { ext_ticks = clk_int; clk.restart = 1; clk.run = 1; lfo_restart(); }   /* START */
        else if (st == 0xFBu) clk.run = 1;                                                           /* CONTINUE */
        else if (st == 0xFCu) clk.run = 0;                                                           /* STOP */
        return;
    }
    if (learn_on && ((hi == 0xB0u && d1 < 120u) || (hi == 0x90u && d2) || hi == 0xE0u)) {   /* LEARN: the controller's half */
        learn_st = st; learn_num = hi == 0xE0u ? 0u : d1; learn_have = 1;
        learn_try();
        return;
    }
    if (learn_on && (hi == 0x80u || hi == 0x90u)) return;
    if (learnt(st, d1, d2)) return;
    if (store.rx == 1u) return;             /* RICEVI SOLO CLOCK: notes, CCs, programs left alone (LEARN, SMC still work) */
    if (store.rx >= 2u && (st & 15u) != store.rx - 2u) return;   /* RICEVI CANALE n: that channel only */
    if (hi == 0x90u && d2) { note_on(d1, d2); if (rec) step_write(d1); }
    else if (hi == 0x80u || hi == 0x90u) note_off(d1);
    else if (hi == 0xB0u && ((d1 >= 20u && d1 < 60u) || (d1 >= 70u && d1 < 90u) || (d1 >= 102u && d1 < 115u))) {
        uint32_t p = d1 < 60u ? d1 - 20u : d1 < 90u ? d1 - 70u + P_SND : d1 < 110u ? d1 - 102u + P_MTYPE : d1 - 110u + P_MOTORE;   /* 1.8: 110..114 MOTORE */
        param_set(p, d2 * PMAX[p] / 127u);
    } else if (hi == 0xC0u && d1 < NPRESET) {   /* Program Change 0..63: that preset, now */
        pend_preset = -1; preset_recall(d1); dirty_hdr = 1; dirty_cells = 0xF;
    } else if (hi == 0xB0u && d1 == 123u) { stop_all(); arp_cnt = 0; arp_fingers = 0; arp_publish(); }
}
static void midi_value(uint32_t c, char *s)
{
    char *e;
    *s = 0;
    switch (c) {
    case 0:
        if (!clk.ext) e = cat(s, "INTERNO");
        else if (fm1_ms - ext_last_ms > 500u) e = cat(s, "ESTERNO");
        else { fmt_u(s, ext_bpm10 / 10u); e = s; while (*e) e++; e = cat(e, " BPM"); }
        if (clk.out) cat(e, "+OUT");
        break;
    case 1:
        if (!store.rx) cat(s, "TUTTO");
        else if (store.rx == 1u) cat(s, "SOLO CLOCK");
        else { e = cat(s, "CANALE "); fmt_u(e, store.rx - 1u); }
        break;
    case 2: cat(s, learn_on ? "ATTIVO" : "SPENTO"); break;
    default: cat(s, del_all ? "TUTTE" : "ULTIMA"); break;
    }
}
static void midi_knob(uint32_t c, int32_t d)   /* the MIDI page's knobs */
{
    if (c == 0u) {                          /* INTERNO, INTERNO+OUT, ESTERNO, ESTERNO+OUT */
        int32_t at = (int32_t)(clk.ext * 2u + clk.out) + (d > 0 ? 1 : -1);
        if (at < 0) at = 0;
        if (at > 3) at = 3;
        if ((uint32_t)at >> 1 != clk.ext) clock_mode((uint32_t)at >> 1);
        clk.out = (uint32_t)at & 1u; store.clk_out = (uint8_t)clk.out; settings_touch();
    } else if (c == 1u) {                   /* RICEVI: TUTTO, SOLO CLOCK, CANALE 1..16 */
        int32_t r = (int32_t)store.rx + d;
        store.rx = (uint8_t)(r < 0 ? 0 : r > 17 ? 17 : r);
        settings_touch();
    }
    else if (c == 2u) { if ((d > 0) != (learn_on != 0u)) learn_start(d > 0); }
    else del_all = d > 0;
    dirty_cells |= 1u << c;
}
static void midi_delete(void)               /* OCT+ on the MIDI page: CANCELLA */
{
    if (!store.nlearn) { learn_say("LEARN: NIENTE DA CANCELLARE", 0); return; }
    if (del_all) store.nlearn = 0; else store.nlearn--;
    settings_touch();
    learn_say(del_all ? "LEARN: CANCELLATE TUTTE" : "LEARN: CANCELLATA L'ULTIMA", 0);
}
static void draw_midi_cell(uint32_t c)
{
    char s[24];
    const felucca_font_t *f = &FONT_L;
    (void)f;
    cell_begin();
    cv_text(6, 2, &FONT_S, MIDI_PN[c], C_LAB);
    midi_value(c, s);
    cell_value(s, c == 2u && learn_on ? C_ACC : C_WHITE);
    if (c == 3u) cv_text(112 - text_w(&FONT_S, "OCT+"), 2, &FONT_S, "OCT+", C_TAB);   /* the button that confirms it */
    cv_blit(c & 1u ? 121 : 1, c < 2u ? 140 : 190);
}

#if GRANA_BT
#include "grana_bt.c"
#elif defined(GRANA_WEB)
static uint32_t smc_sent[64], smc_nsent;    /* the simulator: what would go to the SMC-Mixer (tests) */
static int bt_send(uint32_t st, uint32_t d1, uint32_t d2) { smc_sent[smc_nsent++ & 63u] = st << 16 | d1 << 8 | d2; return 1; }
#include "grana_smc.c"
__attribute__((export_name("smc_in"))) void web_smc_in(uint32_t st, uint32_t d1, uint32_t d2) { smc_in((st >> 4) | st << 8 | d1 << 16 | d2 << 24); }
__attribute__((export_name("smc_frame"))) void web_smc_frame(void) { smc_frame(); }
__attribute__((export_name("smc_sent"))) uint32_t *web_smc_sent(void) { return smc_sent; }
__attribute__((export_name("smc_nsent"))) uint32_t web_smc_nsent(void) { uint32_t n = smc_nsent; smc_nsent = 0; return n; }
#endif
static void app_step(void)
{
    static uint32_t t_wave, dice_seen;
    uint32_t keys, down, up, b, rel, k, kb, lit = 0;
    uint8_t st, d1, d2;
    int32_t d;

    if (intro) {                            /* the opening, a frame every 33 ms; a key or a button skips it */
        uint32_t r2;
        if (fm1_input_edges(&r2) || (fm1_in.notes & ~prev_keys) || fm1_ms - intro_t0 >= IN_END) { prev_keys = fm1_in.notes; intro_finish(); }
        else if (fm1_ms - intro_last >= 33u) { intro_last = fm1_ms; intro_frame(); }
        if (intro) return;
    }

    kb = (uint32_t)(53 + 12 * octave);      /* key 0 = F3 */
    keys = fm1_in.notes;
    down = keys & ~prev_keys;
    up = prev_keys & ~keys;
    prev_keys = keys;
    for (k = 0; k < 27u; k++) {
        if (down >> k & 1u) {
            held[k] = (uint8_t)(kb + k);
            if (split_arm && page == PG_SPLIT && fm1_ms - t_split < 5000u) split_key(held[k]);   /* 1.5: the split where the key is */
            note_on(held[k], 110);
            midi_send(0x90, held[k], 110);
            if (rec) step_write(held[k]);
        }
        if (up >> k & 1u) { note_off(held[k]); midi_send(0x80, held[k], 0); }
    }

    b = fm1_input_edges(&rel) | smc_btn;
    smc_btn = 0;
    {   /* SEL: MOTO, again MOTO 2; held 0.6 s: the dice */
        static uint32_t t_sel, sel_fired;
        if (b & (1u << BTN_SEL)) { t_sel = fm1_ms; sel_fired = 0; }
        if ((fm1_in.buttons >> BTN_SEL & 1u) && !sel_fired && fm1_ms - t_sel >= 600u) { sel_fired = 1; dice_throw(); }
        if ((rel & (1u << BTN_SEL)) && !sel_fired && !save_mode) goto_page(page == PG_MOTO ? PG_MOTO2 : PG_MOTO);
    }
    {   /* HOME held 0.6 s: HOLD (a short press is free, for what comes next) */
        static uint32_t t_home, home_fired;
        if (b & (1u << BTN_HOME)) { t_home = fm1_ms; home_fired = 0; }
        if ((fm1_in.buttons >> BTN_HOME & 1u) && !home_fired && fm1_ms - t_home >= 600u) { home_fired = 1; hold_toggle(keys); }
        if ((rel & (1u << BTN_HOME)) && !home_fired && !save_mode)   /* short: STRATI, SPLIT, QUANTITA', PITCH, INI/AREA 1-2, 3-4 */
            goto_page(page == PG_STR ? PG_SPLIT : page == PG_SPLIT ? PG_LIV : page == PG_LIV ? PG_LPIT : page == PG_LPIT ? PG_LINI :
                      page == PG_LINI ? PG_LAREA : PG_STR);
    }
    if (save_mode && ((b & ~(1u << BTN_SAVE)) || fm1_ms - t_savemode > 20000u)) {   /* another button, or 20 s: no save */
        save_mode = 0; dirty_tabs = 1; dirty_hdr = 1;
    }
    if (b & (1u << BTN_EDIT)) goto_page(page == PG_GRANI ? PG_MOT : PG_GRANI);   /* GRANI, MOTORE (1.8) */
    if (b & (1u << BTN_ENV)) goto_page(page == PG_FORMA ? PG_TAST : PG_FORMA);   /* FORMA, TASTIERA (1.6) */
    if (b & (1u << BTN_GLO))                /* VOLUMI, PAN STRATI, MIX, MIDI (BT) */
        goto_page(page == PG_VOL ? PG_LPAN : page == PG_LPAN ? PG_MIX : page == PG_MIX ? PG_MIDI :
                  page == PG_MIDI && GRANA_BT ? PG_BT : PG_VOL);
    if (b & (1u << BTN_FX)) goto_page(page == PG_FILT ? PG_MOD : page == PG_MOD ? PG_DLY : page == PG_DLY ? PG_NAS2 :
                                      page == PG_NAS2 ? PG_RIV : page == PG_RIV ? PG_RIV2 : PG_FILT);   /* FILTRO, MODULAZIONE, NASTRO 1 and 2, RIVERBERO 1 and 2 */
    {   /* SEQ: its page; in REC held 1 s, it empties the sequence (a bar fills meanwhile, as SAVE's) */
        static uint32_t t_seq, seq_fired;
        if (b & (1u << BTN_SEQ)) { t_seq = fm1_ms; seq_fired = 0; if (page != PG_SEQ) goto_page(PG_SEQ); }
        if (rec && page == PG_SEQ && (fm1_in.buttons >> BTN_SEQ & 1u) && !seq_fired) {
            seq_clear_t = fm1_ms - t_seq;
            if (seq_clear_t >= 1000u) {
                for (k = 0; k < NSTEP; k++) { seq_note[k] = 0; seq_pos[k] = 255; }
                cursor = 0; edited = 1; dirty_hdr = 1; seq_fired = 1; seq_clear_t = 0;
                learn_say("SEQUENZA CANCELLATA", 0);
            }
            dirty_tabs = 1;
        } else if (seq_clear_t) { seq_clear_t = 0; dirty_tabs = 1; }
    }
    if (b & (1u << BTN_LFO)) goto_page(page == PG_LFO1 ? PG_LFO2 : page == PG_LFO2 ? PG_SPEC : PG_LFO1);   /* LFO 1, LFO 2, SPECIALI */
    if ((b & (1u << BTN_ARP)) && page != PG_ARP) goto_page(PG_ARP);   /* ARP: its page first, then on and off there */
    else if (b & (1u << BTN_ARP)) {
        clk.arp = !clk.arp;
        dirty_tabs = 1;
        arp_cnt = 0; arp_fingers = 0;
        if (clk.arp) {                      /* the keys already down start the arpeggio */
            stop_all();
            for (k = 0; k < 27u; k++) if (keys >> k & 1u) arp_add(held[k]);
        }
        arp_publish();
        goto_page(PG_ARP);
    }
    if (b & (1u << BTN_PLAY)) {
        if (!clk.run) { clk.restart = 1; lfo_restart(); }
        clk.run = !clk.run;
        if (clk.out) midi_send(clk.run ? 0xFAu : 0xFCu, 0, 0);   /* CLOCK OUT: START and STOP with it */
    }
    if (b & (1u << BTN_REC)) {
        rec = !rec;
        if (rec) { cursor = 0; goto_page(PG_SEQ); }
        dirty_tabs = 1;
    }
    if (b & (1u << BTN_OCT_DOWN)) {
        if (rec) step_write(0);             /* in REC: a rest */
        else if (octave > -3) octave--;
    }
    if (b & (1u << BTN_OCT_UP)) {
        if (rec) step_write(1);             /* in REC: a tie */
        else if (page == PG_MIDI) midi_delete();   /* the MIDI page: OCT+ confirms CANCELLA */
#if GRANA_BT
        else if (page == PG_BT) bt_confirm();   /* the BLUETOOTH page: OCT+ acts on DISPOSITIVO */
#endif
        else if (octave < 3) octave++;
    }
    if (b & (1u << BTN_SAVE)) {
        if (!save_mode) { save_mode = 1; save_dest = pend_preset >= 0 ? (uint32_t)pend_preset : cur_preset; t_savemode = fm1_ms; t_savehold = 0; name_start(); }
        else if (!pr[save_dest].used) { save_mode = 0; save_to(save_dest); }   /* an empty place: saved */
        else { t_savehold = fm1_ms ? fm1_ms : 1u; t_savemode = fm1_ms; }               /* taken: SAVE held 1 s replaces it */
        dirty_tabs = 1; dirty_hdr = 1;
    }
    if (save_mode && t_savehold) {
        if (!(fm1_in.buttons >> BTN_SAVE & 1u)) { t_savehold = 0; dirty_tabs = 1; }
        else if ((int32_t)(fm1_ms - t_savehold) >= 1000) { save_mode = 0; t_savehold = 0; save_to(save_dest); dirty_tabs = 1; dirty_hdr = 1; }
        else dirty_tabs = 1;
    }

    for (k = 0; k < 4u; k++)                /* the four knobs (while SAVE asks: the name) */
        if ((d = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
            uint32_t p = pp(page, k);
            if (save_mode) { name_knob(k, d); continue; }
            if (page == PG_MIDI) { midi_knob(k, d); continue; }
#if GRANA_BT
            if (page == PG_BT) { bt_knob(k, d); continue; }
#endif
            if (learn_on) {                 /* LEARN: this knob's parameter, its value untouched */
                if (p < NP && PMAX[p]) { learn_par = (int32_t)p; t_learn = fm1_ms; learn_try(); }
                continue;
            }
            param_change(p, accel(d, PMAX[p]));
            if (p == P_SLEN && cursor > par[P_SLEN]) cursor = 0;
            if (p >= P_SP1 && p <= P_SP3 && par[P_DISP]) {   /* a split turned: for 5 s a key can set it instead */
                split_arm = p; t_split = fm1_ms;
                learn_say(PNAME[p], ": O PREMI UN TASTO");
            } else if (p == P_DISP) dirty_cells = 0xF;
        }
    if ((d = fm1_enc_take(ENC_SELECT)) != 0) {
        if (rec) {                          /* in REC: the position, a step's own place in the sound */
            param_change(P_POS, accel(d, 200));
            if (page == PG_MOTO) dirty_cells |= 1u;
        } else if (learn_on) { learn_par = P_SND; t_learn = fm1_ms; learn_try(); }
        else {                              /* 1.8: the sound (ALGORITHM's before), named in the strip */
            char t[24], *e;
            param_change(P_SND, d);
            e = cat(t, "CAMPIONE ");
            fmt_u(e, par[P_SND] + 1u); while (*e) e++; *e++ = ' '; *e = 0;
            learn_say(t, snd_name(par[P_SND]));
            if (page == PG_STR) dirty_cells |= 1u;
        }
    }
    if ((d = fm1_enc_take(ENC_ALGORITHM)) != 0) {   /* 1.8: the engine, and its page */
        if (learn_on) { learn_par = P_MOTORE; t_learn = fm1_ms; learn_try(); }
        else {
            uint32_t was = par[P_MOTORE], k;
            param_change(P_MOTORE, d);
            if (par[P_MOTORE] != was) {
                for (k = 0; k < 4u; k++) par[P_M1 + k] = base[P_M1 + k] = MOT_DEF[par[P_MOTORE] & 3u][k];   /* its own start */
                goto_page(PG_MOT);
                t_flash = fm1_ms ? fm1_ms : 1u;
                draw_flash();
                dirty_hdr = 1;
            }
        }
    }
    if ((d = fm1_enc_take(ENC_PRESETS)) != 0) {
        if (save_mode) {                    /* choosing where to save */
            int32_t p = (int32_t)save_dest + preset_accel(d);
            while (p < 0) p += NPRESET;
            save_dest = (uint32_t)p % NPRESET;
            t_savemode = fm1_ms;
            t_savehold = 0;
            if (!name_touched) name_start();   /* the new place's name, unless one is being written */
            dirty_tabs = 1;
        } else if (rec) {                          /* in REC: the cursor */
            int32_t c = (int32_t)cursor + d, n = (int32_t)par[P_SLEN] + 1;
            while (c < 0) c += n;
            cursor = (uint32_t)(c % n);
        } else {                            /* the next preset, loaded once the knob rests */
            int32_t p = (pend_preset >= 0 ? pend_preset : (int32_t)cur_preset) + preset_accel(d);
            while (p < 0) p += NPRESET;
            pend_preset = p % (int32_t)NPRESET;
            t_pend = fm1_ms;
            dirty_hdr = 1;
        }
    }
    if (pend_preset >= 0 && fm1_ms - t_pend >= 350u) {
        preset_recall((uint32_t)pend_preset);
        pend_preset = -1;
        dirty_hdr = 1; dirty_cells = 0xF; dirty_tabs = 1;
    }

    while (midi_get(&st, &d1, &d2)) midi_in(st, d1, d2);   /* USB and the TRS jack */
#if GRANA_BT
    {   /* after the opening: the trims and the vectors; AVVIO SI: on by itself */
        static uint8_t bt_inited;
        if (!bt_inited) { bt_inited = 1; bt_boot(); if (store.bt_auto) bt_set(1); }
    }
    bt_step();                              /* Bluetooth: the controller's messages, the link */
    {
        uint8_t mk = (uint8_t)(!bt_on ? 0u : bt_state() == BTS_LINK ? 2u : 1u);
        if (mk != bt_link_mark) { bt_link_mark = mk; dirty_hdr = 1; }
    }
    if (bt_kept_new) { bt_kept_new = 0; settings_touch(); }
#endif
    if (learn_on && fm1_ms - t_learn > 30000u) { learn_start(0); learn_say("LEARN: TEMPO SCADUTO", 0); if (page == PG_MIDI) dirty_cells |= 4u; }
    if (t_lmsg && fm1_ms - t_lmsg > 2500u) { t_lmsg = 0; lmsg[0] = 0; dirty_tabs = 1; if (page == PG_MIDI) dirty_cells = 0xF; }
    if (page == PG_MIDI && clk.ext) {       /* the clock's tempo, or its absence, kept up to date */
        static uint32_t t_mc;
        if (fm1_ms - t_mc > 500u) { t_mc = fm1_ms; dirty_cells |= 1u; }
    }

    if (dice_req != dice_seen) { dice_seen = dice_req; dice_throw(); }   /* OGNI: the clock asks */
    if (!intro && layers_partial() && fm1_ms - t_posmove > 150u && fm1_ms - lay_tload > 400u) {
        uint32_t L;                         /* a window left behind by POSIZIONE (or the dice): moved */
        for (L = 0; L < NL; L++) {
            layer_t *ly = &lay[L];
            int32_t want, dd;
            if (!ly->len || ly->len >= ly->full || ly->snd == NOSND) continue;
            want = lay_wstart(L, ly->snd, ly->len);
            dd = want - ly->wstart;
            if (dd < 0) dd = -dd;
            if ((uint32_t)dd > ly->len / 8u) { layers_sync(0); break; }
        }
    }
    if (par[P_DRONE] == 1u) drone_update();  /* ULTIMA follows the last note played */

    /* LEDs */
    for (k = 0; k < NV; k++)
        if (voice[k].gate && voice[k].note >= kb && voice[k].note < kb + 27u) lit |= 1u << (voice[k].note - kb);
    for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), lit >> k & 1u);
    fm1_led_key(BTN_SEL, page == PG_MOTO || page == PG_MOTO2);   /* the button of the page in view, lit */
    fm1_led_key(BTN_HOME, hold || page == PG_STR || page == PG_SPLIT || page == PG_LIV || (page >= PG_LPIT && page <= PG_LAREA));
    {   /* ARP and SEQ's strip follows what they do */
        static uint32_t was;
        uint32_t now = clk.arp | clk.run << 1 | hold << 2 | (uint32_t)par[P_TASTO] << 3;
        if (now != was) { was = now; if (page == PG_ARP || page == PG_SEQ) dirty_tabs = 1; }
    }
    fm1_led_key(BTN_EDIT, page == PG_GRANI || page == PG_MOT);
    fm1_led_key(BTN_ENV, page == PG_FORMA || page == PG_TAST);
    fm1_led_key(BTN_GLO, page == PG_MIX || page == PG_VOL || page == PG_LPAN || page == PG_MIDI || page == PG_BT);
    fm1_led_key(BTN_FX, page == PG_FILT || page == PG_MOD || page == PG_DLY || page == PG_NAS2 || page == PG_RIV || page == PG_RIV2);
    fm1_led_key(BTN_ARP, clk.arp);          /* 1.4: lit while the arpeggio plays, whatever the page */
    fm1_led_key(BTN_SEQ, clk.run);          /* lit while the sequence plays */
    fm1_led_key(BTN_LFO, page == PG_SPEC || page == PG_LFO1 || page == PG_LFO2);
    fm1_led_key(BTN_PLAY, clk.run && (seq_play < 0 || (seq_play & 3) == 0));   /* blinks on the beat */
    fm1_led_key(BTN_REC, rec && (fm1_ms >> 7 & 1u));   /* REC blinks while it writes */
    fm1_led_key(BTN_OCT_DOWN, octave < 0);
    fm1_led_key(BTN_OCT_UP, octave > 0);
    fm1_led_key(BTN_SAVE, save_mode ? (fm1_ms >> 7 & 1u) : edited);

    /* the screen: what changed, the outline 25 times a second */
    if (lfo_tick()) engine_update();        /* the LFOs move their destinations */
    if (t_flash && fm1_ms - t_flash > 700u) { t_flash = 0; dirty_cells = 0xF; }   /* the page's name, then its knobs */
    if (batt_shown() != batt_seen) { batt_seen = batt_shown(); dirty_hdr = 1; }   /* 1.8: the battery's level, its blink */
    if (dirty_hdr) { dirty_hdr = 0; draw_header(); }
    if (dirty_tabs) { dirty_tabs = 0; draw_tabs(); }
    {   /* while SAVE asks: the name where the knobs were; after, the knobs again */
        static uint32_t was_saving;
        if (save_mode && (name_dirty || !was_saving)) { name_dirty = 0; t_flash = 0; draw_name(); }
        if (!save_mode && was_saving) dirty_cells = 0xF;
        was_saving = save_mode;
    }
    if (!t_flash && !save_mode)
        for (k = 0; k < 4u; k++) {
            uint32_t pk = pp(page, k);
            if (pk < NP && PMAX[pk] && epar[pk] != cell_drawn[k] && fm1_ms - t_wave >= 39u) dirty_cells |= 1u << k;   /* an LFO's mark moved */
            if (dirty_cells >> k & 1u) {
#if GRANA_BT
                if (page == PG_BT) draw_bt_cell(k); else
#endif
                if (page == PG_MIDI) draw_midi_cell(k); else draw_cell(k);
                dirty_cells &= ~(1u << k);
            }
        }
    if (fm1_ms - t_wave >= 40u) { t_wave = fm1_ms; draw_wave(); }

    if (save_pending && quiet() && fm1_ms - t_save > 300u) {   /* the erase stops the sound: only when silent */
        presets_flush();
        save_pending = 0;
    }
    if (((t_cur && pr[cur_preset].used && store.cur != cur_preset && fm1_ms - t_cur > 3000u) ||
         (set_dirty && fm1_ms - t_set > 2000u)) && quiet()) {
        store.ver = SET_VER;                /* the preset in use, the MIDI page, for the next power-on */
        if (pr[cur_preset].used) store.cur = (uint8_t)cur_preset;
        settings_save(&store, sizeof store);
        t_cur = 0;
        set_dirty = 0;
    }
}

/* ---- the presets over USB-MIDI SysEx, for the editor: F0 7D 47 52 cmd ... F7 (7D: non-commercial, "GR")
 *   01                  dump: the FM-1 answers 64 x  02 idx <256 B as 512 nibbles, high first>, then 05
 *   03 idx <512 nibbles> a preset into RAM (an empty one: used 0); answers 06 idx
 *   04                  keep them: written to flash when nothing plays, the current one recalled; answers 05 */
static uint32_t sx_got;                     /* presets received since the last 04 */
static void preset_seal(prec_t *r) { r->magic = PREC_MAGIC; r->ver = PREC_VER; r->np = NP; r->sum = prec_sum(r); }
static void bank_reload(void)               /* the banks changed: reopen them, keep the sounds that still exist */
{
    bank_open();
    if (par[P_SND] >= nsounds()) par[P_SND] = base[P_SND] = 0;
    { uint32_t k; for (k = P_SND2; k <= P_SND4; k++) if (par[k] > nsnd) par[k] = base[k] = 0; }
    { uint32_t L; for (L = 0; L < NL; L++) peak_snd[L] = NOSND; }
    layers_sync(1);
    dirty_hdr = 1;
    dirty_cells = 0xF;
}
/* 1.6, the user bank over USB-MIDI (F0 7D 47 52 cmd ... F7; a = a 256-byte block number in three 7-bit bytes):
 *   10                  start: only the factory bank plays, the user bank's head erased   -> 11
 *   12 a a a + 512 nib  a block of the user bank (its sector erased first at each 4 KiB)  -> 13 a a a, or 1F code
 *   14                  end: the banks reopened                                            -> 15 nfact nuser
 *   16 a a a            read a block of the flash above the code (0x93000..)              -> 17 a a a + 512 nib
 *   18                  what is there                                                      -> 19 nfact nuser */
static void ub_reply(uint32_t cmd, const uint8_t *a, uint32_t x, uint32_t y)
{
    uint8_t m[10];
    uint32_t n = 5;
    m[0] = 0xF0; m[1] = 0x7D; m[2] = 0x47; m[3] = 0x52; m[4] = (uint8_t)cmd;
    if (a) { m[n++] = a[0] & 0x7Fu; m[n++] = a[1] & 0x7Fu; m[n++] = a[2] & 0x7Fu; }
    else { m[n++] = (uint8_t)(x & 0x7Fu); m[n++] = (uint8_t)(y & 0x7Fu); }
    m[n++] = 0xF7;
    sysex_send(m, n);
}
static int ub_sysex(const uint8_t *p, uint32_t n)   /* 1: one of the user bank's */
{
    static uint8_t blk[256], m[9 + 512 + 1];
    uint32_t k, at;
    switch (p[0]) {
    case 0x10u:
        stop_all();
        ub_busy = 1;
        ub_total = n >= 4u ? ((uint32_t)(p[1] & 0x7Fu) << 14 | (uint32_t)(p[2] & 0x7Fu) << 7 | (p[3] & 0x7Fu)) : 0u;
        ub_got = 0;
        learn_say("BANCA UTENTE: RICEVO...", 0);
        bank_reload();                       /* no user sound may play from a sector being erased */
        if (ub_erase(0)) { ub_reply(0x1Fu, 0, 1, 0); return 1; }
        ub_reply(0x11u, 0, 0, 0);
        return 1;
    case 0x12u:
        if (n < 4u + 512u) { ub_reply(0x1Fu, 0, 2, 0); return 1; }
        at = ((uint32_t)(p[1] & 0x7Fu) << 14 | (uint32_t)(p[2] & 0x7Fu) << 7 | (p[3] & 0x7Fu)) * 256u;
        if (!ub_busy || at + 256u > UB_LEN) { ub_reply(0x1Fu, 0, 3, 0); return 1; }
        for (k = 0; k < 256u; k++) blk[k] = (uint8_t)((p[4 + 2u * k] & 15u) << 4 | (p[5 + 2u * k] & 15u));
        if (!(at & 0xFFFu) && at && ub_erase(at)) { ub_reply(0x1Fu, 0, 4, 0); return 1; }
        if (ub_write(at, blk, 256u)) { ub_reply(0x1Fu, 0, 5, 0); return 1; }
        ub_reply(0x13u, p + 1, 0, 0);
        if (!(++ub_got & 7u)) {              /* on the screen: how far */
            char t[24], *e = t;
            fmt_u(e, ub_got / 4u); while (*e) e++;
            if (ub_total) { e = cat(e, " DI "); fmt_u(e, ub_total / 4u); while (*e) e++; }
            cat(e, " KB");
            learn_say("BANCA UTENTE: ", t);
        }
        return 1;
    case 0x14u:
        ub_busy = 0;
        bank_reload();
        ub_reply(0x15u, 0, nfact, nsnd - nfact);
        if (nsnd > nfact) { char t[8]; fmt_u(t, nsnd - nfact); learn_say("BANCA UTENTE: SUONI ", t); }
        else learn_say(ub_diag == 1 ? "BANCA UTENTE: NON LEGGIBILE" : "BANCA UTENTE: NON VALIDA", 0);
        return 1;
    case 0x16u:
        if (n < 4u) return 1;
        at = ((uint32_t)(p[1] & 0x7Fu) << 14 | (uint32_t)(p[2] & 0x7Fu) << 7 | (p[3] & 0x7Fu)) * 256u;
        if (flash_peek(at, blk, 256u)) { ub_reply(0x1Fu, 0, 6, 0); return 1; }
        m[0] = 0xF0; m[1] = 0x7D; m[2] = 0x47; m[3] = 0x52; m[4] = 0x17;
        m[5] = p[1] & 0x7Fu; m[6] = p[2] & 0x7Fu; m[7] = p[3] & 0x7Fu;
        for (k = 0; k < 256u; k++) { m[8 + 2u * k] = blk[k] >> 4; m[9 + 2u * k] = blk[k] & 15u; }
        m[8 + 512] = 0xF7;
        sysex_send(m, 9u + 512u);
        return 1;
    case 0x18u:
        ub_reply(0x19u, 0, nfact, nsnd - nfact);
        return 1;
    default: return 0;
    }
}
static void app_sysex(const uint8_t *p, uint32_t n)
{
    static uint8_t m[6 + 512 + 1];
    uint32_t i, k;
    if (!n) return;
    if (ub_sysex(p, n)) return;
    if (p[0] == 0x01u) {
        for (i = 0; i < NPRESET; i++) {
            prec_t r = pr[i];
            const uint8_t *b = (const uint8_t *)&r;
            if (r.used) preset_seal(&r);
            m[0] = 0xF0; m[1] = 0x7D; m[2] = 0x47; m[3] = 0x52; m[4] = 0x02; m[5] = (uint8_t)i;
            for (k = 0; k < 256u; k++) {
                uint8_t v = k < sizeof r ? b[k] : 0xFFu;
                m[6 + 2u * k] = v >> 4; m[7 + 2u * k] = v & 15u;
            }
            m[6 + 512] = 0xF7;
            if (sysex_send(m, sizeof m)) return;
        }
        m[4] = 0x05; m[5] = 0xF7;
        sysex_send(m, 6);
    } else if (p[0] == 0x03u && n >= 2u + 2u * sizeof(prec_t) && p[1] < NPRESET) {
        prec_t r;
        uint8_t *b = (uint8_t *)&r;
        for (k = 0; k < sizeof r; k++) b[k] = (uint8_t)((p[2 + 2u * k] & 15u) << 4 | (p[3 + 2u * k] & 15u));
        i = p[1];
        if (prec_up18(&r) || (r.magic == PREC_MAGIC && r.ver == PREC_VER && r.np == NP && r.sum == prec_sum(&r) && r.used)) {
            for (k = 0; k < NP; k++) if (r.p[k] > PMAX[k]) r.p[k] = PMAX[k];
            r.name[11] = 0;
            pr[i] = r;
        } else { for (k = 0; k < sizeof r; k++) ((uint8_t *)&pr[i])[k] = 0; }   /* an empty place */
        save_sectors |= 1u << (i / 16u);
        sx_got++;
        m[0] = 0xF0; m[1] = 0x7D; m[2] = 0x47; m[3] = 0x52; m[4] = 0x06; m[5] = (uint8_t)i; m[6] = 0xF7;
        sysex_send(m, 7);                   /* 06 idx: taken, the next one may come */
    } else if (p[0] == 0x04u) {
        if (sx_got) { save_pending = 1; t_save = fm1_ms; sx_got = 0; }
        if (!pr[cur_preset].used) cur_preset = 0;
        if (pr[cur_preset].used) preset_recall(cur_preset);
        dirty_hdr = dirty_tabs = 1; dirty_cells = 0xF;
        m[0] = 0xF0; m[1] = 0x7D; m[2] = 0x47; m[3] = 0x52; m[4] = 0x05; m[5] = 0xF7;
        sysex_send(m, 6);
    }
}

#ifdef GRANA_WEB
static void web_presets_out(void)           /* the editor reads the simulator's presets from the user area */
{
    save_sectors = 0xFu;
    presets_flush();
}
static void web_presets_in(void)            /* the editor wrote the user area: the presets again, the current one recalled */
{
    presets_load();
    if (!pr[cur_preset].used) cur_preset = 0;
    preset_recall(cur_preset);
    dirty_hdr = dirty_tabs = 1; dirty_cells = 0xF;
}
static uint8_t *web_bank_buf(void) { return web_bank; }
static uint32_t web_bank_size(void) { return sizeof web_bank; }
static void web_bank_changed(void)          /* the page wrote a new bank (or user bank): reopen them */
{
    bank_reload();
}
static uint32_t web_state(void)             /* for the page: what the panel cannot show by itself */
{
    return page | hold << 5 | clk.arp << 6 | clk.run << 7 | rec << 8 | (uint32_t)(octave + 3) << 9 | cur_preset << 12 | audio_us << 18;
}
__attribute__((export_name("par"))) uint8_t *web_par(void) { return par; }   /* tests: the parameters */
__attribute__((export_name("dbg"))) uint32_t web_dbg(uint32_t k)            /* tests: the clock, LEARN */
{
    switch (k) {
    case 0: return clk_int;
    case 1: return ext_ticks;
    case 2: return ext_bpm10;
    case 3: return (uint32_t)seq_play;
    case 4: return store.nlearn;
    case 5: return learn_on;
    case 6: return clk.run;
    case 7: return store.rx;                 /* 1.5: RICEVI */
    case 8: { uint32_t i, m = 0; for (i = 0; i < NV * NG; i++) if (vis_pos[i] >= 0 && vis_amp[i]) m |= 1u << (vis_lay[i] % NL); return m; }   /* the layers grains sound from */
    case 9: return zone_n;
    case 10: case 11: case 12: case 13: return (uint32_t)(voice[k - 10u].base_ct / 100 + 60);   /* 1.6: the note a voice sounds */
    case 14: { uint32_t v, j; for (v = 0; v < NV; v++) for (j = 0; j < NG; j++) if (voice[v].g[j].on) return (uint32_t)(voice[v].g[j].step < 0 ? -voice[v].g[j].step : voice[v].g[j].step) | (uint32_t)voice[v].g[j].lay << 24; return 0; }
    case 15: return nfact | nsnd << 8;
    default: return 0;
    }
}
#else
static void app_main(void)
{
    app_setup();
    for (;;) {
        platform_poll();
        app_step();
    }
}
#endif
