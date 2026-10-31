# Licensing

Mario Texture's GRANA is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text
in `LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| GRANA: `firmware/src/grana.c`, `grana_bt.c`, `grana_smc.c`, `midi_uart.c` changes, the editor `web/`, `tools/make_samples.py`, `tests/t1*.mjs`, `tests/t_*.mjs`, `rescue/` scripts, `README.md` | Copyright (C) 2026 Mario Texture, written with the help of Claude (Anthropic) |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The starting point: [Dinghy](https://github.com/zvenson/dinghy) — `firmware/src/dinghy.c`, the wiring `platform.c`, the host mode `tests/host.c` | zvenson |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package (and inside the built editor, `grana-editor-X.Y.Z.html`, which makes the packages in the browser); no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |
| jl-uboot-tool (`rescue/jl-uboot-tool/`), with its loader blobs, unchanged | MIT, Copyright (c) 2023 Andrey Grigoryev (kagaimiq) | `rescue/jl-uboot-tool/LICENSE` |

## The logo

Dinghy's designburg signet is not part of GRANA and is not in this tree. GRANA's opening (a wheel of grana cheese) is
drawn by the code.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. GRANA is not made or endorsed by them.

## GRANA + Bluetooth

- `firmware/src/ble/`, `firmware/hal/fm1_ble.h`, `fm1_ble_rf.h`, `ble_rf_tables_v15.h` (copied to `firmware/gen/ble_rf_tables.h`):
  Optimist's BLE stack and WL82 driver (w0ts/optimist, commit 4b05f23, GPL-3.0-only OR MPL-2.0), unchanged. Written from the
  Bluetooth Core and BLE-MIDI specifications; see Optimist's `docs/BLE-STACK.md` for its provenance.
- `firmware/src/grana_bt.c`: GRANA's glue, after fm1-dock's `src/ble/hx.c` (Joakim Eriksson, GPL-3.0-only) and Optimist's
  `io/midi/midi_ble.c`.
- `firmware/src/grana_smc.c`: the SMC-Mixer's DAW mode, after Hortator's `smc.c` (DeadActive, GPL-3.0-only).
