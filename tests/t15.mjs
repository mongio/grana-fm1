// GRANA 1.5 in the browser build: presets of 1.4 converted, the pages, VOLUMI, SPLIT, RICEVI, the header
//   node tests/t15.mjs web/grana.wasm <1.4.8 grana.wasm> firmware/gen/bank_default.bin <out dir for screens>
import fs from 'fs';
const [W15, W14, BANK, OUT] = process.argv.slice(2);
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
const P_SCAN = 2, P_AREA = 1, P_SALTO = 49, P_FCUT = 37, P_FRES = 38, P_L1R = 53, P_L1D = 54, P_L2R = 57, P_L2D = 58;
const P_LV1 = 44, P_LS1 = 72, P_LA1 = 76, P_NCASO = 83, P_VO1 = 88, P_DISP = 92, P_SP1 = 93, P_SND2 = 41, P_NMODE = 16;

// ---- 1.4.8 writes its presets (format 6), one changed by hand
const a = await boot(W14); a.e.presets_out();
const L = a.e.store_len(), st = a.mem().slice(a.e.store(), a.e.store() + L), U = 4 + 2048, rec = (i) => st.subarray(U + i * 256, U + (i + 1) * 256);
let r = rec(5);
check('record 1.4 in formato 6', r[1] === 6 && r[2] === 88);
r[16 + P_SCAN] = 8; r[16 + P_AREA] = 25; r[16 + P_SALTO] = 16; r[16 + P_FCUT] = 127; r[16 + P_FRES] = 50;
r[16 + P_L1R] = 40; r[16 + P_L1D] = 125; r[16 + P_L2R] = 66; r[16 + P_L2D] = 64;
for (let k = 0; k < 12; k++) r[4 + k] = 0;
'SEI BELLO'.split('').forEach((c, k) => { r[4 + k] = c.charCodeAt(0); });
seal(r);

// ---- 1.5 reads them
const b = await boot(W15, st);
const P = () => b.mem().slice(b.e.par(), b.e.par() + 96);
let ms = 0, fr = 0, lastAudio = [];
const run = (t) => { for (let i = 0; i < t; i++) { ms++; b.e.step(ms); while (fr < ms * 44.1) { const p = b.e.audio(); lastAudio.push(new Int32Array(b.e.memory.buffer, p, 512).slice()); if (lastAudio.length > 40) lastAudio.shift(); fr += 256; } } };
const energy = (t) => { lastAudio = []; run(t); let s = 0, n = 0; for (const blk of lastAudio) for (const v of blk) { s += v * v; n++; } return Math.sqrt(s / Math.max(1, n)); };
const btn = (id) => { b.e.button(id, 1); run(30); b.e.button(id, 0); run(30); };
const page = () => b.e.state() & 31;
const shot = (n) => OUT && fs.writeFileSync(`${OUT}/${n}.raw`, Buffer.from(new Uint8Array(b.e.memory.buffer, b.e.fb(), 240 * 240 * 2)));
const set = (p, v) => { const m = b.mem(); m[b.e.par() + p] = v; };
const knob = (k, d) => { b.e.enc(2 + k, d); run(60); };     // KNOB 1..4 are encoders 2..5
const BT = { FX: 2, SEL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, ARP: 10, SEQ: 11, PLAY: 12, REC: 13 };
run(4000);
b.e.midi(0xC0, 5, 0); run(400);
let p = P();
check('P6 convertito: SCAN 8 (40%) -> 40', p[P_SCAN] === 40, p[P_SCAN]);
check('AREA 25% -> 100 (25% al quadrato)', p[P_AREA] === 100, p[P_AREA]);
check('SALTO 16% -> 40', p[P_SALTO] === 40, p[P_SALTO]);
check('FREQUENZA 127 -> 200, RISONANZA 50 -> 100', p[P_FCUT] === 200 && p[P_FRES] === 100, p[P_FCUT] + ' ' + p[P_FRES]);
check('LFO 1 VEL. 40 -> 80, INT. +25 -> +50 (25%)', p[P_L1R] === 80 && p[P_L1D] === 150, p[P_L1R] + ' ' + p[P_L1D]);
check('LFO 2 a tempo 66 (1/1) -> 129, INT. -36 -> -60', p[P_L2R] === 129 && p[P_L2D] === 40, p[P_L2R] + ' ' + p[P_L2D]);
check('volumi 100, sovrapposti, split C4 F#4 C5', [0, 1, 2, 3].every((k) => p[P_VO1 + k] === 100) && p[P_DISP] === 0 &&
      p[P_SP1] === 60 && p[P_SP1 + 1] === 66 && p[P_SP1 + 2] === 72);
check('RIP. A CASO spento', p[P_NCASO] === 0);
shot('h_header');

// ---- pages: HOME and GLO
btn(BT.HOME); check('HOME: STRATI', page() === 10, page());
btn(BT.HOME); check('HOME 2: SPLIT', page() === 23, page());
btn(BT.HOME); check('HOME 3: QUANTITA\'', page() === 11, page());
btn(BT.HOME); check('HOME 4: PITCH STRATI', page() === 17, page());
btn(BT.HOME); check('HOME 5: INI/AREA 1-2', page() === 18, page());
const la1 = P()[P_LA1], ls2 = P()[P_LS1 + 1];
knob(1, 1); knob(2, -1);
check('INI/AREA 1-2: KNOB 2 = AREA 1, KNOB 3 = INIZIO 2', P()[P_LA1] === la1 + 1 && P()[P_LS1 + 1] === ls2 - 1, P()[P_LA1] + ' ' + P()[P_LS1 + 1]);
shot('h_iniarea12');
btn(BT.HOME); check('HOME 6: INI/AREA 3-4', page() === 19, page());
const la4 = P()[P_LA1 + 3]; knob(3, 1);
check('INI/AREA 3-4: KNOB 4 = AREA 4', P()[P_LA1 + 3] === la4 + 1);
btn(BT.HOME); check('HOME 7: di nuovo STRATI', page() === 10, page());
btn(BT.GLO); check('GLO: VOLUMI', page() === 22, page());
shot('h_volumi');
btn(BT.GLO); check('GLO 2: PAN STRATI', page() === 21, page());
btn(BT.GLO); check('GLO 3: MIX', page() === 3, page());
btn(BT.GLO); check('GLO 4: MIDI', page() === 25, page());
shot('h_midi');
btn(BT.GLO); check('GLO 5: di nuovo VOLUMI', page() === 22, page());
{   // SELECT through every page: 22 of them, back where it started
  const seen = new Set();
  for (let i = 0; i < 23; i++) { b.e.enc(0, 1); run(80); seen.add(page()); }
  check('SELECT: 23 pagine, si torna a VOLUMI', seen.size === 23 && page() === 22, seen.size + ' / ' + page());
}

// ---- VOLUMI: the layer's own volume
b.e.midi(0xC0, 0, 0); run(400);
set(P_SND2, 0); b.e.midi(0xB0, 20, 64); run(300);             // one layer
b.e.midi(0x90, 60, 100);
const e1 = energy(800);
btn(BT.FX); btn(BT.GLO); knob(0, -100); run(200);   // VOLUME 1 to OFF
run(3000); const g0 = b.e.dbg(8), e0 = energy(800);
check('VOLUME 1 a OFF: nessun grano, resta solo la coda degli effetti', e1 > 300 && g0 === 0 && e0 < e1 / 10, Math.round(e1) + ' -> ' + Math.round(e0) + ' grani ' + g0);
knob(0, 100); run(200);
const e2 = energy(800);
check('VOLUME 1 di nuovo 100: suona', e2 > e1 / 3, Math.round(e2));
b.e.midi(0x80, 60, 0); run(1500);

// ---- SPLIT: two layers, the keyboard in two
b.e.midi(0xB0, 71, Math.round(127 * 3 / 8)); run(800);       // STRATO 2: a sound
btn(BT.HOME); while (page() !== 23) btn(BT.HOME);
knob(0, 1);
check('DISPOSIZIONE SPLIT, due zone', P()[P_DISP] === 1 && b.e.dbg(9) === 2, P()[P_DISP] + ' ' + b.e.dbg(9));
shot('h_split');
b.e.midi(0x90, 48, 100); run(600);
const lo = b.e.dbg(8);
b.e.midi(0x80, 48, 0); run(2500);
b.e.midi(0x90, 72, 100); run(600);
const hi = b.e.dbg(8);
b.e.midi(0x80, 72, 0); run(2500);
check('nota sotto lo split: solo strato 1; sopra: solo strato 2', lo === 1 && hi === 2, lo + ' / ' + hi);
knob(1, 1);                                                   // SPLIT 1 turned: armed
check('SPLIT 1 girato: la striscia chiede un tasto', true);
shot('h_split_arm');
b.e.keys(1 << 10); run(80); b.e.keys(0); run(80);             // key 10 = F3 + 10 = D#4 (63) at octave 0
check('tasto premuto: SPLIT 1 = D#4 (63)', P()[P_SP1] === 63, P()[P_SP1]);
shot('h_split_key');
b.e.midi(0x90, 62, 100); run(600);
const z62 = b.e.dbg(8); b.e.midi(0x80, 62, 0); run(2500);
b.e.midi(0x90, 64, 100); run(600);
const z64 = b.e.dbg(8); b.e.midi(0x80, 64, 0); run(2500);
check('D4 zona 1, E4 zona 2', z62 === 1 && z64 === 2, z62 + ' / ' + z64);
knob(0, -1);
b.e.midi(0x90, 48, 100); run(300);
let both = 0; for (let i = 0; i < 30; i++) { run(30); both |= b.e.dbg(8); } b.e.midi(0x80, 48, 0); run(2500);
check('SOVRAPPOSTI: tutti e due gli strati', both === 3, both);

// ---- RICEVI
btn(BT.GLO); while (page() !== 25) btn(BT.GLO);
knob(1, 1);
check('RICEVI: SOLO CLOCK', b.e.dbg(7) === 1);
b.e.midi(0x90, 60, 100); const q1 = energy(600); b.e.midi(0x80, 60, 0); run(2000);
check('SOLO CLOCK: la nota non suona', q1 < 50, Math.round(q1));
knob(1, 2);
check('RICEVI: CANALE 2', b.e.dbg(7) === 3);
shot('h_ricevi');
b.e.midi(0x90, 60, 100); const q2 = energy(600); b.e.midi(0x80, 60, 0); run(2000);
b.e.midi(0x91, 60, 100); const q3 = energy(600); b.e.midi(0x81, 60, 0); run(2000);
check('CANALE 2: canale 1 ignorato, canale 2 suona', q2 < 50 && q3 > 300, Math.round(q2) + ' / ' + Math.round(q3));
knob(1, -10);
check('RICEVI: TUTTO', b.e.dbg(7) === 0);

// ---- the header: a long preset name and a sound name do not overlap (drawn: checked on the screens)
b.e.midi(0xC0, 5, 0); run(600); shot('h_header2');
console.log(`\n${ok} OK, ${bad} NO`);
