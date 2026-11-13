// GRANA 1.8 in the browser build: the engines on ALGORITHM, the sounds on SELECT, MOTORE under EDIT, the factory presets 9..14,
// the old presets converted, the engines' pitch (FORMANTI, RISONATORE) and level
// node tests/t18.mjs web/grana.wasm firmware/gen/bank_default.bin
import fs from 'fs';
const W = process.argv[2], BANK = process.argv[3];
const { instance } = await WebAssembly.instantiate(fs.readFileSync(W), {});
const e = instance.exports, mem = () => new Uint8Array(e.memory.buffer);
mem().set(fs.readFileSync(BANK), e.bank_buf());
e.init();
let ms = 0, frames = 0, cap = null;
const BTN = { OCT_DOWN: 0, OCT_UP: 1, FX: 2, SEL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13 };
const ENC = { SELECT: 0, ALGORITHM: 1 };
const P_SND = 40, P_MOTORE = 99, P_M1 = 100, PG_GRANI = 1, PG_STR = 10, PG_MOT = 25;
function run(t) {
  for (let i = 0; i < t; i++) { ms++; e.step(ms); while (frames < ms * 44.1) { const p = e.audio(); frames += 256;
    if (cap) { const a = new Int32Array(e.memory.buffer, p, 512); for (let k = 0; k < 512; k += 2) cap.push(a[k] + a[k + 1]); } } }
}
function press(b) { e.button(BTN[b], 1); run(30); e.button(BTN[b], 0); run(30); }
const par = (p) => mem()[e.par() + p];
const page = () => e.state() & 31;
let ok = 0, bad = 0;
function check(name, cond, info = '') { if (cond) ok++; else bad++; console.log((cond ? 'OK  ' : 'NO  ') + name + (info ? '  ' + info : '')); }
function pitchOf(x, f0) {                   // the strongest of the first harmonics, by a fine DFT around them
  const n = x.length; let best = 0, bf = 0;
  for (let h = 1; h <= 4; h++) for (let c = -40; c <= 40; c++) {
    const f = f0 * h * 2 ** (c / 1200); let re = 0, im = 0;
    for (let i = 0; i < n; i += 2) { const a = 2 * Math.PI * f * i / 44100; re += x[i] * Math.cos(a); im += x[i] * Math.sin(a); }
    const m = re * re + im * im; if (m > best) { best = m; bf = f / h; }
  }
  return 1200 * Math.log2(bf / f0);         // cents off
}
run(4000);

// the factory presets 9..14: two an engine
const want = [[8, 1, 'MARE FERMO'], [9, 1, 'CATTEDRALE'], [10, 2, 'CORO BASSO'], [11, 2, 'SUSSURRO'], [12, 3, 'CORDE'], [13, 3, 'CAMPANE']];
for (const [i, m] of want) { e.midi(0xC0, i, 0); run(300); check(`P${i + 1}: motore ${m}`, par(P_MOTORE) === m, 'motore ' + par(P_MOTORE)); }
e.midi(0xC0, 0, 0); run(300);
check('P1: CLASSICO', par(P_MOTORE) === 0);

// ALGORITHM: the engine, its own start, the MOTORE page
e.enc(ENC.ALGORITHM, 1); run(100);
check('ALGORITHM +1: STIRAMENTO', par(P_MOTORE) === 1);
check('ALGORITHM: pagina MOTORE', page() === PG_MOT, 'page ' + page());
check('STIRAMENTO: i suoi valori di partenza', par(P_M1) === 20 && par(P_M1 + 1) === 30 && par(P_M1 + 2) === 15 && par(P_M1 + 3) === 20);
e.enc(ENC.ALGORITHM, 1); run(100); e.enc(ENC.ALGORITHM, 1); run(100); e.enc(ENC.ALGORITHM, 1); run(100);
check('ALGORITHM fino in fondo: RISONATORE, non oltre', par(P_MOTORE) === 3);
e.enc(ENC.ALGORITHM, -3); run(100);
check('ALGORITHM indietro: CLASSICO', par(P_MOTORE) === 0);

// SELECT: the sound (no longer the pages)
const pg0 = page(), s0 = par(P_SND);
e.enc(ENC.SELECT, 1); run(100);
check('SELECT +1: campione successivo', par(P_SND) === (s0 + 1) % 8, 'snd ' + par(P_SND));
check('SELECT non cambia pagina', page() === pg0);
e.enc(ENC.SELECT, -1); run(100);

// EDIT: GRANI, MOTORE, GRANI
press('EDIT'); check('EDIT: GRANI', page() === PG_GRANI, 'page ' + page());
press('EDIT'); check('EDIT di nuovo: MOTORE', page() === PG_MOT, 'page ' + page());
press('EDIT'); check('EDIT ancora: GRANI', page() === PG_GRANI);

// CC 110..114: MOTORE and its four
e.midi(0xB0, 110, 127); run(50); check('CC 110 = 127: RISONATORE', par(P_MOTORE) === 3);
e.midi(0xB0, 111, 0); run(50); check('CC 111 = 0: DECADIMENTO 0', par(P_M1) === 0);
e.midi(0xB0, 110, 0); run(50);

// the engines' pitch: FORMANTI (CORO BASSO) and RISONATORE (CORDE) on C3, C4, C5
for (const [pr, name] of [[10, 'CORO BASSO'], [12, 'CORDE']]) {
  e.midi(0xC0, pr, 0); run(400);
  e.midi(0xB0, 40, 0); e.midi(0xB0, 39, 0); e.midi(0xB0, 102, 0); e.midi(0xB0, 106, 0);   // no reverb, NASTRO, VELVET, shimmer
  if (pr === 10) e.midi(0xB0, 114, 0);       // FORMANTI: no vibrato
  run(100);
  for (const n of [48, 60, 72]) {
    const f0 = 440 * 2 ** ((n - 69) / 12);
    e.midi(0x90, n, 100); run(300); cap = []; run(400); const x = cap; cap = null; e.midi(0x80, n, 0); run(2500);
    const c = pitchOf(x.slice(0, 8192), f0);
    check(`${name}: nota ${n} intonata (entro 10 cent)`, Math.abs(c) <= 10, c.toFixed(1) + ' cent');
  }
}

// the level: each engine's factory presets loud enough, never clipping, on a chord
for (const pr of [8, 9, 10, 11, 12, 13]) {
  e.midi(0xC0, pr, 0); run(400);
  let ss = 0, n = 0, clip = 0; cap = [];
  for (const k of [48, 55, 60, 64]) e.midi(0x90, k, 100);
  run(2000); for (const k of [48, 55, 60, 64]) e.midi(0x80, k, 0); run(500);
  for (const v of cap) { ss += v * v; n++; if (Math.abs(v) >= 2 * 32767) clip++; }
  cap = null; run(3000);
  const rms = Math.sqrt(ss / n);
  check(`P${pr + 1}: accordo udibile e senza saturazione`, rms > 300 && clip === 0, 'rms ' + Math.round(rms) + ' clip ' + clip);
}

// a preset of 1.6 .. 1.7 (format 8, 100 parameters) converted: CLASSICO, the rest kept
{
  const PREC = 256, base = e.store();
  const st = new Uint8Array(e.memory.buffer, base, e.store_len());
  const r = st.subarray(4 + 2048 + 20 * PREC, 4 + 2048 + 21 * PREC);   // place 21
  r.fill(0); r[0] = 0xA7; r[1] = 8; r[2] = 100; r[3] = 1;
  for (let k = 0; k < 100; k++) r[16 + k] = 50;
  r[16 + 99] = 0; r[16 + P_SND] = 2;
  let h = 2166136261; for (let k = 0; k < 248; k++) h = Math.imul(h ^ r[k], 16777619) >>> 0;
  r[248] = h & 255; r[249] = h >>> 8 & 255; r[250] = h >>> 16 & 255; r[251] = h >>> 24;
  e.presets_in(); run(100);
  e.midi(0xC0, 20, 0); run(300);
  check('preset 1.7 convertito: CLASSICO, valori conservati', par(P_MOTORE) === 0 && par(P_SND) === 2 && par(0) === 50, `mot ${par(P_MOTORE)} snd ${par(P_SND)} pos ${par(0)}`);
}
console.log(`${ok} OK, ${bad} NO`);
process.exit(bad ? 1 : 0);
