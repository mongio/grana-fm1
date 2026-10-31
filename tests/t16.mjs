// GRANA 1.6 in the browser build: presets of 1.5 converted, TASTIERA, SCALA, 12 BIT, the user bank over SysEx, 11 kHz sounds
//   node tests/t16.mjs web/grana.wasm <1.5 grana.wasm> firmware/gen/bank_default.bin
import fs from 'fs';
const [W16, W15, BANK] = process.argv.slice(2);
const bank = fs.readFileSync(BANK);
async function boot(w, store) {
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(w), {});
  const e = instance.exports, mem = () => new Uint8Array(e.memory.buffer);
  mem().set(bank, e.bank_buf()); if (store) mem().set(store, e.store()); e.init(); return { e, mem };
}
let ok = 0, bad = 0;
const check = (n, c, i = '') => { c ? ok++ : bad++; console.log((c ? 'OK  ' : 'NO  ') + n + (i !== '' ? '  ' + i : '')); };
const fnv = (r, n) => { let h = 0x811c9dc5; for (let k = 0; k < n; k++) h = Math.imul(h ^ r[k], 16777619) >>> 0; return h; };
const seal = (r) => { const h = fnv(r, 240); r[240] = h & 255; r[241] = h >>> 8 & 255; r[242] = h >>> 16 & 255; r[243] = h >>> 24; };
const P_RETRIG = 14, P_TASTO = 35, P_SND = 40, P_VO1 = 88, P_DISP = 92, P_SCALA = 96, P_TONICA = 97, P_BIT12 = 98, P_COMP = 13;

// ---- 1.5 writes its presets (format 7)
const a = await boot(W15); a.e.presets_out();
const L = a.e.store_len(), st = a.mem().slice(a.e.store(), a.e.store() + L), U = 4 + 2048, rec = (i) => st.subarray(U + i * 256, U + (i + 1) * 256);
let r = rec(3);
check('record 1.5 in formato 7', r[1] === 7 && r[2] === 96);
r[16 + P_VO1] = 55; r[16 + P_DISP] = 1; r[16 + P_COMP] = 7; r[112 + 5] = 64; seal(r);

const b = await boot(W16, st);
const P = () => b.mem().slice(b.e.par(), b.e.par() + 100);
let ms = 0, fr = 0, blocks = [];
const run = (t) => { for (let i = 0; i < t; i++) { ms++; b.e.step(ms); while (fr < ms * 44.1) { const p = b.e.audio(); blocks.push(new Int32Array(b.e.memory.buffer, p, 512).slice()); if (blocks.length > 60) blocks.shift(); fr += 256; } } };
const btn = (id) => { b.e.button(id, 1); run(30); b.e.button(id, 0); run(30); };
const page = () => b.e.state() & 31;
const knob = (k, d) => { b.e.enc(2 + k, d); run(60); };
const BT = { FX: 2, SEL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8 };
run(4000);
b.e.midi(0xC0, 3, 0); run(400);
let p = P();
check('P4 della 1.5 convertito: VOLUME 1 55, SPLIT, COMPRESSORE 7 conservati', p[P_VO1] === 55 && p[P_DISP] === 1 && p[P_COMP] === 7, [p[P_VO1], p[P_DISP], p[P_COMP]].join(' '));
check('nuovi: SCALA cromatica, USCITA 12 BIT accesa', p[P_SCALA] === 0 && p[P_TONICA] === 0 && p[P_BIT12] === 1);
b.e.midi(0xC0, 0, 0); run(400);
check('preset di fabbrica: 12 BIT acceso', P()[P_BIT12] === 1);

// ---- pages: ENV twice, MIX, SPECIALI
btn(BT.ENV); check('ENV: FORMA', page() === 2, page());
btn(BT.ENV); check('ENV 2: TASTIERA', page() === 24, page());
const rt = P()[P_RETRIG], ta = P()[P_TASTO];
knob(0, 1); knob(1, 1);
check('TASTIERA: KNOB 1 RETRIG, KNOB 2 TASTO', P()[P_RETRIG] !== rt && P()[P_TASTO] !== ta, rt + '->' + P()[P_RETRIG] + ' ' + ta + '->' + P()[P_TASTO]);
knob(0, -1); knob(1, -1);
btn(BT.ENV); check('ENV 3: di nuovo FORMA', page() === 2, page());
btn(BT.GLO); btn(BT.GLO); btn(BT.GLO); check('GLO 3: MIX', page() === 3, page());
knob(2, -1); check('MIX: KNOB 3 = USCITA (16 BIT)', P()[P_BIT12] === 0);
knob(2, 1); check('MIX: di nuovo 12 BIT', P()[P_BIT12] === 1);

// ---- SCALA: the keys snap
btn(BT.ENV); btn(BT.ENV);
knob(2, 1); check('SCALA: MAGGIORE', P()[P_SCALA] === 1);
const notes = () => [10, 11, 12, 13].map((k) => b.e.dbg(k));
const playN = (n) => { b.e.midi(0x90, n, 100); run(80); const v = notes(); b.e.midi(0x80, n, 0); run(400); return v; };
let v = playN(61); check('C#4 in DO maggiore -> C4', v.includes(60) && !v.includes(61), v.join(' '));
v = playN(66); check('F#4 -> F4 (a pari distanza la nota sotto)', v.includes(65), v.join(' '));
v = playN(64); check('E4 resta E4', v.includes(64), v.join(' '));
knob(2, 1); knob(3, 2);                                   // MINORE, tonic D
check('SCALA MINORE, TONICA D', P()[P_SCALA] === 2 && P()[P_TONICA] === 2);
v = playN(63); check('D#4 in RE minore -> D4', v.includes(62), v.join(' '));
v = playN(70); check('A#4 (Sib) resta: è nella scala', v.includes(70), v.join(' '));
knob(2, -2); check('CROMATICA: nessun aggancio', P()[P_SCALA] === 0);
v = playN(61); check('C#4 resta C#4', v.includes(61), v.join(' '));

// ---- 12 BIT
b.e.master(32768);
b.e.midi(0x90, 60, 110); blocks = []; run(600);
let low = 0, n = 0; for (const bl of blocks) for (const x of bl) { if (x) { n++; if (x & 15) low++; } }
check('12 BIT: nessun campione con i 4 bit bassi', n > 1000 && low === 0, n + ' / ' + low);
btn(BT.GLO); while (page() !== 3) btn(BT.GLO);
knob(2, -1); blocks = []; run(600);
low = 0; n = 0; for (const bl of blocks) for (const x of bl) { if (x) { n++; if (x & 15) low++; } }
check('16 BIT: i bit bassi ci sono', n > 1000 && low > n / 4, n + ' / ' + low);
knob(2, 1);
b.e.midi(0x80, 60, 0); run(2500);

// ---- the user bank over SysEx: a sound at 22 kHz and the same at 11 kHz
const STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767];
const IDXT = [-1, -1, -1, -1, 2, 4, 6, 8];
function enc(pcm) { const out = new Uint8Array((pcm.length + 1) >> 1); let pred = 0, idx = 0; for (let i = 0; i < pcm.length; i++) { const s = STEP[idx]; let d = pcm[i] - pred, c = 0, df = s >> 3; if (d < 0) { c = 8; d = -d; } if (d >= s) { c |= 4; d -= s; df += s; } if (d >= s >> 1) { c |= 2; d -= s >> 1; df += s >> 1; } if (d >= s >> 2) { c |= 1; df += s >> 2; } pred = c & 8 ? pred - df : pred + df; pred = Math.max(-32768, Math.min(32767, pred)); idx = Math.max(0, Math.min(88, idx + IDXT[c & 7])); out[i >> 1] |= c << ((i & 1) * 4); } return out; }
function dec(data, n) { const out = new Int16Array(n); let pred = 0, idx = 0; for (let i = 0; i < n; i++) { const c = (data[i >> 1] >> ((i & 1) * 4)) & 15, s = STEP[idx]; let df = s >> 3; if (c & 4) df += s; if (c & 2) df += s >> 1; if (c & 1) df += s >> 2; pred = c & 8 ? pred - df : pred + df; pred = Math.max(-32768, Math.min(32767, pred)); idx = Math.max(0, Math.min(88, idx + IDXT[c & 7])); out[i] = pred; } return out; }
const crcT = new Uint32Array(256).map((_, i) => { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xEDB88320 : c >>> 1; return c >>> 0; });
const crc32 = (u) => { let c = 0xFFFFFFFF; for (const x of u) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
function build(es) { const head = 16 + 24 * es.length; let size = head; for (const e of es) size += e.data.length + (e.data.length & 1); const bb = new Uint8Array(size), dv = new DataView(bb.buffer); let off = head; es.forEach((e, i) => { const o = 16 + 24 * i; for (let k = 0; k < e.name.length; k++) bb[o + k] = e.name.charCodeAt(k); dv.setUint32(o + 12, off, true); dv.setUint32(o + 16, e.frames, true); bb[o + 20] = 0; bb[o + 21] = e.rate; bb.set(e.data, off); off += e.data.length + (e.data.length & 1); }); bb.set([0x47, 0x52, 0x4E, 0x42]); dv.setUint16(4, 1, true); dv.setUint16(6, es.length, true); dv.setUint32(8, size, true); dv.setUint32(12, crc32(bb.subarray(16)), true); return bb; }
const bv = new DataView(bank.buffer, bank.byteOffset), off0 = bv.getUint32(16 + 12, true), fr0 = bv.getUint32(16 + 16, true);
const pcm22 = dec(bank.subarray(off0), fr0), pcm11 = new Int16Array(fr0 >> 1).map((_, i) => (pcm22[2 * i] + pcm22[2 * i + 1]) >> 1);
const ub = build([{ name: 'UTENTE 22', frames: fr0, rate: 0, data: enc(pcm22) }, { name: 'UTENTE 11', frames: pcm11.length, rate: 1, data: enc(pcm11) }]);
const sx = (bytes) => { const m = new Uint8Array(b.e.memory.buffer, b.e.sx_in(), 1024); m.set([0x7D, 0x47, 0x52, ...bytes]); b.e.sx_feed(3 + bytes.length); run(5); };
const replies = () => { const n = b.e.sx_out_len(), o = new Uint8Array(b.e.memory.buffer, b.e.sx_out(), n).slice(), out = []; let s = -1; for (let i = 0; i < n; i++) { if (o[i] === 0xF0) s = i; if (o[i] === 0xF7 && s >= 0) { out.push(o.slice(s, i + 1)); s = -1; } } return out; };
replies();
check('prima: 8 suoni di fabbrica, nessuno utente', (b.e.dbg(15) & 255) === 8 && (b.e.dbg(15) >> 8) === 8, (b.e.dbg(15) & 255) + ' / ' + (b.e.dbg(15) >> 8));
sx([0x10]); let rr = replies();
check('INIZIO: risposta 11', rr.length === 1 && rr[0][4] === 0x11);
const nb = Math.ceil(ub.length / 256);
let acks = 0;
for (let k = 0; k < nb; k++) {
  const m = [0x12, k >> 14 & 127, k >> 7 & 127, k & 127];
  for (let j = 0; j < 256; j++) { const x = 256 * k + j < ub.length ? ub[256 * k + j] : 0xFF; m.push(x >> 4, x & 15); }
  sx(m);
  rr = replies(); if (rr.length === 1 && rr[0][4] === 0x13 && rr[0][7] === (k & 127)) acks++;
}
check(`blocchi confermati ${nb}`, acks === nb, acks);
sx([0x14]); rr = replies();
check('FINE: 8 di fabbrica + 2 utente', rr.length === 1 && rr[0][4] === 0x15 && rr[0][5] === 8 && rr[0][6] === 2, rr[0] ? [...rr[0]].join(' ') : '');
check('lista dei suoni: 10', (b.e.dbg(15) >> 8) === 10);
sx([0x18]); rr = replies(); check('domanda 18 -> 19 8 2', rr[0] && rr[0][4] === 0x19 && rr[0][5] === 8 && rr[0][6] === 2);
// play them: the 11 kHz one at half the step
const playSnd = (s) => {
  b.e.midi(0xC0, 0, 0); run(300);
  btn(BT.HOME); while (page() !== 10) btn(BT.HOME);
  const cur = P()[P_SND]; knob(0, s - cur); run(800);
  b.e.midi(0x90, 60, 110); blocks = []; run(500);
  let e2 = 0, cnt = 0; for (const bl of blocks) for (const x of bl) { e2 += x * x; cnt++; }
  const stp = b.e.dbg(14) & 0xFFFFFF;
  b.e.midi(0x80, 60, 0); run(2500);
  return [P()[P_SND], Math.sqrt(e2 / cnt), stp];
};
const [s22, en22, st22] = playSnd(8), [s11, en11, st11] = playSnd(9);
check('suono utente 22 kHz (il 9°) suona', s22 === 8 && en22 > 300, s22 + ' ' + Math.round(en22));
check('suono utente 11 kHz (il 10°) suona', s11 === 9 && en11 > 300, s11 + ' ' + Math.round(en11));
check('a 11 kHz il passo dei grani è la metà', st22 > 0 && Math.abs(st11 * 2 - st22) <= st22 * 0.03, st22 + ' / ' + st11);
// a transfer cut in the middle: only the factory bank
sx([0x10]); replies();
check('durante l’invio suona solo la banca di fabbrica', (b.e.dbg(15) >> 8) === 8);
sx([0x12, 0, 0, 0, ...new Array(512).fill(0)]); replies();
sx([0x14]); rr = replies();
check('invio interrotto: la banca utente non vale, restano gli 8 di fabbrica', rr[0] && rr[0][6] === 0 && (b.e.dbg(15) >> 8) === 8);
sx([0x16, 0x0E, 0x50, 0]); rr = replies();   // 0xEA000 / 256 = 0xEA0: 0x0E 0x50? (0xEA0 >> 7 = 29, & 127 = 32)
sx([0x16, 0, 29, 32]); rr = replies();
check('lettura della zona USR (simulatore: vuota)', rr.some((x) => x[4] === 0x17 && x.length === 9 + 512));
console.log(`\n${ok} OK, ${bad} NO`);
