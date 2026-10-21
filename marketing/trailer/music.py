"""Trilha original do filme "A Interrupção" (48 s, 120 BPM, lá menor).

Tudo é sintetizado aqui — sem samples nem áudio de terceiros.
Saída: out/trilha.wav (48 kHz, estéreo, 16-bit).
"""

import os
import wave
from functools import lru_cache

import numpy as np

SR = 48_000
DUR = 48.0
BEAT = 0.5  # 120 BPM
N = int(SR * DUR)
rng = np.random.default_rng(7)

L = np.zeros(N)
R = np.zeros(N)
# Barramento separado para os tiques do silêncio: recebe reverb próprio e não herda a cauda da música.
TL = np.zeros(N)
TR = np.zeros(N)


def t_axis(sec):
    return np.arange(int(sec * SR)) / SR


def note_hz(name):
    names = {"C": -9, "C#": -8, "D": -7, "D#": -6, "E": -5, "F": -4, "F#": -3, "G": -2, "G#": -1, "A": 0, "A#": 1, "B": 2}
    pitch, octave = name[:-1], int(name[-1])
    return 440.0 * 2 ** ((names[pitch] + 12 * (octave - 4)) / 12)


def add(sig, start, gain=1.0, pan=0.0, bus=None):
    """Mistura `sig` (mono) a partir de `start` segundos, com pan -1..1."""
    left, right = bus or (L, R)
    i = int(start * SR)
    if i >= N:
        return
    sig = sig[: N - i]
    lg = gain * np.cos((pan + 1) * np.pi / 4)
    rg = gain * np.sin((pan + 1) * np.pi / 4)
    left[i : i + len(sig)] += sig * lg
    right[i : i + len(sig)] += sig * rg


def onepole_lp(x, cutoff):
    """Passa-baixa de um polo; `cutoff` pode ser escalar ou vetor (filtro varrendo)."""
    cutoff = np.broadcast_to(np.asarray(cutoff, dtype=float), x.shape)
    a = 1 - np.exp(-2 * np.pi * cutoff / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc += a[i] * (x[i] - acc)
        y[i] = acc
    return y


def highpass(x, cutoff):
    return x - onepole_lp(x, cutoff)


def env_adsr(n, a=0.01, d=0.1, s=0.7, r=0.2, length=None):
    length = length or n / SR
    t = np.arange(n) / SR
    e = np.where(t < a, t / a, np.where(t < a + d, 1 - (1 - s) * (t - a) / d, s))
    rel_start = max(length - r, a + d)
    e = np.where(t > rel_start, e * np.clip(1 - (t - rel_start) / r, 0, 1), e)
    return e


# ---------- instrumentos ----------
@lru_cache(maxsize=None)
def piano(freq, dur=1.6):
    t = t_axis(dur)
    sig = sum((0.6 ** k) * np.sin(2 * np.pi * freq * (k + 1) * t * (1 + 0.0004 * k)) * np.exp(-t * (2.2 + 1.3 * k)) for k in range(6))
    attack = np.clip(t / 0.004, 0, 1)
    return sig * attack * 0.35


@lru_cache(maxsize=None)
def _pad(freqs, dur, attack, release, bright):
    t = t_axis(dur)
    sig = np.zeros_like(t)
    for f in freqs:
        for det in (-0.004, 0.0, 0.005):
            ph = 2 * np.pi * f * (1 + det) * t
            # serra aproximada com poucos harmônicos (macia)
            sig += sum(np.sin(ph * k) / k for k in range(1, 7))
    sig /= max(1, len(freqs) * 3)
    sig = onepole_lp(sig, bright)
    return sig * env_adsr(len(t), a=attack, d=0.2, s=0.9, r=release, length=dur) * 0.22


def pad(freqs, dur, attack=0.6, release=0.8, bright=1800):
    return _pad(tuple(round(f, 3) for f in freqs), dur, attack, release, bright)


@lru_cache(maxsize=None)
def kick():
    t = t_axis(0.45)
    f = 45 + 110 * np.exp(-t * 28)
    ph = 2 * np.pi * np.cumsum(f) / SR
    return np.sin(ph) * np.exp(-t * 7.5) * 1.1 + 0.25 * np.sin(ph * 2) * np.exp(-t * 40)


@lru_cache(maxsize=None)
def clap():
    t = t_axis(0.25)
    n = rng.standard_normal(len(t))
    bursts = sum(np.exp(-np.clip(t - d, 0, None) * 120) * (t >= d) for d in (0, 0.011, 0.022))
    return highpass(n, 900) * (bursts * 0.5 + np.exp(-t * 18) * 0.6) * 0.35


@lru_cache(maxsize=None)
def hat(open_=False):
    t = t_axis(0.25 if open_ else 0.06)
    n = highpass(rng.standard_normal(len(t)), 7000)
    return n * np.exp(-t * (14 if open_ else 70)) * 0.18


@lru_cache(maxsize=None)
def bass(freq, dur):
    t = t_axis(dur)
    ph = 2 * np.pi * freq * t
    saw = sum(np.sin(ph * k) / k for k in range(1, 12))
    sub = np.sin(ph) * 0.8
    sig = onepole_lp(saw, 220 + 900 * np.exp(-t * 9)) + sub
    return sig * env_adsr(len(t), a=0.005, d=0.15, s=0.75, r=0.06, length=dur) * 0.33


@lru_cache(maxsize=None)
def pluck(freq, dur=0.35):
    t = t_axis(dur)
    sig = np.sin(2 * np.pi * freq * t) + 0.4 * np.sin(2 * np.pi * freq * 2.001 * t) + 0.15 * np.sin(2 * np.pi * freq * 3 * t)
    return sig * np.exp(-t * 9) * 0.12


@lru_cache(maxsize=None)
def tick():
    t = t_axis(0.05)
    return np.sin(2 * np.pi * 2600 * t) * np.exp(-t * 160) * 0.5 + np.sin(2 * np.pi * 5200 * t) * np.exp(-t * 300) * 0.2


def riser(dur, reverse=False):
    t = t_axis(dur)
    x = t / dur
    n = rng.standard_normal(len(t))
    sig = onepole_lp(n, 300 + 9000 * x**2) * x**2 * 0.25
    sweep = np.sin(2 * np.pi * np.cumsum(200 + 1400 * x**2) / SR) * x**3 * 0.08
    out = sig + sweep
    return out[::-1] if reverse else out


def impact():
    t = t_axis(3.0)
    boom = np.sin(2 * np.pi * np.cumsum(30 + 60 * np.exp(-t * 6)) / SR) * np.exp(-t * 1.6)
    noise = onepole_lp(rng.standard_normal(len(t)), 1200) * np.exp(-t * 3) * 0.3
    return (boom + noise) * 0.9


# ---------- composição ----------
CHORDS = [  # Am – F – C – G, um acorde por compasso (2 s)
    (["A2", "E3"], ["A3", "C4", "E4"], ["A4", "C5", "E5", "C5"]),
    (["F2", "C3"], ["F3", "A3", "C4"], ["F4", "A4", "C5", "A4"]),
    (["C3", "G3"], ["C4", "E4", "G4"], ["C5", "E5", "G5", "E5"]),
    (["G2", "D3"], ["G3", "B3", "D4"], ["G4", "B4", "D5", "B4"]),
]


def chord_at(sec):
    return CHORDS[int(sec // 2) % 4]


# 0–6 s: piano + pad crescendo, corte seco em 6.0
for i in range(12):
    s = i * BEAT
    notes = chord_at(s)[2]
    add(piano(note_hz(notes[i % 4]), 1.4), s, gain=0.7 + 0.04 * i, pan=-0.3 + 0.15 * (i % 4))
for bar in range(3):
    add(pad([note_hz(n) for n in chord_at(bar * 2)[1]], 2.0, attack=0.8 if bar == 0 else 0.3, release=0.05, bright=900 + 500 * bar), bar * 2, gain=0.6 + 0.3 * bar)
add(riser(4.0), 2.0, gain=0.9)

# corte: zera tudo que vazou depois de 6.0 s
cut = int(6.0 * SR)
L[cut : int(9.0 * SR)] = 0
R[cut : int(9.0 * SR)] = 0

# 6–9 s: silêncio + tique do relógio da "contagem"
for k in range(3):
    add(tick(), 6.25 + k, gain=0.55, bus=(TL, TR))

# 9–12 s: pad volta (crescendo) + riser invertido até o drop
add(pad([note_hz(n) for n in ["A3", "C4", "E4"]], 3.0, attack=2.5, release=0.05, bright=700), 9.0, gain=0.7)
add(riser(3.0), 9.0, gain=1.0)

# 12 s: DROP
add(impact(), 12.0, gain=0.8)


def groove(start, end, kick_on=True, hats_on=True, arp_on=False, cutoff=2200):
    bars = int((end - start) // 2)
    for b in range(bars):
        bs = start + b * 2
        roots, mids, arp = chord_at(bs)
        add(pad([note_hz(n) for n in mids], 2.0, attack=0.05, release=0.1, bright=cutoff), bs, gain=0.55)
        for beat in range(4):
            t0 = bs + beat * BEAT
            if kick_on:
                add(kick(), t0, gain=0.9)
            if beat in (1, 3):
                add(clap(), t0, gain=0.8, pan=0.05)
            if hats_on:
                add(hat(), t0 + BEAT / 2, gain=0.9, pan=0.35)
                add(hat(), t0 + BEAT / 4 * 3, gain=0.4, pan=-0.35)
            # baixo em colcheias com oitava
            for e in range(2):
                f = note_hz(roots[0]) * (2 if (beat * 2 + e) % 4 == 3 else 1)
                add(bass(f, BEAT / 2 - 0.01), t0 + e * BEAT / 2, gain=0.85)
        if arp_on:
            for s16 in range(16):
                f = note_hz(arp[s16 % 4]) * (2 if s16 % 8 >= 4 else 1)
                add(pluck(f), bs + s16 * BEAT / 4, gain=0.9, pan=(-0.6 if s16 % 2 else 0.6))


groove(12, 20, cutoff=2400)  # SONORA + "Zero"
groove(20, 26, arp_on=True, cutoff=4200)  # qualidade
groove(26, 30, kick_on=False, hats_on=False, cutoff=900)  # respiro: dezembro / "aqui é agora"
add(riser(2.0), 30.0, gain=0.9)
groove(30, 32, kick_on=True, hats_on=False, cutoff=1500)
groove(32, 42, arp_on=True, cutoff=3200)  # playlists + gapless

# 42 s: acorde final + impacto do logo
add(impact(), 42.0, gain=0.9)
add(pad([note_hz(n) for n in ["A2", "E3", "A3", "C4", "E4", "B4"]], 6.0, attack=0.02, release=4.5, bright=2600), 42.0, gain=1.0)
add(piano(note_hz("A4"), 3.0), 42.0, gain=0.8)
add(piano(note_hz("E5"), 3.0), 42.25, gain=0.6, pan=0.3)
add(impact(), 45.0, gain=0.35)


# ---------- master ----------
def reverb(x, seconds=2.2, mix=0.18):
    n = int(seconds * SR)
    t = np.arange(n) / SR
    ir = rng.standard_normal(n) * np.exp(-t * 3.2)
    ir[0] = 0
    size = 1 << int(np.ceil(np.log2(len(x) + n)))
    wet = np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(ir, size), size)[: len(x)]
    wet /= np.max(np.abs(wet)) + 1e-9
    return x + wet * mix * np.max(np.abs(x))


L = reverb(L)
R = reverb(R)
# O corte em 6 s é absoluto: some até a cauda do reverb. Só os tiques (com eco próprio) ficam.
L[cut : int(9.0 * SR)] = 0
R[cut : int(9.0 * SR)] = 0
L += reverb(TL, seconds=1.2, mix=0.35)
R += reverb(TR, seconds=1.2, mix=0.35)

stereo = np.stack([L, R], axis=1)
peak = np.max(np.abs(stereo))
stereo = np.tanh(stereo / peak * 1.6) / np.tanh(1.6) * 0.95  # saturação suave = "cola" da mix
fade = int(0.8 * SR)
stereo[-fade:] *= np.linspace(1, 0, fade)[:, None]

os.makedirs("out", exist_ok=True)
pcm = (stereo * 32767).astype(np.int16)
with wave.open("out/trilha.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(pcm.tobytes())
print(f"out/trilha.wav  {DUR:.0f}s  pico={np.max(np.abs(pcm))}")
