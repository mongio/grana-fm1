import fs from 'fs';
const [W12, W13, BANK, OUT] = process.argv.slice(2);
const bank = fs.readFileSync(BANK);
async function boot(w, store) {
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(w), {});
  const e = instance.exports, mem = () => new Uint8Array(e.memory.buffer);
  mem().set(bank, e.bank_buf());
  if (store) mem().set(store, e.store());
  e.init();
  return { e, mem };
}
let ok = 0, bad = 0;
const check = (n, c, i = '') => { c ? ok++ : bad++; console.log((c ? 'OK  ' : 'NO  ') + n + (i ? '  ' + i : '')); };
// a 1.2 store: factory presets written out, P5 given LFO 2 -> INT. LFO 1 (21) and a name
const a = await boot(W12);
a.e.presets_out();
const L = a.e.store_len(), st = a.mem().slice(a.e.store(), a.e.store() + L);
const U = 4 + 2048, rec = (i) => st.subarray(U + i * 256, U + (i + 1) * 256);
const fnv = (r, n) => { let h = 0x811c9dc5; for (let k = 0; k < n; k++) h = Math.imul(h ^ r[k], 16777619) >>> 0; return h; };
const r4 = rec(4); r4[16 + 59] = 21; r4.set([...Buffer.from('PROVA')], 4);
let h = fnv(r4, 216); r4[216] = h & 255; r4[217] = h >>> 8 & 255; r4[218] = h >>> 16 & 255; r4[219] = h >>> 24;
check('record 1.2 in formato 4', r4[1] === 4 && r4[2] === 68);
const b = await boot(W13, st);
const par = () => b.mem().subarray(b.e.par(), b.e.par() + 80);
let ms = 0, fr = 0;
const run = (t) => { for (let i = 0; i < t; i++) { ms++; b.e.step(ms); while (fr < ms * 44.1) { b.e.audio(); fr += 256; } } };
run(4000);
b.e.midi(0xC0, 4, 0); run(400);
const p = par();
check('P5 convertito: LFO 2 su INT. LFO 1 (25)', p[59] === 25, 'L2T ' + p[59]);
check('P5: strati come prima', [68, 69, 70, 71].every((k) => p[k] === 120) && [72, 76].every((k) => p[k] === 100));
check('P5: resto invariato', p[36] === r4[16 + 36] && p[40] === r4[16 + 40] && p[28] === r4[16 + 28]);
b.e.presets_out();
const s2 = b.mem().subarray(b.e.store() + U + 4 * 256, b.e.store() + U + 5 * 256);
check('riscritto in formato 5 con il nome', s2[1] === 5 && s2[2] === 80 && String.fromCharCode(...s2.subarray(4, 9)) === 'PROVA');
// layer pages: HOME short x2 -> PITCH STRATI, then knob 2 (+1 slow, then fast)
b.e.midi(0xB0, 71, 40); run(300);                  // STRATO 2 on
const btn = (id) => { b.e.button(id, 1); run(30); b.e.button(id, 0); run(30); };
btn(8); btn(8);
check('HOME, HOME: PITCH STRATI', (b.e.state() & 31) === 17, 'page ' + (b.e.state() & 31));
for (let i = 0; i < 7; i++) { b.e.enc(3, 1); run(20); }
check('PITCH 2: +7 cent', par()[69] === 127, 'v ' + par()[69]);
for (let i = 0; i < 10; i++) { b.e.enc(3, 1); run(20); }
fs.writeFileSync(`${OUT}/p13a.raw`, Buffer.from(new Uint8Array(b.e.memory.buffer, b.e.fb(), 240 * 240 * 2)));
btn(8);
for (let i = 0; i < 40; i++) { b.e.enc(3, -1); run(10); }                          // INIZIO 2: -20 %
btn(8);
for (let i = 0; i < 50; i++) { b.e.enc(3, -1); run(10); } run(300);                         // AREA 2: 50 %
fs.writeFileSync(`${OUT}/p13b.raw`, Buffer.from(new Uint8Array(b.e.memory.buffer, b.e.fb(), 240 * 240 * 2)));
check('INIZIO 2 e AREA 2', par()[73] === 60 && par()[77] === 50, par()[73] + ' ' + par()[77]);
// MODULAZIONE: STUTTER, SYNC 1/16
btn(2); btn(2);                                     // FILTRO, MODULAZIONE
check('pagina MODULAZIONE', (b.e.state() & 31) === 15);
for (let i = 0; i < 6; i++) { b.e.enc(2, 1); run(10); } for (let i = 0; i < 100; i++) { b.e.enc(3, 1); run(10); } for (let i = 0; i < 4; i++) { b.e.enc(3, -1); run(10); } run(60);
fs.writeFileSync(`${OUT}/p13c.raw`, Buffer.from(new Uint8Array(b.e.memory.buffer, b.e.fb(), 240 * 240 * 2)));
check('STUTTER a tempo', par()[60] === 6 && par()[61] === 96, par()[60] + ' ' + par()[61]);
console.log(`\n${ok} OK, ${bad} NO`);
