# Mario Texture's GRANA

**Un granulatore di campioni per l'M-VAVE FM-1**, nello spirito della MicroGranny 2: quattro strati di campioni
suonati da una nuvola di grani, effetti lo-fi, arpeggiatore, sequencer, 64 preset con nome e un editor nel browser
per caricare i propri campioni e installare il firmware.

*A granular sample player for the M-VAVE FM-1, in the spirit of the MicroGranny 2: four layers of samples played by a
cloud of grains, lo-fi effects, arpeggiator, sequencer, 64 named presets and a browser editor to load your own samples
and install the firmware. The interface and the docs are in Italian.*

> Firmware non ufficiale: non è fatto né approvato da M-VAVE. Si installa e si toglie via USB; per tornare al
> firmware originale basta reinstallarlo dall'editor ufficiale o dall'[FM-1 Firmware Hub](https://fm1.designburgapps.com).
> Lo usi a tuo rischio.

Versione **1.6.2**. Provata su un FM-1 vero (tasti neri).

## Installazione

1. Scarica dalla pagina [Releases](../../releases) l'editor `grana-editor-X.Y.Z.html` e aprilo in **Chrome o Edge**
   (serve Web MIDI e WebUSB).
2. Prova GRANA nel simulatore della pagina: è lo stesso codice che gira sull'FM-1.
3. Scegli i campioni (vedi sotto), collega l'FM-1 via USB e premi **Installa**. Resta sulla scheda finché non finisce.
4. Variante **GRANA + Bluetooth**: con la casella apposita l'FM-1 si collega da solo a un controller BLE-MIDI
   (SMC-Mixer, SMC-PAD…). Il codice è più lungo, quindi lascia un po' meno spazio ai campioni.

I pacchetti `grana-X.Y.Z.fwsc` (e `-bluetooth`) nelle Releases contengono i campioni di serie, per chi installa con
altri strumenti.

**Recupero**: se qualcosa va storto, accendi l'FM-1 tenendo premuto **OCT−**: parte solo l'aggiornamento via USB.
Nella cartella `rescue/` ci sono gli strumenti e le istruzioni (`rescue/LEGGIMI.txt`) per il caso peggiore.

## I campioni

- **Banca di fabbrica**: sta dentro il firmware, cambia solo reinstallando (circa 440 KB, 390 KB con il Bluetooth).
- **Banca utente**: 276 KB in una zona a parte della memoria dell'FM-1; si manda dall'editor con
  *Invia la banca utente all'FM-1*, **senza reinstallare**, e resta anche quando si aggiorna GRANA.
- Ogni campione a **22 kHz** o **11 kHz** (metà spazio, ancora più grana) e **HIFI** (16 bit) o **LOFI** (8 bit) in RAM.
  Tutto a 11 kHz si arriva a circa due minuti di campioni; l'editor mostra lo spazio che resta.
- Fino a 48 campioni in tutto. Otto suoni di serie, sintetizzati da `tools/make_samples.py`.

## Il suono

- Fino a 4 strati suonati insieme (SOVRAPPOSTI) o divisi sulla tastiera (SPLIT), ognuno con intonazione, inizio, area,
  pan, volume e quantità di grani propri.
- Grani: dimensione, densità, spray, jitter, salto, direzione, reverse, quattro finestre; scansione libera o ferma, dado.
- Filtro multimodo (ladder 24 dB, 12 dB, banda, passa-alto 12/24 dB, notch) con movimento lento.
- MODULAZIONE: VELVET (chorus, flanger, phaser e le vie di mezzo) o METALLO (ring modulator e frequency shifter).
- NASTRO: eco ping-pong, ripetizioni a tempo o a caso (beat repeat), echi al contrario, eco a nastro con wow e flutter,
  congelamento.
- Riverbero plate (struttura di Jon Dattorro) con shimmer; compressore; uscita a **12 bit** (o 16).
- Due LFO con molte destinazioni, arpeggiatore, sequencer a 64 passi, SCALE con aggancio dei tasti, drone, freeze, crush.
- MIDI: USB e presa TRS sempre attive, MIDI LEARN (32 associazioni), clock in e out, filtro RICEVI; SMC-Mixer via Bluetooth.

## Le pagine

Ogni pulsante apre le sue pagine, premuto di nuovo passa alla successiva; SELECT le scorre tutte. KNOB 1–4 sono i
quattro parametri della pagina.

| Pulsante | Pagina | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
| --- | --- | --- | --- | --- | --- |
| HOME | STRATI | STRATO 1 | STRATO 2 | STRATO 3 | STRATO 4 |
|  | SPLIT | DISPOSIZIONE | SPLIT 1 | SPLIT 2 | SPLIT 3 |
|  | QUANTITA' | LIVELLO 1 | LIVELLO 2 | LIVELLO 3 | LIVELLO 4 |
|  | PITCH STRATI | PITCH 1 | PITCH 2 | PITCH 3 | PITCH 4 |
|  | INI/AREA 1-2 | INIZIO 1 | AREA 1 | INIZIO 2 | AREA 2 |
|  | INI/AREA 3-4 | INIZIO 3 | AREA 3 | INIZIO 4 | AREA 4 |
| FX | FILTRO | MODO | FREQUENZA | RISONANZA | MOVIMENTO |
|  | MODULAZIONE | TIPO | VELOCITA' | PROFONDITA' | MIX |
|  | NASTRO | MODO | TEMPO | RITORNO | MIX |
|  | NASTRO 2 | VARIAZIONE | TONO | PASSO | RIP. A CASO |
|  | RIVERBERO | MIX | CODA | TONO | MODULAZIONE |
|  | RIVERBERO 2 | SHIMMER | INTERVALLO | TONO | DIFFUSIONE |
| SEL | MOTO | POSIZIONE | AREA | SCAN | DIREZIONE |
|  | MOTO 2 | JITTER | SALTO | DADO | OGNI |
| ENV | FORMA | FINESTRA | REVERSE | ATTACCO | RILASCIO |
|  | TASTIERA | RETRIG | TASTO | SCALA | TONICA |
| LFO | LFO 1 | FORMA | VELOCITA' | INTENSITA' | DESTINAZIONE |
|  | LFO 2 | FORMA | VELOCITA' | INTENSITA' | DESTINAZIONE |
|  | SPECIALI | DRONE | FREEZE | CRUSH | — |
| EDIT | GRANI | DIMENSIONE | DENSITA' | SPRAY | PITCH |
| GLO | VOLUMI | VOLUME 1 | VOLUME 2 | VOLUME 3 | VOLUME 4 |
|  | PAN STRATI | PAN 1 | PAN 2 | PAN 3 | PAN 4 |
|  | MIX | STEREO | COMPRESSORE | USCITA | VOLUME |
| ARP | ARP | MODO | DIVISIONE | OTTAVE | GATE |
| SEQ | SEQ | TEMPO | SWING | PASSI | DIVISIONE |
| GLO | MIDI | CLOCK | RICEVI | LEARN | CANCELLA |
| | BT (solo GRANA + Bluetooth) | BLUETOOTH | DISPOSITIVO | MODO | AVVIO |

Altri comandi: HOME tenuto = HOLD; SEL tenuto = dado; ALGORITHM = suono del primo strato; PRESETS = i 64 preset;
SAVE = salva (le manopole scrivono il nome); PLAY/REC = sequencer; OCT−/OCT+ = ottava.

## Compilare

Serve Linux x86-64 (o Docker), Python 3 con Pillow, la toolchain JieLi e tre file dello SDK JieLi AC79
(`uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`, licenza Apache-2.0, non inclusi qui).

```sh
tools/get_toolchain.sh                         # la toolchain JieLi in ~/.jieli
export AC79_SDK=~/fw-AC79_AIoT_SDK             # con cpu/wl82/tools/ dentro
./build.sh --release 1.6.2 [--bt]              # build/grana-1.6.2[-bluetooth].fwsc, build/grana-app[-bt].bin
sh web/build_wasm.sh                           # web/grana.wasm, il simulatore (clang con wasm32)
python3 web/make_page.py 1.6.2 FM-1_956 FM-1_966   # web/grana-editor.html
```

Prove sul simulatore: `node tests/t16.mjs web/grana.wasm <grana.wasm della 1.5> firmware/gen/bank_default.bin`,
`node tests/t_smc.mjs web/grana.wasm firmware/gen/bank_default.bin`, `node tests/t_midi.mjs …`.

## Crediti e licenza

GRANA è di **Mario Texture**, scritto con l'aiuto di Claude (Anthropic). È software libero sotto **GPL-3.0-only**
(`LICENSE`): chi distribuisce un firmware costruito da qui deve dare anche i sorgenti, con la stessa licenza.

Costruito su [Dinghy](https://github.com/zvenson/dinghy) (zvenson), che usa la piattaforma di
[Felucca](https://github.com/hugelton/Felucca) (Leo Kuroshita, Hügelton Instruments) come la porta
[SLOOP](https://github.com/isod89/sloop-fm1) (isod89). Bluetooth: lo stack di [Optimist](https://github.com/w0ts/optimist)
(w0ts), il collegamento ripreso da fm1-dock (Joakim Eriksson) e la modalità SMC-Mixer da
[Hortator](https://github.com/DeadActive/hortator) (DeadActive). Dettagli in `LICENSING.md`.

"M-VAVE" e "FM-1" sono marchi dei rispettivi proprietari.
