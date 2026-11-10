#!/usr/bin/env python3
"""Gera os sons do Vale dos Cristais (cenas/vale/sons) por síntese, sem arquivos de fora. Uso (de engine/):

    python3 tools/gerar_sons_vale.py

  - cristal.wav:   brilho (arpejo de sinos subindo, com eco curto).
  - alerta.wav:    o guarda viu o jogador (dois latidos curtos, ruído filtrado com envelope).
  - derrubar.wav:  baque grave (o guarda caindo).
  - vitoria.wav:   fanfarra curta (acorde maior em arpejo).
  - vento.wav:     vento ambiente em laço (ruído filtrado com rajadas lentas; começo e fim casam).
"""
import math
import os
import random
import struct
import wave

TAXA = 22050


def gravar(caminho, amostras):
    pico = max(1e-9, max(abs(a) for a in amostras))
    escala = 0.89 / pico
    with wave.open(caminho, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(TAXA)
        w.writeframes(b''.join(struct.pack('<h', int(max(-1.0, min(1.0, a * escala)) * 32767)) for a in amostras))


def sino(f, dur, t0, total, amp=1.0):
    """Sino: parcial fundamental + inarmônicas, decaimento exponencial."""
    out = [0.0] * total
    for i in range(int(t0 * TAXA), min(total, int((t0 + dur) * TAXA))):
        t = i / TAXA - t0
        env = math.exp(-t * 5.0) * min(1.0, t * 400)
        out[i] = amp * env * (math.sin(2 * math.pi * f * t) + 0.4 * math.sin(2 * math.pi * f * 2.76 * t) +
                              0.2 * math.sin(2 * math.pi * f * 5.4 * t))
    return out


def somar(*listas):
    n = max(len(x) for x in listas)
    return [sum(x[i] if i < len(x) else 0.0 for x in listas) for i in range(n)]


def eco(s, atraso, ganho):
    d = int(atraso * TAXA)
    out = s[:] + [0.0] * d
    for i in range(len(s)):
        out[i + d] += s[i] * ganho
    return out


def passa_baixa(s, corte):
    a = 1.0 - math.exp(-2 * math.pi * corte / TAXA)
    y, out = 0.0, []
    for x in s:
        y += a * (x - y)
        out.append(y)
    return out


def cristal():
    total = int(1.2 * TAXA)
    notas = [1046.5, 1318.5, 1568.0, 2093.0]  # dó, mi, sol, dó (agudos)
    s = somar(*[sino(f, 1.0, 0.07 * k, total, 1.0 - 0.15 * k) for k, f in enumerate(notas)])
    return eco(s, 0.11, 0.35)


def alerta():
    random.seed(3)
    total = int(0.6 * TAXA)
    s = [0.0] * total
    for t0 in (0.0, 0.22):
        for i in range(int(t0 * TAXA), min(total, int((t0 + 0.16) * TAXA))):
            t = i / TAXA - t0
            env = math.sin(math.pi * t / 0.16) ** 2
            f = 520 - 900 * t  # tom que cai, como um latido
            s[i] += env * (0.7 * math.sin(2 * math.pi * f * t) + 0.5 * (random.random() * 2 - 1))
    return passa_baixa(s, 2500)


def derrubar():
    random.seed(5)
    total = int(0.7 * TAXA)
    s = []
    for i in range(total):
        t = i / TAXA
        env = math.exp(-t * 7.0)
        s.append(env * (math.sin(2 * math.pi * (70 - 30 * t) * t) + 0.6 * (random.random() * 2 - 1) * math.exp(-t * 30)))
    return passa_baixa(s, 900)


def vitoria():
    total = int(2.4 * TAXA)
    notas = [(523.25, 0.0), (659.25, 0.15), (783.99, 0.3), (1046.5, 0.45), (783.99, 0.75), (1046.5, 0.9)]
    s = [0.0] * total
    for f, t0 in notas:
        for i in range(int(t0 * TAXA), total):
            t = i / TAXA - t0
            env = math.exp(-t * 2.2) * min(1.0, t * 200)
            s[i] += env * (math.sin(2 * math.pi * f * t) + 0.3 * math.sin(2 * math.pi * 2 * f * t) +
                           0.15 * math.sin(2 * math.pi * 3 * f * t))
    return eco(s, 0.18, 0.3)[:total]


def vento():
    random.seed(11)
    dur = 6.0
    total = int(dur * TAXA)
    ruido = [random.random() * 2 - 1 for _ in range(total)]
    # Duas passadas do filtro (a segunda casa o fim com o começo: o laço não estala).
    s = passa_baixa(ruido + ruido, 400)[total:]
    out = []
    for i, x in enumerate(s):
        t = i / TAXA
        rajada = 0.55 + 0.3 * math.sin(2 * math.pi * t / dur) + 0.15 * math.sin(2 * math.pi * 3 * t / dur + 1.0)
        out.append(x * rajada)
    return out


def main():
    pasta = os.path.join('cenas', 'vale', 'sons')
    os.makedirs(pasta, exist_ok=True)
    for nome, f in (('cristal', cristal), ('alerta', alerta), ('derrubar', derrubar), ('vitoria', vitoria),
                    ('vento', vento)):
        caminho = os.path.join(pasta, f'{nome}.wav')
        gravar(caminho, f())
        print(caminho, os.path.getsize(caminho) // 1024, 'KB')


if __name__ == '__main__':
    main()
