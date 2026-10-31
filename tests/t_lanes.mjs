import fs from 'fs';
const [W, BANK, OUT] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(fs.readFileSync(W), {});
const e = instance.exports, mem = () => new Uint8Array(e.memory.buffer);
mem().set(fs.readFileSync(BANK), e.bank_buf()); e.init();
let ms = 0, frames = 0;
function run(t) { for (let i = 0; i < t; i++) { ms++; e.step(ms); while (frames < ms * 44.1) { e.audio(); frames += 256; } } }
const shot = (n) => fs.writeFileSync(`${OUT}/${n}.raw`, Buffer.from(new Uint8Array(e.memory.buffer, e.fb(), 240 * 240 * 2)));
const par = (p) => mem()[e.par() + p];
run(4000);
e.midi(0x90, 60, 100); run(600); shot('lane1');
e.midi(0xB0, 71, 127 * 3 / 8 | 0); run(300);        // STRATO 2: a sound
e.midi(0xB0, 72, 127 * 6 / 8 | 0); run(800); shot('lane3');
e.midi(0xB0, 73, 127 * 2 / 8 | 0); e.button(11, 1); run(40); e.button(11, 0); e.button(12, 1); run(40); e.button(12, 0); run(1500); shot('lane4seq');
e.midi(0x80, 60, 0); run(200);
// TASTO: HOLD on, a key twice
e.button(12, 1); run(40); e.button(12, 0); run(40);   // sequencer off
e.button(8, 1); run(700); e.button(8, 0); run(50);    // HOME held: HOLD
e.button(5, 1); run(30); e.button(5, 0); e.button(5, 1); run(30); e.button(5, 0); e.button(5, 1); run(30); e.button(5, 0); run(30);
e.enc(5, 1); run(50);
console.log('TASTO', par(35));
const lit = () => mem()[e.leds() + 14 + 7];           // key 7 = C4 at octave 0
e.keys(1 << 7); run(100); e.keys(0); run(300);
console.log('after 1st press (held):', lit());
e.keys(1 << 7); run(100); e.keys(0); run(300);
console.log('after 2nd press (released):', lit());
