// GRANA 1.2 in the browser build, driven like the editor: MIDI page, LEARN, clock in and out, TASTO
import fs from 'fs';
const W = process.argv[2], BANK = process.argv[3];
const { instance } = await WebAssembly.instantiate(fs.readFileSync(W), {});
const e = instance.exports, mem = () => new Uint8Array(e.memory.buffer);
const bank = fs.readFileSync(BANK); mem().set(bank, e.bank_buf());
e.init();
let ms = 0, frames = 0;
const BTN = { OCT_DOWN: 0, OCT_UP: 1, FX: 2, SEL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13 };
function run(t) {               // t ms: the main loop every ms, the audio as the time asks
  for (let i = 0; i < t; i++) { ms++; e.step(ms); while (frames < ms * 44.1) { e.audio(); frames += 256; } }
}
function press(b) { e.button(BTN[b], 1); run(30); e.button(BTN[b], 0); run(30); }
const par = (p) => mem()[e.par() + p];
const page = () => e.state() & 31;
let ok = 0, bad = 0;
function check(name, cond, info = '') { if (cond) ok++; else bad++; console.log((cond ? 'OK  ' : 'NO  ') + name + (info ? '  ' + info : '')); }
run(4000);                      // the opening
press('GLO'); press('GLO'); press('GLO'); press('GLO');
check('GLO x4: pagina MIDI', page() === 26, 'page ' + page());
if (process.env.OUT) fs.writeFileSync(process.env.OUT + '/midi_page.raw', Buffer.from(new Uint8Array(e.memory.buffer, e.fb(), 240 * 240 * 2)));
e.enc(4, 1); run(50);           // KNOB 3: LEARN on
check('LEARN acceso', e.dbg(5) === 1);
e.midi(0xB0, 21, 64); run(20);  // the controller's half first
press('FX');                    // FILTRO
check('FX: pagina FILTRO', page() === 9);
const before = par(37);
e.enc(3, 1); run(50);           // KNOB 2: FREQUENZA chosen, not moved
check('associazione salvata', e.dbg(4) === 1 && e.dbg(5) === 0, 'n ' + e.dbg(4));
check('il knob non ha cambiato il valore', par(37) === before);
e.midi(0xB0, 21, 127); run(20);
check('CC 21 = 127 -> FREQUENZA al massimo', par(37) === 200, 'v ' + par(37));
e.midi(0xB0, 21, 0); run(20);
check('CC 21 = 0 -> FREQUENZA a zero', par(37) === 0);
// a learnt note: FREEZE (choice) toggles, the note does not play
press('GLO'); press('GLO'); press('GLO'); press('GLO');   // FILTRO -> LIVELLI -> MIX -> MIDI
check('di nuovo pagina MIDI', page() === 26, 'page ' + page());
e.enc(4, 1); run(50);
press('LFO'); press('LFO'); press('LFO');   // LFO 1, LFO 2, SPECIALI
check('pagina SPECIALI', page() === 8);
e.enc(3, 1); run(50);           // FREEZE first this time
e.midi(0x99, 36, 100); run(20); // then the pad (channel 10)
check('nota imparata', e.dbg(4) === 2);
e.midi(0x89, 36, 0); run(20);
const fz = par(33);
e.midi(0x99, 36, 100); run(20); e.midi(0x89, 36, 0); run(20);
check('la nota accende/spegne FREEZE', par(33) !== fz, fz + ' -> ' + par(33));
// the fixed CC map still works for what is not learnt; the filter mode in the knob's order
e.midi(0xB0, 56, 127); run(20);
check('CC 56 = 127 -> MODO filtro NOTCH (5)', par(36) === 5, 'v ' + par(36));
e.midi(0xB0, 56, Math.round(5 * 127 / 6)); run(20);
check('CC 56 al 5/6 -> ALTO 24DB (6)', par(36) === 6, 'v ' + par(36));
// TASTO (1.6: TASTIERA, ENV twice, knob 2)
press('ENV'); press('ENV');
e.enc(3, 1); run(50);
check('TASTO = SPEGNE', par(35) === 1);
// clock out at the preset's tempo: CLOCK OUT on
press('GLO'); press('GLO'); press('GLO'); press('GLO');
e.enc(2, 1); run(50);           // 1.5: KNOB 1 CLOCK: INTERNO -> INTERNO+OUT
const c0 = e.clock_n(), bpm = par(28) + 20; run(5000);
const got = e.clock_n() - c0, want = bpm * 24 * 5 / 60;
check('CLOCK OUT: 24 per nota a ' + bpm + ' BPM', Math.abs(got - want) <= 2, got + ' / ' + want.toFixed(1));
// clock in: ESTERNO, START, 24 clocks a quarter at 100 BPM
e.enc(2, 1); run(50);           // KNOB 1: ESTERNO
check('CLOCK IN ESTERNO', par(28) >= 0 && e.dbg(1) === e.dbg(0));
e.midi(0xFA, 0, 0); run(1);
const per = 60000 / (100 * 24);
let tnext = ms;
for (let q = 0; q < 24 * 8; q++) { e.midi(0xF8, 0, 0); tnext += per; run(Math.max(0, Math.round(tnext - ms))); }
check('tempo misurato ~100 BPM', Math.abs(e.dbg(2) - 1000) <= 15, (e.dbg(2) / 10).toFixed(1));
check('i nostri tick seguono il clock', Math.abs(e.dbg(1) - e.dbg(0)) <= 8, e.dbg(0) + ' / ' + e.dbg(1));
check('START fa partire il sequencer', e.dbg(6) === 1);
run(1000);                      // the clock stops: we stop with it
const held = e.dbg(0); run(1000);
check('senza clock i tick si fermano', e.dbg(0) === held);
e.midi(0xFC, 0, 0); run(10);
check('STOP ferma il sequencer', e.dbg(6) === 0);
if (process.env.OUT) fs.writeFileSync(process.env.OUT + '/midi_page2.raw', Buffer.from(new Uint8Array(e.memory.buffer, e.fb(), 240 * 240 * 2)));
console.log(`\n${ok} OK, ${bad} NO`);
