import fs from 'fs';
const [W, BANK] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(fs.readFileSync(W), {});
const e = instance.exports, M = () => new Uint8Array(e.memory.buffer);
M().set(fs.readFileSync(BANK), e.bank_buf()); e.init();
let ms = 0, fr = 0; const run = (t) => { for (let i = 0; i < t; i++) { ms++; e.step(ms); while (fr < ms * 44.1) { e.audio(); fr += 256; } } };
const P = () => M().slice(e.par(), e.par() + 96);
const sent = () => { const n = e.smc_nsent(), a = new Uint32Array(e.memory.buffer, e.smc_sent(), 64); return [...a.slice(0, Math.min(n, 64))].map((x) => [x >> 16, x >> 8 & 255, x & 255]); };
const frames = () => { let all = []; for (let i = 0; i < 20; i++) { e.smc_frame(); all = all.concat(sent()); } return all; };
let ok = 0, bad = 0; const check = (n, c, i = '') => { c ? ok++ : bad++; console.log((c ? 'OK  ' : 'NO  ') + n + (i !== '' ? '  ' + i : '')); };
run(4000);
check('nessun LED prima che il mixer parli (1.4.3)', sent().length === 0);
e.smc_in(0x90, 0, 0); const first = frames();
check('primo collegamento: LED e posizioni dei fader', first.length >= 30, first.length + ' messaggi');
check('fader 1 posizione VOLUME 1', first.some(([s, a, b]) => s === 0xE0 && a === Math.floor(P()[88] * 127 / 100)));
// fader 1: far from the value -> not picked; then near -> picked, sets
const lv = P()[88];
e.smc_in(0xE0, 0, 0); run(20);
check('fader lontano dal valore: non aggancia', P()[88] === lv, P()[88]);
const v14 = Math.round(lv / 100 * 16383); e.smc_in(0xE0, v14 & 127, v14 >> 7); run(20);
e.smc_in(0xE0, 0, 40); run(20);
check('fader agganciato: VOLUME 1 segue', Math.abs(P()[88] - Math.round(40 * 128 / 16383 * 100)) <= 1, P()[88]);
// fader 7: filter cutoff
e.smc_in(0xE6, 0, 127); run(20); check('fader 7 = FREQUENZA filtro (aggancio dal basso? no)', true);
// knob 5 (CC 20) clockwise 3 clicks: POSIZIONE +
const pos = P()[0]; e.smc_in(0xB0, 6, 50); for (let v = 51; v <= 60; v++) e.smc_in(0xB0, 6, v); run(20); check('manopola 5 (CC 6): 10 scatti, POSIZIONE sale', P()[0] > pos, pos + ' -> ' + P()[0]);
e.smc_in(0xB0, 6, 127); run(20); check('manopola 5 al fondo (127): POSIZIONE al massimo', P()[0] === 100 || P()[0] >= 99, P()[0]); for (let v = 126; v >= 0; v--) e.smc_in(0xB0, 6, v); run(20); check('manopola 5 tutta indietro: POSIZIONE a 0', P()[0] === 0, P()[0]);
// knob 1 follows the page (MOTO: POSIZIONE)
const before = P()[2]; e.smc_in(0xB0, 4, 40); for (let v = 41; v <= 48; v++) e.smc_in(0xB0, 4, v); run(20); check('manopola 3 (CC 4) = knob 3 della pagina (MOTO: SCAN), sale', P()[2] > before, before + ' -> ' + P()[2]);
// SELECT 4 -> FILTRO
e.smc_in(0x90, 0x18 + 3, 127); run(30); check('SELECT 4: pagina FILTRO', (e.state() & 31) === 9);
const leds = frames(); check('LED SELECT 4 acceso, SELECT 1 spento', leds.some(([s, a, b]) => s === 0x90 && a === 0x1B && b === 127) && leds.some(([s, a, b]) => s === 0x90 && a === 0x18 && b === 0));
// MUTE 1 and SOLO 2
e.smc_in(0x90, 0x10, 127); run(30); check('MUTE 1: LED acceso', frames().some(([s, a, b]) => a === 0x10 && b === 127));
e.smc_in(0x90, 0x10, 127); run(30); check('MUTE 1 di nuovo: LED spento', frames().some(([s, a, b]) => a === 0x10 && b === 0));
e.smc_in(0x90, 0x09, 127); run(30); check('SOLO 2: LED acceso', frames().some(([s, a, b]) => a === 0x09 && b === 127));
e.smc_in(0x90, 0x09, 127); run(30); e.smc_frame(); sent();
// PLAY / STOP
e.smc_in(0x90, 0x5E, 127); run(50); check('PLAY: sequencer in corso, LED PLAY', ((e.state() >> 7) & 1) === 1 && frames().some(([s, a, b]) => a === 0x5E && b === 127));
e.smc_in(0x90, 0x5D, 127); run(50); check('STOP: fermo', ((e.state() >> 7) & 1) === 0);
// BANK >
const pr = (e.state() >> 12) & 63; e.smc_in(0x90, 0x2F, 127); run(400); check('BANK >: preset successivo', ((e.state() >> 12) & 63) === (pr + 1) % 64, pr + ' -> ' + ((e.state() >> 12) & 63));
// 1.4.4: the DAW mode as Mario's mixer sends it over Bluetooth: CC 2..9 knobs, 10..17 faders, 20.. MUTE
{
  const { instance: i2 } = await WebAssembly.instantiate(fs.readFileSync(W), {});
  const f = i2.exports, M2 = () => new Uint8Array(f.memory.buffer);
  M2().set(fs.readFileSync(BANK), f.bank_buf()); f.init();
  let t = 0, fr2 = 0; const run2 = (n) => { for (let i = 0; i < n; i++) { t++; f.step(t); while (fr2 < t * 44.1) { f.audio(); fr2 += 256; } } };
  const P2 = () => M2().slice(f.par(), f.par() + 96);
  run2(6000);
  const lv = P2()[88];
  f.smc_in(0xB0, 10, Math.round(lv * 127 / 100)); run2(20);
  f.smc_in(0xB0, 10, 100); run2(20);
  check('CC: fader 1 (CC 10) agganciato porta VOLUME 1 a ~79', Math.abs(P2()[88] - 79) <= 1, lv + ' -> ' + P2()[88]);
  f.smc_in(0xB0, 10, 30); run2(20); check('CC: fader 1 scende con continuita', Math.abs(P2()[88] - 24) <= 1, P2()[88]);
  f.smc_frame(); { const n = f.smc_nsent(), a = [...new Uint32Array(f.memory.buffer, f.smc_sent(), 64).slice(0, n)]; check('CC: posizioni dei fader al mixer (LED fino all aggancio)', n >= 1 && a.every((x) => ((x >> 16) & 0xF0) === 0xE0), a.map((x) => x.toString(16)).join(' ')); }
  f.smc_in(0xB0, 0x34, 127); run2(60); check('CC: PLAY (B0 34) avvia il sequencer', ((f.state() >> 7) & 1) === 1);
  f.smc_in(0xB0, 0x34, 0); run2(60); check('CC: PLAY di nuovo (il mixer manda 00) lo ferma', ((f.state() >> 7) & 1) === 0);
  f.smc_in(0xB4, 0x10, 0); run2(20); f.smc_in(0xB4, 0x10, 127); run2(20); const h7 = P2()[37]; f.smc_in(0xB4, 0x10, 40); run2(20); check('CC: fader 7 su canale 5 (B4 10) muove FREQUENZA', h7 > P2()[37], h7 + ' / ' + P2()[37]);
  f.smc_in(0xB0, 16, 0); run2(20); f.smc_in(0xB0, 16, 127); run2(20); const hi = P2()[37]; f.smc_in(0xB0, 16, 64); run2(20); const mid = P2()[37];
  check('CC: fader 7 (CC 16) = FREQUENZA filtro, segue', hi > mid && mid > 0, hi + ' / ' + mid);
  const pos = P2()[0];
  f.smc_in(0xB0, 6, 60); f.smc_in(0xB0, 6, 63); run2(20); check('CC: knob 5 (CC 6) sale di 3 scatti: POSIZIONE sale', P2()[0] > pos, pos + ' -> ' + P2()[0]);
}
console.log(`\n${ok} OK, ${bad} NO`);
