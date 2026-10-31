// node web/test_fwsc.mjs: the browser's package builder against tools/fm1pkg_make.py (build/grana-<v>.fwsc)
import fs from 'fs';
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const F = require('./fwsc_make.js');
const [, , ref, product] = process.argv;
const sdk = process.env.AC79_SDK + '/cpu/wl82/tools/';
const code = fs.readFileSync(process.argv[4] || 'build/grana-app.bin'), bank = fs.readFileSync('firmware/gen/bank_default.bin');
const app = F.granaApp(code, bank);
const same = Buffer.compare(Buffer.from(app), fs.readFileSync('build/grana.bin')) === 0;
const pkg = F.fwscMake({ app, ota: fs.readFileSync('build/loader/ota.bin'), uboot: fs.readFileSync(sdk + 'uboot.boot'),
  cfgTool: fs.readFileSync(sdk + 'cfg_tool.bin'), eq: fs.readFileSync(sdk + 'cfg/eq_cfg_hw.bin'), product });
const r = fs.readFileSync(ref);
let diff = -1; for (let i = 0; i < Math.max(r.length, pkg.length); i++) if (r[i] !== pkg[i]) { diff = i; break; }
console.log('app.bin identical:', same, '| package', pkg.length, 'B vs', r.length, 'B |', diff < 0 ? 'IDENTICAL' : 'first difference at ' + diff);
console.log('bank room', F.bankRoom(code), 'B');
process.exit(diff < 0 && same ? 0 : 1);
