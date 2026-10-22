"""Trilha do comercial-paródia JBDS (52 s): drone, braams, tambores, silêncio e zumbido.

Tudo sintetizado aqui, sem samples. Saída: out/trilha.wav (48 kHz, estéreo, 16-bit).
"""

import os
import wave
from functools import lru_cache

import numpy as np

SR = 48_000
DUR = 52.0
N = int(SR * DUR)
rng = np.random.default_rng(11)

L = np.zeros(N)
R = np.zeros(N)


def t_axis(sec):
    return np.arange(int(sec * SR)) / SR


def add(sig, start, gain=1.0, pan=0.0):
    i = int(start * SR)
    if i >= N:
        return
    sig = sig[: N - i]
    L[i : i + len(sig)] += sig * gain * np.cos((pan + 1) * np.pi / 4)
    R[i : i + len(sig)] += sig * gain * np.sin((pan + 1) * np.pi / 4)


def onepole_lp(x, cutoff):
    cutoff = np.broadcast_to(np.asarray(cutoff, dtype=float), x.shape)
    a = 1 - np.exp(-2 * np.pi * cutoff / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc += a[i] * (x[i] - acc)
        y[i] = acc
    return y


def saw(freq, t, harmonics=24):
    ph = 2 * np.pi * freq * t
    return sum(np.sin(ph * k) / k for k in range(1, harmonics))


# ---------- instrumentos ----------
@lru_cache(maxsize=None)
def drone(dur):
    t = t_axis(dur)
    sig = saw(55, t, 12) + saw(55.3, t, 12) + 0.6 * saw(82.4, t, 8)
    lfo = 300 + 250 * (0.5 + 0.5 * np.sin(2 * np.pi * 0.15 * t))
    out = onepole_lp(sig, lfo) * 0.12
    return out * np.clip(t / 2.0, 0, 1)


@lru_cache(maxsize=None)
def heartbeat():
    t = t_axis(0.7)
    thump = lambda d: np.sin(2 * np.pi * (40 + 30 * np.exp(-np.clip(t - d, 0, None) * 20)) * t) * np.exp(-np.clip(t - d, 0, None) * 14) * (t >= d)
    return (thump(0.0) + 0.7 * thump(0.22)) * 0.8


@lru_cache(maxsize=None)
def braam(root=55.0, dur=3.5):
    """O "BWAAAM" de trailer: pilha de serras graves, saturada, com ataque rápido e cauda longa."""
    t = t_axis(dur)
    sig = sum(saw(root * m * (1 + d), t, 30) for m in (0.5, 1, 1.5, 2) for d in (-0.006, 0.006))
    env = np.clip(t / 0.06, 0, 1) * np.exp(-t * 0.9)
    bright = 400 + 2400 * np.exp(-t * 2.5)
    sig = onepole_lp(sig, bright) * env
    sig = np.tanh(sig * 0.9)
    sub = np.sin(2 * np.pi * root * 0.5 * t) * np.exp(-t * 0.8)
    return (sig * 0.45 + sub * 0.6)


@lru_cache(maxsize=None)
def drum(pitch=70.0):
    """Tambor grande (estilo taiko): pitch que cai + corpo de ruído."""
    t = t_axis(1.2)
    f = pitch * (1 + 1.5 * np.exp(-t * 25))
    body = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 4)
    skin = onepole_lp(rng.standard_normal(len(t)), 1800) * np.exp(-t * 30) * 0.6
    return (body + skin) * 0.9


@lru_cache(maxsize=None)
def impact(size=1.0):
    t = t_axis(4.0)
    boom = np.sin(2 * np.pi * np.cumsum(28 + 70 * np.exp(-t * 5)) / SR) * np.exp(-t * (1.3 / size))
    crack = onepole_lp(rng.standard_normal(len(t)), 3000) * np.exp(-t * 6) * 0.5
    return np.tanh((boom + crack) * 1.4) * 0.9


@lru_cache(maxsize=None)
def glass():
    t = t_axis(1.5)
    tones = sum(np.sin(2 * np.pi * f * t) * np.exp(-t * d) for f, d in ((3150, 3), (4720, 4), (6380, 6), (8120, 8)))
    shatter = (rng.standard_normal(len(t)) * np.exp(-t * 9)) * 0.5
    return (tones * 0.08 + onepole_lp(shatter, 9000) * 0.6) * 0.7


@lru_cache(maxsize=None)
def beep(freq=1400.0, dur=0.12):
    t = t_axis(dur)
    return np.sin(2 * np.pi * freq * t) * np.clip(np.minimum(t / 0.005, (dur - t) / 0.01), 0, 1) * 0.25


@lru_cache(maxsize=None)
def chug(freq=55.0):
    t = t_axis(0.11)
    return onepole_lp(saw(freq, t, 20), 700) * np.exp(-t * 18) * 0.35


def riser(dur):
    t = t_axis(dur)
    x = t / dur
    noise = onepole_lp(rng.standard_normal(len(t)), 300 + 8000 * x**2) * x**2 * 0.3
    tone = np.sin(2 * np.pi * np.cumsum(80 + 900 * x**3) / SR) * x**2 * 0.15
    return noise + tone


def tinnitus(dur):
    """Zumbido de "ouvido estourado": senoidal aguda que oscila de leve e some."""
    t = t_axis(dur)
    f = 7600 + 40 * np.sin(2 * np.pi * 3 * t)
    tone = np.sin(2 * np.pi * np.cumsum(f) / SR)
    return tone * np.clip(t / 0.15, 0, 1) * np.exp(-t * 0.6) * 0.09


# ---------- composição ----------
add(drone(30.0), 0.0, gain=1.0)
for s in (0.6, 1.9, 3.2, 4.4):  # batimento acelerando
    add(heartbeat(), s, gain=0.9)
add(riser(1.2), 3.8, gain=0.6)

add(braam(55.0), 5.0, gain=1.0)  # "Esta é uma delas."
add(braam(41.2), 8.0, gain=1.1)  # JBDS
add(impact(1.2), 8.0, gain=0.9)

for s, extra in ((12.0, None), (14.0, glass), (16.0, None)):  # specs
    add(drum(65.0), s, gain=1.0)
    add(drum(90.0), s + 0.75, gain=0.6)
    if extra:
        add(extra(), s + 0.35, gain=1.0, pan=0.2)
add(riser(2.0), 16.0, gain=0.8)

# 18–24: ostinato tenso (colcheias a 120 BPM)
for i in range(24):
    add(chug(55.0 if (i // 8) % 2 == 0 else 58.27), 18.0 + i * 0.25, gain=0.9)
add(drum(60.0), 18.0)
add(drum(60.0), 21.0)

# 24–30: radar
for k in range(8):
    add(beep(1200.0, 0.08), 24.3 + k * 0.6, gain=0.7, pan=-0.3)
add(beep(700.0, 0.4), 26.2, gain=1.2)  # alguém detectado
add(beep(700.0, 0.4), 26.8, gain=1.2)
add(drone(4.0), 26.0, gain=0.8)
add(riser(1.6), 28.4, gain=0.7)  # "A não ser que…"

# 30–36: revelação do fone
add(braam(65.4), 30.0, gain=0.9)
add(impact(0.8), 30.0, gain=0.6)
for i in range(12):
    add(drum(75.0 if i % 2 == 0 else 100.0), 30.5 + i * 0.5, gain=0.55 if i % 2 else 0.8)

# 36–42: termos de uso (ostinato acelerando)
for i in range(48):
    add(chug(55.0 if i % 16 < 12 else 61.7), 36.0 + i * 0.125, gain=0.7)
for i in range(6):
    add(drum(60.0), 36.0 + i, gain=0.8)
add(riser(2.0), 40.0, gain=0.9)

# 42–44: contagem
for k, s in enumerate((42.0, 42.66, 43.33)):
    add(beep(1500.0 + 300 * k, 0.15), s, gain=1.1)

# 44: o volume máximo
add(impact(2.5), 44.0, gain=1.6)
add(braam(36.7, 4.0), 44.0, gain=1.4)
add(onepole_lp(rng.standard_normal(int(0.6 * SR)), 6000) * np.exp(-t_axis(0.6) * 5) * 0.8, 44.0, gain=1.0)


# ---------- master ----------
def reverb(x, seconds=2.6, mix=0.22):
    n = int(seconds * SR)
    t = np.arange(n) / SR
    ir = rng.standard_normal(n) * np.exp(-t * 2.6)
    ir[0] = 0
    size = 1 << int(np.ceil(np.log2(len(x) + n)))
    wet = np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(ir, size), size)[: len(x)]
    wet /= np.max(np.abs(wet)) + 1e-9
    return x + wet * mix * np.max(np.abs(x))


L = reverb(L)
R = reverb(R)

# Depois do impacto: "ficamos surdos" — tudo abafado, quase sumindo, e o zumbido por cima.
deaf_start, deaf_end = int(44.35 * SR), int(46.5 * SR)
for ch in (L, R):
    seg = ch[deaf_start:]
    cutoff = np.concatenate([np.full(deaf_end - deaf_start, 180.0), np.linspace(180, 12000, len(seg) - (deaf_end - deaf_start))])
    ch[deaf_start:] = onepole_lp(seg, cutoff) * np.concatenate([np.linspace(0.35, 0.12, deaf_end - deaf_start), np.full(len(seg) - (deaf_end - deaf_start), 0.12)])

# Assinatura: braam final distante (entra depois do "ensurdecimento", então não é abafado).
end_braam = reverb(np.concatenate([np.zeros(int(46.5 * SR)), braam(55.0, 5.5)])[:N], seconds=3.5, mix=0.5)
L += end_braam * 0.45
R += end_braam * 0.45
tin = np.zeros(N)
tin[deaf_start : deaf_start + int(4.0 * SR)] = tinnitus(4.0)
L += tin
R += tin

stereo = np.stack([L, R], axis=1)
peak = np.max(np.abs(stereo))
stereo = np.tanh(stereo / peak * 1.8) / np.tanh(1.8) * 0.95
fade = int(1.0 * SR)
stereo[-fade:] *= np.linspace(1, 0, fade)[:, None]

os.makedirs("out", exist_ok=True)
pcm = (stereo * 32767).astype(np.int16)
with wave.open("out/trilha.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(pcm.tobytes())
print(f"out/trilha.wav  {DUR:.0f}s")
