"""Minimal FM-1 .fwsc reader for FM-1 Transporter's `write` (a stand-in for fm-1-research-lab's fm1fw).

A port of Felucca's web/fm1pkg.js (Package.parseUfw): the 20 marker bytes come out, the UFW header and its
entry list are decrypted (JieLi ENC), and every CRC is checked. Only what fm1t.py uses is provided:
Firmware(raw).raw, .skew, .product and .entries[{type, data_off, size, name}]."""

BLOCKS, BLK, KEEP = 20, 0x30, 0x2F

_T = []
for _i in range(256):
    _c = _i << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x1021) if _c & 0x8000 else (_c << 1)
    _T.append(_c & 0xFFFF)


def crc16(d, crc=0):
    for b in d:
        crc = ((crc << 8) & 0xFFFF) ^ _T[((crc >> 8) ^ b) & 0xFF]
    return crc


def enc(buf, off, size, key=0xFFFF):
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def u16(b, o):
    return b[o] | b[o + 1] << 8


def u32(b, o):
    return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24


class Firmware:
    def __init__(self, raw):
        self.raw = bytes(raw)
        if len(self.raw) < BLOCKS * BLK:
            raise ValueError("not an FM-1 package (too short)")
        self.product = "".join(chr((self.raw[i * BLK + KEEP] - i - 1) & 0xFF)
                               for i in range(BLOCKS) if self.raw[i * BLK + KEEP] != 0x7D)
        h = bytearray()
        for i in range(BLOCKS):
            h += self.raw[i * BLK:i * BLK + KEEP]
        enc(h, 0, 0x40)
        hcrc, lcrc, nent = u16(h, 0), u16(h, 2), u16(h, 8)
        if crc16(h[2:0x40]) != hcrc:
            raise ValueError("not an FM-1 .fwsc (UFW header CRC mismatch)")
        self.skew = BLOCKS * BLK - len(h)
        hsize = 0x40 + nent * 0x50
        if crc16(h[0x40:hsize]) != lcrc:
            raise ValueError("UFW entry list CRC mismatch")
        self.entries = []
        for off in range(0x40, hsize, 0x50):
            e = bytearray(h[off:off + 0x50])
            enc(e, 0, 0x50)
            name = bytes(e[0x40:0x50]).split(b"\0")[0].decode("latin-1")
            ent = {"type": u16(e, 0), "dcrc": u16(e, 4), "data_off": u32(e, 8), "size": u32(e, 12), "name": name}
            a = ent["data_off"] + self.skew
            if crc16(self.raw[a:a + ent["size"]]) != ent["dcrc"]:
                raise ValueError(f"{name}: data CRC mismatch")
            self.entries.append(ent)
        if not any(e["type"] == 0 for e in self.entries):
            raise ValueError("no flash.bin in the package")
