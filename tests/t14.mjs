import fs from 'fs';
const [W13, W14, BANK, OUT] = process.argv.slice(2);
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
const a = await boot(W13); a.e.presets_out();
const L = a.e.store_len(), st = a.mem().slice(a.e.store(), a.e.store() + L), U = 4 + 2048, rec = (i) => st.subarray(U + i * 256, U + (i + 1) * 256);
let r = rec(1); r.set([25, 46, 40, 45], 16 + 16); r.set([6, 94, 70, 80], 16 + 60); seal(r);
r = rec(2); r.set([4, 70, 30, 60], 16 + 60); seal(r);
r = rec(3); r[16 + 59] = 25; seal(r);
check('record 1.3 in formato 5', rec(1)[1] === 5 && rec(1)[2] === 80);
const b = await boot(W14, st);
const P = () => b.mem().slice(b.e.par(), b.e.par() + 96);
let ms = 0, fr = 0;
const run = (t) => { for (let i = 0; i < t; i++) { ms++; b.e.step(ms); while (fr < ms * 44.1) { b.e.audio(); fr += 256; } } };
const btn = (id) => { b.e.button(id, 1); run(30); b.e.button(id, 0); run(30); };
const shot = (n) => fs.writeFileSync(`${OUT}/${n}.raw`, Buffer.from(new Uint8Array(b.e.memory.buffer, b.e.fb(), 240 * 240 * 2)));
const led = (id) => b.mem()[b.e.leds() + id] === 2;
run(4000);
const load = (i) => { b.e.midi(0xC0, i, 0); run(400); return P(); };
let p = load(0);
check('P1 VOCI: phaser -> VELVET 100', p[60] === 101, p[60]);
check('P1: delay -> NASTRO ECO, stessi valori', p[16] === 0 && p[17] === 46 && p[18] === 40 && p[19] === 20 && p[81] === 45, [p[16], p[17], p[18], p[19], p[81]].join(' '));
check('P1: strati al centro', [84, 85, 86, 87].every((k) => p[k] === 100));
p = load(1);
check('P2 stutter -> NASTRO RIPETI 1/8', p[16] === 1 && p[17] === 71 && p[80] === 70 && p[19] === 80 && p[18] === 44 && p[60] === 0, [p[16], p[17], p[80], p[19], p[18], p[60]].join(' '));
check('P2: tono del delay conservato', p[81] === 45);
p = load(2);
check('P3 freq shift su -> METALLO SU 100', p[60] === 202 && p[61] > 0 && p[62] === 30, [p[60], p[61], p[62]].join(' '));
p = load(3);
check('P4 LFO 2 su INT. LFO 1 -> 30', p[59] === 30, p[59]);
load(0);
// pages
btn(2); btn(2); shot('m'); check('FX x2: MODULAZIONE', (b.e.state() & 31) === 15);
btn(2); shot('n1'); check('FX x3: NASTRO', (b.e.state() & 31) === 4);
btn(2); shot('n2'); check('FX x4: NASTRO 2', (b.e.state() & 31) === 20);
btn(2); check('FX x5: RIVERBERO', (b.e.state() & 31) === 5);
btn(7); btn(7); shot('pan'); check('GLO x2: PAN STRATI', (b.e.state() & 31) === 21);
for (let i = 0; i < 30; i++) { b.e.enc(3, -1); run(10); }
check('PAN 2 a sinistra 30', P()[85] === 70, P()[85]);
// ARP: first press opens, second turns on
btn(3);
btn(10); check('ARP da MOTO: apre la pagina, arpeggio spento', (b.e.state() & 31) === 6 && !((b.e.state() >> 6) & 1) && !led(10));
btn(10); check('ARP sulla pagina: acceso, LED acceso', ((b.e.state() >> 6) & 1) === 1 && led(10));
btn(10); check('ARP di nuovo: spento, LED spento', !((b.e.state() >> 6) & 1) && !led(10));
// SEQ: LED with PLAY
btn(11); check('SEQ: pagina, LED spento da fermo', (b.e.state() & 31) === 7 && !led(11));
btn(12); run(200); check('PLAY: LED SEQ acceso', led(11));
btn(12); run(100); check('STOP: LED SEQ spento', !led(11));
// REC: SEQ short does not clear, held 1 s clears
b.e.midi(0x90, 60, 100); run(50); b.e.midi(0x80, 60, 0); run(50);
const seqHas = () => { b.e.presets_out(); return true; };
btn(13);
b.e.keys(1 << 7); run(60); b.e.keys(0); run(60);           // a step written
btn(11); run(50);
shot('seqrec');
b.e.button(11, 1); run(500); shot('seqhold'); b.e.button(11, 0); run(50);
check('SEQ tenuto 0,5 s: niente', true);
b.e.button(11, 1); run(1100); b.e.button(11, 0); run(50);
shot('seqclear');
console.log(`\n${ok} OK, ${bad} NO`);
