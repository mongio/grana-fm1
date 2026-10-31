#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""GRANA: the built-in sounds, synthesised here (no recordings, nothing to license), and the tables.

  python3 tools/make_samples.py      -> firmware/gen/grana_data.h

Each sound: 22050 Hz mono, about 3 s, stored as IMA ADPCM (4 bit) and decoded into RAM when chosen.
Also: the grain windows (1024 points, Q15), the pitch table (2^(c/1200), Q15), the sizes table.
"""
from pathlib import Path
import numpy as np

SR = 22050
DUR = 3.0
N = int(SR * DUR)
t = np.arange(N) / SR
rng = np.random.default_rng(7)
OUT = Path(__file__).resolve().parents[1] / "firmware" / "gen" / "grana_data.h"


def norm(x, peak=0.89):
    x = x - np.mean(x)
    return x * (peak / (np.max(np.abs(x)) + 1e-9))


def fade(x, a=0.01, r=0.05):
    na, nr = int(a * SR), int(r * SR)
    x = x.copy()
    x[:na] *= np.linspace(0, 1, na)
    x[-nr:] *= np.linspace(1, 0, nr)
    return x


def biquad_bp(x, f, q):
    """band-pass, f may be an array (slow sweep)"""
    f = np.broadcast_to(np.asarray(f, float), x.shape)
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i in range(len(x)):
        w = 2 * np.pi * f[i] / SR
        al = np.sin(w) / (2 * q)
        b0, a0, a1, a2 = al, 1 + al, -2 * np.cos(w), 1 - al
        yi = (b0 * x[i] - b0 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1, y2, y1 = x1, x[i], y1, yi
        y[i] = yi
    return y


def lowpass_res(x, f, q):
    f = np.broadcast_to(np.asarray(f, float), x.shape)
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i in range(len(x)):
        w = 2 * np.pi * f[i] / SR
        al = np.sin(w) / (2 * q)
        c = np.cos(w)
        b0 = (1 - c) / 2
        b1 = 1 - c
        a0, a1, a2 = 1 + al, -2 * c, 1 - al
        yi = (b0 * x[i] + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1, y2, y1 = x1, x[i], y1, yi
        y[i] = yi
    return y


def saw(f, ph0=0.0):
    ph = np.cumsum(np.broadcast_to(f, t.shape) / SR) + ph0
    return 2 * (ph % 1.0) - 1


# 1 VOCI: two sawtooth voices through vowel formants a-e-i-o-u, with vibrato
def voci():
    vib = 1 + 0.006 * np.sin(2 * np.pi * 5.2 * t)
    src = saw(110 * vib) + 0.7 * saw(164.8 * vib, 0.3) + 0.5 * saw(220.6 * vib, 0.6)
    vowels = np.array([[800, 1150, 2900], [400, 1600, 2700], [270, 2300, 3000], [450, 800, 2830], [325, 700, 2530]], float)
    k = np.linspace(0, len(vowels) - 1, N)
    i0 = np.floor(k).astype(int).clip(0, len(vowels) - 2)
    fr = k - i0
    fr = fr * fr * (3 - 2 * fr)
    F = vowels[i0] * (1 - fr[:, None]) + vowels[i0 + 1] * fr[:, None]
    y = biquad_bp(src, F[:, 0], 8) + 0.6 * biquad_bp(src, F[:, 1], 10) + 0.25 * biquad_bp(src, F[:, 2], 12)
    return fade(norm(y), 0.08, 0.3)


# 2 CAMPANA: inharmonic partials, two strikes
def campana():
    ratios = [0.56, 0.92, 1.19, 1.71, 2.0, 2.74, 3.0, 3.76, 4.07]
    amps = [1, 0.67, 1, 1.8, 2.67, 1.67, 1.46, 1.33, 1.33]
    decs = [1.6, 1.2, 0.9, 0.8, 0.7, 0.5, 0.45, 0.35, 0.3]
    y = np.zeros(N)
    for t0, f0, g in ((0.0, 392.0, 1.0), (1.35, 293.7, 0.8)):
        tt = t - t0
        on = tt >= 0
        for r, a, d in zip(ratios, amps, decs):
            y[on] += g * a * np.exp(-tt[on] / (d * 1.6)) * np.sin(2 * np.pi * f0 * r * tt[on])
    return fade(norm(y), 0.001, 0.1)


# 3 ARPA: Karplus-Strong arpeggio
def arpa():
    y = np.zeros(N)
    notes = [130.81, 164.81, 196.0, 246.94, 293.66, 329.63, 392.0, 493.88]
    for i, f in enumerate(notes):
        start = int(i * 0.3 * SR)
        L = int(SR / f)
        buf = rng.uniform(-1, 1, L)
        buf = np.convolve(buf, [0.5, 0.5], "same")
        n = N - start
        out = np.zeros(n)
        b = buf.copy()
        for j in range(n):
            out[j] = b[j % L]
            b[j % L] = 0.996 * 0.5 * (b[j % L] + b[(j + 1) % L])
        y[start:] += out * 0.6
    return fade(norm(y), 0.001, 0.1)


# 4 PIOGGIA: drops: noise grains through resonant band-passes
def pioggia():
    y = np.zeros(N)
    bed = biquad_bp(rng.normal(0, 1, N), 3000, 0.7) * 0.05
    y += bed
    for _ in range(140):
        s = rng.integers(0, N - 3000)
        L = int(rng.uniform(0.004, 0.03) * SR)
        burst = rng.normal(0, 1, L) * np.exp(-np.arange(L) / (L / 4))
        f = rng.uniform(800, 5000)
        seg = np.zeros(3000)
        seg[:L] = burst
        seg = biquad_bp(seg, f, rng.uniform(6, 30))
        y[s:s + 3000] += seg * rng.uniform(0.3, 1.0)
    return fade(norm(y), 0.05, 0.2)


# 5 ORGANO: drawbars on a Dm9 chord with a slow leslie
def organo():
    chord = [146.83, 174.61, 220.0, 261.63, 329.63]
    bars = [(0.5, 0.6), (1, 1.0), (1.5, 0.5), (2, 0.7), (3, 0.4), (4, 0.3), (6, 0.15)]
    y = np.zeros(N)
    for f in chord:
        for h, a in bars:
            y += a * np.sin(2 * np.pi * f * h * t + rng.uniform(0, 6.28))
    y *= 1 + 0.25 * np.sin(2 * np.pi * 5.8 * t)
    return fade(norm(y), 0.03, 0.15)


# 6 RITMO: one bar and a half of drums at 120 BPM: kick, snare, hats
def ritmo():
    y = np.zeros(N)
    step = 0.125  # sixteenths at 120 BPM
    kick = [0, 6, 8, 11, 16, 22]
    snare = [4, 12, 20]
    for s in range(24):
        i = int(s * step * SR)
        n = N - i
        tt = np.arange(n) / SR
        if s in kick:
            f = 45 + 110 * np.exp(-tt / 0.03)
            y[i:] += np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-tt / 0.25)
        if s in snare:
            y[i:] += 0.7 * rng.normal(0, 1, n) * np.exp(-tt / 0.08) + 0.5 * np.sin(2 * np.pi * 190 * tt) * np.exp(-tt / 0.06)
        hat = biquad_bp(rng.normal(0, 1, 2000), 8000, 1.5) * np.exp(-np.arange(2000) / (300 if s % 2 else 120))
        m = min(2000, n)
        y[i:i + m] += 0.35 * hat[:m]
    return fade(norm(np.tanh(1.3 * y)), 0.001, 0.05)


# 7 VETRO: FM glass tones, a slow sequence
def vetro():
    y = np.zeros(N)
    seq = [659.26, 783.99, 987.77, 880.0, 1174.66, 987.77]
    for i, f in enumerate(seq):
        t0 = i * 0.5
        tt = t - t0
        on = tt >= 0
        e = np.exp(-tt[on] / 0.9)
        idx = 2.5 * np.exp(-tt[on] / 0.25)
        y[on] += e * np.sin(2 * np.pi * f * tt[on] + idx * np.sin(2 * np.pi * f * 3.5 * tt[on]))
    return fade(norm(y), 0.001, 0.1)


# 8 MOTORE: detuned saws, resonant low-pass sweeping
def motore():
    src = saw(55.0) + saw(55.4, 0.2) + saw(82.5, 0.5) * 0.6 + saw(110.3, 0.7) * 0.4
    fc = 200 + 2600 * (0.5 - 0.5 * np.cos(2 * np.pi * t / DUR))
    y = lowpass_res(src, fc, 6)
    return fade(norm(np.tanh(0.8 * y)), 0.05, 0.2)


SOUNDS = [("VOCI", voci), ("CAMPANA", campana), ("ARPA", arpa), ("PIOGGIA", pioggia),
          ("ORGANO", organo), ("RITMO", ritmo), ("VETRO", vetro), ("MOTORE", motore)]

# ---- IMA ADPCM
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
        97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
        4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
        18500, 20350, 22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(pcm):
    pred, idx, out, nib = 0, 0, bytearray(), []
    for s in pcm:
        st = STEP[idx]
        d = int(s) - pred
        code = 0
        if d < 0:
            code, d = 8, -d
        diff = st >> 3
        if d >= st: code |= 4; d -= st; diff += st
        if d >= st >> 1: code |= 2; d -= st >> 1; diff += st >> 1
        if d >= st >> 2: code |= 1; diff += st >> 2
        pred = pred - diff if code & 8 else pred + diff
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + IDX[code & 7]))
        nib.append(code)
    if len(nib) & 1:
        nib.append(0)
    for i in range(0, len(nib), 2):
        out.append(nib[i] | nib[i + 1] << 4)
    return bytes(out)


def adpcm_decode(data, n):
    pred, idx, out = 0, 0, []
    for i in range(n):
        code = (data[i >> 1] >> ((i & 1) * 4)) & 15
        st = STEP[idx]
        diff = st >> 3
        if code & 4: diff += st
        if code & 2: diff += st >> 1
        if code & 1: diff += st >> 2
        pred = pred - diff if code & 8 else pred + diff
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + IDX[code & 7]))
        out.append(pred)
    return np.array(out)


def carr(name, ctype, vals, per=16):
    s = [f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0, len(vals), per):
        s.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
    s.append("};")
    return "\n".join(s)


def bank_build(items):
    """the sample bank, as the firmware reads it after its code (and the editor writes it):
      0  "GRNB", u16 version 1, u16 count, u32 length of the whole bank, u32 crc32 of bytes 16..end
      16 count entries of 24 bytes: name[12] (ASCII, zero padded), u32 offset (from the bank's start),
         u32 frames (22050 Hz mono), u8 mode (0 HIFI, 1 LOFI), 3 zero bytes
      then each sound's IMA ADPCM stream (4 bits a frame, low nibble first, predictor and index from 0)"""
    import struct, zlib
    head = 16 + 24 * len(items)
    blobs, entries, off = [], [], head
    for name, frames, mode, data in items:
        entries.append(struct.pack("<12sIIB3x", name.encode("ascii")[:12], off, frames, mode))
        blobs.append(data + (b"\0" if len(data) & 1 else b""))
        off += len(blobs[-1])
    body = b"".join(entries) + b"".join(blobs)
    return struct.pack("<4sHHII", b"GRNB", 1, len(items), 16 + len(body), zlib.crc32(body)) + body


def main():
    parts = ["/* generated by tools/make_samples.py: do not edit */", "#pragma once", ""]
    total = 0
    items = []
    for i, (name, fn) in enumerate(SOUNDS):
        x = fn()
        pcm = np.clip(np.round(x * 32767), -32767, 32767).astype(np.int32)
        enc = adpcm_encode(pcm)
        dec = adpcm_decode(enc, len(pcm))
        snr = 10 * np.log10(np.sum(pcm.astype(float) ** 2) / (np.sum((pcm - dec).astype(float) ** 2) + 1))
        print(f"{name:8s} {len(pcm)} frames, {len(enc)} B, SNR {snr:.1f} dB")
        items.append((name, len(pcm), 0, enc))
        total += len(enc)
    bank = bank_build(items)
    (OUT.parent / "bank_default.bin").write_bytes(bank)
    host = ["/* generated by tools/make_samples.py: the default bank, for the host build */", "#pragma once",
            carr("GRANA_BANK_DEFAULT", "uint8_t", list(bank), 32)]
    (OUT.parent / "grana_bank.h").write_text("\n".join(host) + "\n")
    print(f"bank     {len(bank)} B -> {OUT.parent / 'bank_default.bin'}")
    parts.append(carr("ADPCM_STEP", "int16_t", STEP))
    parts.append(carr("ADPCM_IDX", "int8_t", IDX))

    # grain windows, 1024 points + 1 guard, Q15
    W = 1024
    x = np.arange(W + 1) / W
    hann = 0.5 - 0.5 * np.cos(2 * np.pi * x)
    tuk = np.ones_like(x)
    a = 0.2
    m = x < a / 2
    tuk[m] = 0.5 - 0.5 * np.cos(2 * np.pi * x[m] / a)
    m = x > 1 - a / 2
    tuk[m] = 0.5 - 0.5 * np.cos(2 * np.pi * (1 - x[m]) / a)
    perc = np.where(x < 0.03, x / 0.03, np.exp(-(x - 0.03) / 0.25))
    perc[-8:] *= np.linspace(1, 0, 8)
    rperc = perc[::-1].copy()
    wins = [hann, tuk, perc, rperc]
    flat = []
    for w in wins:
        flat += [int(round(v * 32767)) for v in np.clip(w, 0, 1)]
    parts.append("#define WIN_N 1024u")
    parts.append(carr("WINDOWS", "int16_t", flat))
    # 2^(c/1200) Q15 for c = 0..1199 (as uint16: 32768..65502 does not fit int16)
    cents = [int(round(32768 * 2 ** (c / 1200))) for c in range(1200)]
    parts.append(carr("CENTS_Q15", "uint32_t", cents))
    # grain sizes: 64 steps, 5 ms .. 1500 ms, exponential (in ms)
    sizes = [int(round(5 * (300 ** (i / 63)))) for i in range(64)]
    parts.append(carr("SIZE_MS", "uint16_t", sizes))
    # density: 64 steps, 1 .. 120 grains / s, as tenths of Hz
    dens = [int(round(10 * (1 * (120 ** (i / 63))))) for i in range(64)]
    parts.append(carr("DENS_DHZ", "uint16_t", dens))
    # attack / release: 64 steps, 2 ms .. 4000 ms
    env = [int(round(2 * (2000 ** (i / 63)))) for i in range(64)]
    parts.append(carr("ENV_MS", "uint16_t", env))
    # delay time: 64 steps, 20 ms .. 1000 ms
    dly = [int(round(20 * (50 ** (i / 63)))) for i in range(64)]
    parts.append(carr("DLY_MS", "uint16_t", dly))
    # reverb tail: 0..99 -> the tank gain for a decay of 0.4 s .. 16 s (the tank's round trip is ~0.7 s and
    # passes two gains); 100 = 0.995, almost endless
    rdec = []
    for v in range(101):
        rt = 0.4 * 40 ** (v / 99)
        g = 10 ** (-0.70 * 60 / (40 * rt)) if v < 100 else 0.995
        rdec.append(int(round(32767 * min(g, 0.995))))
    parts.append(carr("RDEC_Q15", "uint16_t", rdec))
    # the filter: 1024 steps over 9 octaves, 30 Hz .. 15.4 kHz; g = tan(pi fc / 44100) for the TPT state variable
    # filter, Q16; and the 128 steps of its knob, in Hz, for the screen
    fc = [30 * 512 ** (i / 1023) for i in range(1024)]
    parts.append(carr("SVF_G", "uint32_t", [int(round((1 << 28) * np.tan(np.pi * f / 44100))) for f in fc]))   # Q28
    parts.append(carr("RES_K", "int32_t", [int(round(65536 * 2 / 20 ** (r / 100))) for r in range(101)]))   # SVF damping: Q 0.5 .. 10, even to the ear
    parts.append(carr("LAD_K", "int32_t", [int(round(65536 * 3.95 * (r / 100) ** 0.6)) for r in range(101)]))   # ladder feedback: 0 .. 3.95
    parts.append(carr("LFO_INC", "uint32_t", [int(round((1 / 60) * 1200 ** (i / 63) * 2 ** 32 / 1000)) for i in range(64)]))   # LFO phase per ms: 1/60 Hz .. 20 Hz
    # MODULAZIONE: phase per sample at 44100 Hz; the rate (phaser, flanger) 0.03..10 Hz, the ring's carrier 20..2000 Hz,
    # the frequency shift +-0.1..1000 Hz (50 = none); and the same in tenths of Hz for the screen
    mr = [0.03 * (10 / 0.03) ** (i / 100) for i in range(101)]
    rg = [20 * 100 ** (i / 100) for i in range(101)]
    fs_ = [0.0] + [0.1 * 10000 ** ((k - 1) / 49) for k in range(1, 51)]
    parts.append(carr("MRATE_INC", "uint32_t", [int(round(f * 2 ** 32 / 44100)) for f in mr]))
    parts.append(carr("RING_INC", "uint32_t", [int(round(f * 2 ** 32 / 44100)) for f in rg]))
    parts.append(carr("SHIFT_INC", "uint32_t", [int(round(f * 2 ** 32 / 44100)) for f in fs_]))
    parts.append(carr("MRATE_DHZ", "uint32_t", [int(round(f * 100)) for f in mr]))   # hundredths
    parts.append(carr("RING_HZ", "uint16_t", [int(round(f)) for f in rg]))
    parts.append(carr("SHIFT_DHZ", "uint32_t", [int(round(f * 10)) for f in fs_]))
    # the compressor: log2(1 + i/32) and 2^(i/32), Q8 and Q15
    parts.append(carr("SIN_T", "int16_t", [int(round(32767 * np.sin(2 * np.pi * i / 256))) for i in range(257)]))   # a sine, 256 steps
    parts.append(carr("LOG2T", "uint16_t", [int(round(256 * np.log2(1 + i / 32))) for i in range(33)]))
    parts.append(carr("EXP2T", "uint16_t", [int(round(32768 * 2 ** (i / 32))) - 1 for i in range(33)]))
    parts.append(carr("FCUT_HZ", "uint16_t", [int(round(30 * 512 ** (v / 127))) for v in range(128)]))
    OUT.write_text("\n".join(parts) + "\n")
    print(f"total ADPCM {total} B -> {OUT}")


if __name__ == "__main__":
    main()
