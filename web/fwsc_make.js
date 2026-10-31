/* SPDX-License-Identifier: GPL-3.0-only */
/* Build an FM-1 update package (.fwsc) in the browser: a line-by-line port of tools/fm1pkg_make.py
 * (Leo Kuroshita, Hügelton Instruments), checked byte for byte against it (web/test_fwsc.mjs).
 * The editor uses it to put GRANA's code and your sample bank into one package.
 *
 *   fwscMake({ app, ota, uboot, cfgTool, eq, product, key })  -> Uint8Array
 *   grana_app(appCode, bank)                                   -> the app with its bank at the next 4 KiB */
const FWSC = (() => {
  const APP_SLOT = 0x8DFBC, FLASH_SIZE = 0x93000, KEY = 0x980F, APP_XIP = 0x02000120;
  const FLASH_HDR = hex('20022f104cc900f00f001010a4ff4e5d0b41e0cd781dc856417fb367cf9f');
  const UFW_CHIP = ascii('AC791N');

  function hex(s) { const u = new Uint8Array(s.length / 2); for (let i = 0; i < u.length; i++) u[i] = parseInt(s.substr(2 * i, 2), 16); return u; }
  function ascii(s) { return Uint8Array.from(s, (c) => c.charCodeAt(0)); }
  function cat(...a) { const n = a.reduce((x, b) => x + b.length, 0), u = new Uint8Array(n); let o = 0; for (const b of a) { u.set(b, o); o += b.length; } return u; }

  const T = (() => {
    const t = new Uint16Array(256);
    for (let i = 0; i < 256; i++) { let c = i << 8; for (let k = 0; k < 8; k++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1); t[i] = c & 0xFFFF; }
    return t;
  })();
  function crc16(d, crc = 0) { for (let i = 0; i < d.length; i++) crc = ((crc << 8) & 0xFFFF) ^ T[((crc >> 8) ^ d[i]) & 0xFF]; return crc; }

  function enc(buf, off, size, key = 0xFFFF) {           /* JieLi ENC stream cipher, in place */
    for (let i = 0; i < size; i++) {
      buf[off + i] ^= key & 0xFF;
      key = ((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0)) & 0xFFFF;
    }
  }
  function sfc(buf, off, size, base, key) {              /* ENC per 32-byte block, key mixed with the address */
    for (let i = 0; i < size; i += 32) enc(buf, off + i, Math.min(size - i, 32), (key ^ ((off + i - base) >>> 2)) & 0xFFFF);
  }
  function chipkeyDecode(d) {
    let s = 0; for (let i = 0; i < 16; i++) s += d[i]; s &= 0xFF;
    s = s >= 0xE0 ? 0xAA : s <= 0x10 ? 0x55 : s;
    let k = 0; for (let i = 0; i < 16; i++) if ((d[16 + i] ^ d[15 - i]) < s) k |= 1 << i;
    return k;
  }

  function le(...fields) {                              /* [bytes, value] pairs, little endian */
    const n = fields.reduce((x, [b]) => x + b, 0), u = new Uint8Array(n); let o = 0;
    for (const [b, v] of fields) {
      if (v instanceof Uint8Array) { u.set(v.subarray(0, b), o); }
      else for (let i = 0; i < b; i++) u[o + i] = Number((BigInt(v) >> BigInt(8 * i)) & 0xFFn);
      o += b;
    }
    return u;
  }
  function entry(dcrc, offset, size, flags, resvd, index, name, fill = 0xFF) {
    let nm;
    if (name instanceof Uint8Array) nm = name;
    else { nm = new Uint8Array(16).fill(fill); nm.set(ascii(name)); nm[name.length] = 0; }
    const body = le([2, dcrc], [4, offset >>> 0], [4, size >>> 0], [1, flags], [1, resvd], [2, index], [16, nm]);
    return cat(le([2, crc16(body)]), body);
  }
  function keyBlob(key) {
    const d = new Uint8Array(32);
    d.fill(0x10, 0, 16);
    for (let i = 0; i < 16; i++) d[16 + i] = d[15 - i] ^ ((key >> i) & 1 ? 0x00 : 0xFF);
    if (chipkeyDecode(d) !== key) throw new Error('key blob');
    return cat(d, le([2, crc16(d)]));
  }

  function flashImage(app, key, uboot, cfgTool, eq) {
    const isd = cat(keyBlob(key), ascii('[FELUCCA]\r\n'));
    const f = new Uint8Array(FLASH_SIZE).fill(0xFF);
    const splOff = 0xA0, isdOff = splOff + uboot.length;
    if (isdOff + isd.length > 0x4000) throw new Error('SPL too large');
    f.set(uboot, splOff);
    f.set(isd, isdOff);
    const top = cat(entry(crc16(uboot), splOff, uboot.length, 0x00, 0x00, 0, 'uboot.boot'),
      entry(crc16(isd), isdOff, isd.length, 0x02, 0x80, 0, 'isd_config.ini'),
      entry(0xFFFF, 0x4000, 0xFFFFFFFF, 0x81, 0xFF, 0, 'app_dir_head'),
      entry(0xFFFF, 0xFF000, 0x1000, 0x12, 0x01, 0xFFFF, 'key_mac'));
    const head = cat(le([2, crc16(FLASH_HDR)]), FLASH_HDR, top);
    for (let o = 0; o < head.length; o += 32) enc(head, o, Math.min(32, head.length - o));
    f.set(head, 0);
    if (app.length > APP_SLOT) throw new Error(`il programma e la banca occupano ${app.length} B, lo spazio è di ${APP_SLOT} B`);
    const appP = new Uint8Array(APP_SLOT).fill(0xFF); appP.set(app);
    const blk = 0x120 + APP_SLOT + cfgTool.length;
    const area = new Uint8Array(blk).fill(0xFF);
    area.set(appP, 0x120);
    area.set(cfgTool, 0x120 + APP_SLOT);
    const ents = cat(entry(crc16(appP), 0x120, APP_SLOT, 0x82, 0xFF, 0, 'app.bin'),
      entry(crc16(cfgTool), 0x120 + APP_SLOT, cfgTool.length, 0x82, 0xFF, 0, 'cfg_tool.bin'),
      entry(0xFFFF, 0x93000, 352256, 0x12, 0x80, 0, hex('564d00ffffffffffffffffff00800000')),
      entry(0xFFFF, 0, FLASH_SIZE, 0x92, 0x82, 0, hex('5052435400ffffffffffffff434f4445')),
      entry(0xFFFF, 0xE9000, 0x1000, 0x92, 0x80, 0, hex('4254494600ffffffffffffff4155544f')),
      entry(0xFFFF, 0xEA000, 0x12000, 0x92, 0x81, 1, 'USR'));
    area.set(ents, 32);
    area.set(entry(crc16(area.subarray(32, blk)), 0x02000120, blk, 0x83, 0xFF, 0, 'app_area_head'), 0);
    const eqE = entry(crc16(eq), 0x40, eq.length, 0x82, 0xFF, 1, 'eq_cfg_hw.bin');
    const cfgBody = cat(eqE, eq);
    const cfg = cat(entry(crc16(cfgBody), 0x20, 0x20 + cfgBody.length, 0x83, 0xFF, 1, 'cfg'), cfgBody);
    const region = cat(area, cfg);
    sfc(region, 0, region.length, 0, key);
    if (0x4000 + region.length > FLASH_SIZE) throw new Error('app area too large');
    f.set(region, 0x4000);
    return f;
  }

  function ufw(flash, ota, product) {
    const files = [[0, 'flash.bin', flash], [100, 'ota.bin', ota]];
    let off = 0x400;
    const ents = [];
    files.forEach(([typ, name, d], i) => {
      off = (off + 0xFF) & ~0xFF;
      const e = new Uint8Array(0x50);
      e.set(le([2, typ], [2, i], [2, crc16(d)], [2, 0], [4, off], [4, d.length], [4, (d.length + 31) & ~31]), 0);
      e.set(ascii(name), 0x40);
      ents.push([e, off, d]);
      off += d.length;
    });
    const total = off;
    const hdr = new Uint8Array(0x40);
    hdr.set(le([4, total], [2, files.length]), 4);
    hdr.set(le([2, 0x0004], [2, 0x0200], [2, 0]), 10);
    hdr.set(UFW_CHIP, 16);
    const lst = new Uint8Array(0x50 * ents.length);
    ents.forEach(([e], i) => { const x = e.slice(); enc(x, 0, 0x50); lst.set(x, 0x50 * i); });
    hdr.set(le([2, crc16(lst)]), 2);
    hdr.set(le([2, crc16(hdr.subarray(2, 0x40))]), 0);
    enc(hdr, 0, 0x40);
    const logical = new Uint8Array(total).fill(0xFF);
    logical.set(hdr, 0);
    logical.set(lst, 0x40);
    for (const [, o, d] of ents) logical.set(d, o);
    if (product.length > 20 || [...product].some((c, i) => ((c.charCodeAt(0) + i + 1) & 0xFF) === 0x7D)) throw new Error('product');
    const raw = new Uint8Array(logical.length + 20);
    let w = 0;
    for (let i = 0; i < 20; i++) {
      const m = i < product.length ? (product.charCodeAt(i) + i + 1) & 0xFF : 0x7D;
      raw.set(logical.subarray(i * 0x2F, (i + 1) * 0x2F), w); w += 0x2F;
      raw[w++] = m;
    }
    raw.set(logical.subarray(20 * 0x2F), w);
    return raw;
  }

  /* the code alone (build/grana-app.bin) + a bank: the bank at the next 4 KiB of the XIP map, as tools/build.py */
  function granaApp(code, bank) {
    const pad = (4096 - ((APP_XIP + code.length) % 4096)) % 4096;
    const app = new Uint8Array(code.length + pad + bank.length).fill(0xFF);
    app.set(code, 0);
    app.set(bank, code.length + pad);
    return app;
  }
  function bankRoom(code) {                             /* the bytes a bank may take next to this code */
    const pad = (4096 - ((APP_XIP + code.length) % 4096)) % 4096;
    return APP_SLOT - code.length - pad;
  }

  function fwscMake({ app, ota, uboot, cfgTool, eq, product, key = KEY }) {
    return ufw(flashImage(app, key, uboot, cfgTool, eq), ota, product);
  }
  return { fwscMake, granaApp, bankRoom, crc16, APP_SLOT };
})();
if (typeof module !== 'undefined') module.exports = FWSC;
