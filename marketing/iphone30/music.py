"""Trilha do filme-conceito "iPhone 30 — Um dia" (60 s, 120 BPM, ré maior).

Despertador em marimba e indie-pop totalmente originais, sintetizados aqui.
Saída: out/trilha.wav (48 kHz, estéreo, 16-bit).
"""

import os
import wave
from functools import lru_cache

import numpy as np

SR = 48_000
DUR = 60.0
BEAT = 0.5
N = int(SR * DUR)
rng = np.random.default_rng(30)

L = np.zeros(N)
R = np.zeros(N)

NOTES = {"C": -9, "C#": -8, "D": -7, "D#": -6, "E": -5, "F": -4, "F#": -3, "G": -2, "G#": -1, "A": 0, "A#": 1, "B": 2}


def hz(name):
    return 440.0 * 2 ** ((NOTES[name[:-1]] + 12 * (int(name[-1]) - 4)) / 12)


def t_axis(sec):
    return np.arange(int(sec * SR)) / SR


def add(sig, start, gain=1.0, pan=0.0):
    i = int(start * SR)
    if i >= N or i < 0:
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


# ---------- instrumentos ----------
@lru_cache(maxsize=None)
def marimba(freq, dur=0.9):
    t = t_axis(dur)
    # marimba: fundamental + parcial ~4x (barra de madeira), decaimento rápido
    sig = np.sin(2 * np.pi * freq * t) * np.exp(-t * 7) + 0.35 * np.sin(2 * np.pi * freq * 3.93 * t) * np.exp(-t * 22)
    return sig * np.clip(t / 0.002, 0, 1) * 0.5


@lru_cache(maxsize=None)
def kick():
    t = t_axis(0.4)
    f = 48 + 100 * np.exp(-t * 30)
    return np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 9) * 0.9


@lru_cache(maxsize=None)
def clap():
    t = t_axis(0.2)
    n = rng.standard_normal(len(t))
    hp = n - onepole_lp(n, 1200)
    env = sum(np.exp(-np.clip(t - d, 0, None) * 140) * (t >= d) for d in (0, 0.009, 0.019)) * 0.4 + np.exp(-t * 22) * 0.5
    return hp * env * 0.4


@lru_cache(maxsize=None)
def shaker():
    t = t_axis(0.07)
    n = rng.standard_normal(len(t))
    return (n - onepole_lp(n, 6000)) * np.exp(-t * 60) * 0.1


@lru_cache(maxsize=None)
def bass(freq, dur):
    t = t_axis(dur)
    sig = np.sin(2 * np.pi * freq * t) + 0.35 * np.sin(2 * np.pi * freq * 2 * t) + 0.12 * np.sin(2 * np.pi * freq * 3 * t)
    return sig * np.clip(t / 0.004, 0, 1) * np.exp(-t * 4) * 0.4


@lru_cache(maxsize=None)
def epiano(freqs, dur=0.45, soft=False):
    t = t_axis(dur)
    sig = sum(np.sin(2 * np.pi * f * t + 0.8 * np.sin(2 * np.pi * f * t) * np.exp(-t * 8)) for f in freqs) / len(freqs)
    env = np.clip(t / 0.003, 0, 1) * np.exp(-t * (1.6 if soft else 5.5))
    return sig * env * 0.32


@lru_cache(maxsize=None)
def lead(freq, dur):
    t = t_axis(dur)
    vib = 1 + 0.004 * np.sin(2 * np.pi * 5.5 * t) * np.clip((t - 0.12) / 0.2, 0, 1)
    sig = np.sin(2 * np.pi * np.cumsum(freq * vib) / SR) + 0.08 * np.sin(2 * np.pi * np.cumsum(freq * 2 * vib) / SR)
    return sig * np.clip(t / 0.03, 0, 1) * np.clip((dur - t) / 0.08, 0, 1) * 0.16


@lru_cache(maxsize=None)
def pad(freqs, dur):
    t = t_axis(dur)
    sig = sum(sum(np.sin(2 * np.pi * f * (1 + d) * t) for d in (-0.003, 0.003)) for f in freqs) / (2 * len(freqs))
    return sig * np.clip(t / 0.8, 0, 1) * np.clip((dur - t) / 0.8, 0, 1) * 0.22


def impact():
    t = t_axis(1.2)
    thud = np.sin(2 * np.pi * np.cumsum(55 + 90 * np.exp(-t * 30)) / SR) * np.exp(-t * 8)
    tap = onepole_lp(rng.standard_normal(len(t)), 2500) * np.exp(-t * 40) * 0.6
    return (thud + tap) * 0.9


def riser(dur):
    t = t_axis(dur)
    x = t / dur
    return onepole_lp(rng.standard_normal(len(t)), 400 + 9000 * x**2) * x**2 * 0.18


# Progressão: D – A – Bm – G (um acorde por compasso)
PROG = [
    ("D2", ("D4", "F#4", "A4")),
    ("A1", ("C#4", "E4", "A4")),
    ("B1", ("D4", "F#4", "B4")),
    ("G1", ("D4", "G4", "B4")),
]
HOOK = [  # (tempo em colcheias desde o início do compasso duplo, nota, duração em colcheias)
    (0, "F#5", 1), (1, "A5", 1), (2, "B5", 2), (5, "A5", 1), (6, "F#5", 2),
    (8, "E5", 1), (9, "F#5", 1), (10, "A5", 3), (14, "E5", 2),
]


def chord(sec):
    return PROG[int(sec // 2) % 4]


def groove(start, end, kick_on=True, hook=False, keys=True, hats=True):
    for b in range(int((end - start) // 2)):
        bs = start + b * 2
        root, triad = chord(bs)
        rf = hz(root)
        tf = tuple(hz(n) for n in triad)
        for beat in range(4):
            t0 = bs + beat * BEAT
            if kick_on:
                add(kick(), t0, gain=0.85)
            if beat in (1, 3):
                add(clap(), t0, gain=0.9)
            if keys:
                add(epiano(tf), t0 + BEAT / 2, gain=0.8, pan=-0.2)  # acordes no contratempo
        if hats:
            for s in range(16):
                add(shaker(), bs + s * BEAT / 4, gain=1.0 if s % 2 else 0.5, pan=0.4)
        # baixo sincopado
        for pos, mult in ((0, 1), (0.75, 1), (1.5, 2), (2.5, 1), (3.0, 1), (3.5, 2)):
            add(bass(rf * mult, 0.3), bs + pos * BEAT, gain=0.9)
        if hook and b % 2 == 0:
            for e, note, length in HOOK:
                add(lead(hz(note), length * BEAT / 2), bs + e * BEAT / 2, gain=0.9, pan=0.15)


# 0–3 s: despertador (motivo original em marimba)
ALARM = ["D5", "A5", "F#5", "A5", "D6", "A5", "F#5", "E5"]
for i in range(12):
    add(marimba(hz(ALARM[i % 8])), 0.15 + i * 0.25, gain=0.75 - (0.02 * i if i > 8 else 0), pan=0.25)
# corta o despertador quando ele toca "Parar" (2,85 s)
cut = int(2.85 * SR)
fade = np.linspace(1, 0, int(0.05 * SR))
L[cut : cut + len(fade)] *= fade
R[cut : cut + len(fade)] *= fade
L[cut + len(fade) : int(3.0 * SR)] = 0
R[cut + len(fade) : int(3.0 * SR)] = 0

# 3 s: a música entra
groove(3.0, 7.0)
add(pad(tuple(hz(n) for n in ("A2", "E3", "C#4")), 2.0), 3.0, gain=0.8)
add(pad(tuple(hz(n) for n in ("B2", "F#3", "D4")), 2.0), 5.0, gain=0.8)
# 7–8: câmera lenta (só acorde suspenso) e impacto do celular no chão em 7,9
add(pad(tuple(hz(n) for n in ("A3", "D4", "E4")), 1.0), 6.95, gain=1.0)
add(impact(), 7.9, gain=1.0)
groove(8.0, 22.0, hook=True)
groove(22.0, 28.0, hats=False)  # túnel (o filtro vem no master)
groove(28.0, 36.0, hook=True)
groove(36.0, 42.0)
add(riser(2.0), 40.0, gain=0.9)
# 42–50: noite, emoção — sem bumbo no começo
groove(42.0, 46.0, kick_on=False, hats=False, keys=False)
for b in range(4):
    root, triad = chord(42 + b * 2)
    add(epiano(tuple(hz(n) for n in triad), 2.0, soft=True), 42 + b * 2, gain=1.1)
    add(pad(tuple(hz(n) for n in triad), 2.2), 42 + b * 2, gain=0.8)
for e, note, length in HOOK:
    add(lead(hz(note) * 2 ** (2 / 12), length * BEAT / 2), 43.0 + e * BEAT / 2, gain=0.7)
groove(46.0, 50.0, hook=True)
# 50–55: a música se desfaz
for b in range(2):
    root, triad = chord(50 + b * 2)
    add(epiano(tuple(hz(n) for n in triad), 2.0, soft=True), 50 + b * 2, gain=0.9 - 0.3 * b)
    add(bass(hz(root), 1.5), 50 + b * 2, gain=0.6 - 0.2 * b)
# 55–60: acorde final + o motivo do despertador, uma última vez
add(pad(tuple(hz(n) for n in ("D3", "A3", "F#4", "A4")), 5.0), 55.0, gain=0.9)
add(epiano(tuple(hz(n) for n in ("D4", "F#4", "A4", "E5")), 3.0, soft=True), 55.0, gain=0.9)
for i, n in enumerate(["D5", "A5", "F#5", "A5", "D6"]):
    add(marimba(hz(n), 1.4), 57.0 + i * 0.25, gain=0.5, pan=0.25)


# ---------- master ----------
def reverb(x, seconds=1.8, mix=0.14):
    n = int(seconds * SR)
    t = np.arange(n) / SR
    ir = rng.standard_normal(n) * np.exp(-t * 3.5)
    ir[0] = 0
    size = 1 << int(np.ceil(np.log2(len(x) + n)))
    wet = np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(ir, size), size)[: len(x)]
    wet /= np.max(np.abs(wet)) + 1e-9
    return x + wet * mix * np.max(np.abs(x))


L = reverb(L)
R = reverb(R)

# Túnel do metrô (22–28 s): tudo abafado, abrindo de novo na saída.
a, b = int(22.0 * SR), int(28.0 * SR)
cutoff = np.concatenate([np.linspace(8000, 500, int(0.3 * SR)), np.full(b - a - int(0.8 * SR), 500.0), np.linspace(500, 12000, int(0.5 * SR))])
for ch in (L, R):
    ch[a:b] = onepole_lp(ch[a:b], cutoff)

# Câmera lenta (7–7,9 s): o groove some, sobra só o acorde suspenso.
s0, s1 = int(7.0 * SR), int(7.9 * SR)
duck = np.linspace(1, 0.25, s1 - s0)
L[s0:s1] = onepole_lp(L[s0:s1], 900) * duck
R[s0:s1] = onepole_lp(R[s0:s1], 900) * duck

stereo = np.stack([L, R], axis=1)
stereo = np.tanh(stereo / np.max(np.abs(stereo)) * 1.4) / np.tanh(1.4) * 0.93
fade_n = int(1.5 * SR)
stereo[-fade_n:] *= np.linspace(1, 0, fade_n)[:, None]

os.makedirs("out", exist_ok=True)
pcm = (stereo * 32767).astype(np.int16)
with wave.open("out/trilha.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(pcm.tobytes())
print(f"out/trilha.wav  {DUR:.0f}s")
