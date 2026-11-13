# Mario Texture's GRANA

**A granular sample player for the M-VAVE FM-1**, in the spirit of the MicroGranny 2: four layers of samples played by
a cloud of grains, lo-fi effects, arpeggiator, sequencer, 64 named presets and a browser editor to load your own samples
and install the firmware.

*[Versione italiana: LEGGIMI.md](LEGGIMI.md)*

> The interface (screen and editor) is in **Italian**; this page explains what the names mean.

> Unofficial firmware: not made or endorsed by M-VAVE. It installs and uninstalls over USB; to go back to the stock
> firmware, reinstall it with the official updater or the [FM-1 Firmware Hub](https://fm1.designburgapps.com).
> Use at your own risk.

Version **1.8**. Tested on a real FM-1 (black keys) up to 1.7; the 1.8 engines are tested in the simulator and the chip
emulator.

## Install

1. From the [Releases](../../releases) page download the editor `grana-editor-X.Y.Z.html` and open it in **Chrome or
   Edge** (it needs Web MIDI and WebUSB).
2. Try GRANA in the page's simulator: it is the same code that runs on the FM-1.
3. Choose your samples (see below), connect the FM-1 over USB and press **Installa** (install). Stay on the tab until
   it is done.
4. **GRANA + Bluetooth** variant: with its checkbox the FM-1 connects by itself to a BLE-MIDI controller (SMC-Mixer,
   SMC-PAD…). Its code is longer, so it leaves a little less room for samples.

The packages `grana-X.Y.Z.fwsc` (and `-bluetooth`) in the Releases hold the stock sounds, for those who install with
other tools.

**Recovery**: if something goes wrong, power the FM-1 on while holding **OCT−**: only the USB updater starts. The
`rescue/` folder has the tools and the instructions (`rescue/LEGGIMI.txt`, in Italian) for the worst case.

## Samples

- **Factory bank** (*banca di fabbrica*): lives inside the firmware, changes only by reinstalling (about 440 KB,
  390 KB with Bluetooth).
- **User bank** (*banca utente*): 276 KB in a separate area of the FM-1's memory; sent from the editor with
  *Invia la banca utente all'FM-1*, **without reinstalling**, and kept when GRANA is updated.
- Each sample at **22 kHz** or **11 kHz** (half the space, even more grit) and **HIFI** (16 bit) or **LOFI** (8 bit)
  in RAM. All at 11 kHz you get about two minutes of samples; the editor shows the room left.
- The user bank can be saved to and opened from a `.grnb` file (the bank exactly as the FM-1 stores it); presets go to a
  `.grnp` file from the Preset section.
- Up to 48 samples in all. Eight stock sounds, synthesised by `tools/make_samples.py`.

## The sound

- **Four engines** on ALGORITHM, each with its own character and a MOTORE page of four knobs: CLASSICO (the granulator
  as always), STIRAMENTO (time stands still: endless pads), FORMANTI (the pitch moves, the timbre stays: voices and
  instruments natural far from their pitch), RISONATORE (the grains pluck strings tuned to the note: an instrument from
  any sample, from string to bell). Two factory presets an engine (P9–P14).
- Up to 4 layers played together (SOVRAPPOSTI) or split across the keyboard (SPLIT), each with its own pitch, start,
  area, pan, volume and share of grains.
- Grains: size, density, spray, jitter, skip, direction, reverse, four windows; free or frozen scan, dice.
- Multimode filter (24 dB ladder, 12 dB, band-pass, 12/24 dB high-pass, notch) with slow movement.
- MODULAZIONE (modulation): VELVET (chorus, flanger, phaser and what lies between) or METALLO (ring modulator and
  frequency shifter).
- NASTRO (tape): ping-pong echo, repeats in time or at random (beat repeat), reverse echoes, tape echo with wow and
  flutter, freeze.
- Plate reverb (Jon Dattorro's figure) with shimmer; compressor; **12-bit** output (or 16).
- Two LFOs with many destinations, arpeggiator, 64-step sequencer, SCALES that snap the keys, drone, freeze, crush.
- MIDI: USB and TRS jack always on, MIDI LEARN (32 mappings), clock in and out, RICEVI filter (all / clock only / one
  channel); SMC-Mixer over Bluetooth.

## The pages

Each button opens its pages, pressed again it goes to the next. KNOB 1–4 are the
page's four parameters. Names as on the screen.

| Button | Page | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
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
|  | SPECIALI | DRONE | FREEZE | CRUSH | MOTORE |
| EDIT | GRANI | DIMENSIONE | DENSITA' | SPRAY | PITCH |
|  | MOTORE: STIRAMENTO | VELOCITA' | SFOCATURA | ARMONICI | DERIVA |
|  | MOTORE: FORMANTI | FORMANTE | LARGHEZZA | RESPIRO | VIBRATO |
|  | MOTORE: RISONATORE | DECADIMENTO | TIMBRO | DISPERSIONE | MIX |
| GLO | VOLUMI | VOLUME 1 | VOLUME 2 | VOLUME 3 | VOLUME 4 |
|  | PAN STRATI | PAN 1 | PAN 2 | PAN 3 | PAN 4 |
|  | MIX | STEREO | COMPRESSORE | USCITA | VOLUME |
| ARP | ARP | MODO | DIVISIONE | OTTAVE | GATE |
| SEQ | SEQ | TEMPO | SWING | PASSI | DIVISIONE |
| GLO | MIDI | CLOCK | RICEVI | LEARN | CANCELLA |
| | BT (GRANA + Bluetooth only) | BLUETOOTH | DISPOSITIVO | MODO | AVVIO |


A few words: STRATI layers · QUANTITA' share of grains · INI/AREA start and area · NASTRO tape · RIVERBERO reverb ·
MOTO motion · FORMA shape (envelope) · TASTIERA keyboard (SCALA scale, TONICA root) · SPECIALI specials · MOTORE engine (STIRAMENTO stretch, FORMANTI formants, RISONATORE resonator) ·
GRANI grains · VOLUMI volumes · USCITA output (12/16 bit) · RICEVI receive.

Other controls: HOME held = HOLD; SEL held = dice; ALGORITHM = engine (opens MOTORE); SELECT = sound of the first layer (in REC: the position); PRESETS = the 64 presets;
SAVE = save (the knobs write the name); PLAY/REC = sequencer; OCT−/OCT+ = octave.

At the top: the preset's name on the left, its number and the engine in the middle, the battery on the right (green,
yellow, a red one blinking when nearly empty; a bolt on USB power). MIDI CC 110–114: the engine and its four knobs.

## Building

You need Linux x86-64 (or Docker), Python 3 with Pillow, the JieLi toolchain and three files of the JieLi AC79 SDK
(`uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`, Apache-2.0, not included here).

```sh
tools/get_toolchain.sh                         # the JieLi toolchain in ~/.jieli
export AC79_SDK=~/fw-AC79_AIoT_SDK             # with cpu/wl82/tools/ inside
./build.sh --release 1.8 [--bt]              # build/grana-1.8[-bluetooth].fwsc, build/grana-app[-bt].bin
sh web/build_wasm.sh                           # web/grana.wasm, the simulator (clang with wasm32)
python3 web/make_page.py 1.8 FM-1_958 FM-1_968   # web/grana-editor.html
```

Simulator tests: `node tests/t16.mjs web/grana.wasm <1.5's grana.wasm> firmware/gen/bank_default.bin`,
`node tests/t_smc.mjs web/grana.wasm firmware/gen/bank_default.bin`, `node tests/t_midi.mjs …`.

## Credits and licence

GRANA is by **Mario Texture**, written with the help of Claude (Anthropic). It is free software under
**GPL-3.0-only** (`LICENSE`): whoever distributes a firmware built from here must give its source too, under the same
licence.

Built on [Dinghy](https://github.com/zvenson/dinghy) (zvenson), which uses the platform of
[Felucca](https://github.com/hugelton/Felucca) (Leo Kuroshita, Hügelton Instruments) as
[SLOOP](https://github.com/isod89/sloop-fm1) (isod89) carries it. Bluetooth: [Optimist](https://github.com/w0ts/optimist)'s
stack (w0ts), the glue after fm1-dock (Joakim Eriksson) and the SMC-Mixer mode after
[Hortator](https://github.com/DeadActive/hortator) (DeadActive). Details in `LICENSING.md`.

"M-VAVE" and "FM-1" are trademarks of their owners.
