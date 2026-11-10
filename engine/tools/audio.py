#!/usr/bin/env python3
"""Testes do áudio 3D (miniaudio na camada de jogo). Uso (de dentro de engine/):

    python3 tools/audio.py [--bin build/legacy]

Um tom de 440 Hz em laço, preso a uma entidade, gravado com --audio-wav (ouvinte = câmera olhando para -z):
  - distancia: a 2 m e a 8 m à frente; a atenuação inversa (dist_min 1 m) dá ganho 0,5 e 0,125 => razão 4 (±5%).
  - direcao: a 4 m à direita, o canal direito fica bem mais alto que o esquerdo (> 2x).
  - silencio: sem som na cena, o arquivo sai mudo (nada vaza).
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave


def tom(caminho, hz=440.0, segundos=1.0, taxa=48000):
    with wave.open(caminho, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(taxa)
        dados = b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * hz * i / taxa)))
                         for i in range(int(segundos * taxa)))
        w.writeframes(dados)


def gravar(binario, pasta, nome, entidades):
    cena = {'camera': {'posicao': [0, 1, 0], 'alvo': [0, 1, -10]}, 'entidades': entidades}
    caminho = os.path.join(pasta, f'{nome}.json')
    with open(caminho, 'w', encoding='utf-8') as f:
        json.dump(cena, f)
    wav = os.path.join(pasta, f'{nome}.wav')
    subprocess.run([binario, '--headless', '--scene', caminho, '--width', '32', '--height', '18', '--frames', '45',
                    '--out', os.path.join(pasta, f'{nome}.png'), '--audio-wav', wav], check=True, capture_output=True)
    with wave.open(wav, 'rb') as w:
        n = w.getnframes()
        amostras = struct.unpack(f'<{2 * n}h', w.readframes(n))
    esq, dir_ = amostras[0::2], amostras[1::2]
    meio = len(esq) // 3  # descarta o começo (o som começa no primeiro quadro)
    rms = lambda xs: math.sqrt(sum(x * x for x in xs[meio:]) / max(len(xs) - meio, 1))
    return rms(esq), rms(dir_)


def fonte(som, pos):
    return {'nome': 'fonte', 'forma': 'esfera', 'raio': 0.1, 'posicao': pos,
            'som': {'arquivo': som, 'laco': True, 'dist_min': 1.0, 'queda': 1.0}}


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    a = ap.parse_args()
    ok_todos = True
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as pasta:
        som = os.path.join(pasta, 'tom.wav')
        tom(som)
        l2, r2 = gravar(a.bin, pasta, 'perto', [fonte(som, [0, 1, -2])])
        l8, r8 = gravar(a.bin, pasta, 'longe', [fonte(som, [0, 1, -8])])
        razao = (l2 + r2) / max(l8 + r8, 1e-9)
        ok = abs(razao - 4.0) < 0.2
        ok_todos &= ok
        print(f'distancia  {"PASSOU" if ok else "FALHOU"}: volume a 2 m / a 8 m = {razao:.3f} (esperado 4,0)')
        lr, rr = gravar(a.bin, pasta, 'direita', [fonte(som, [4, 1, 0])])
        ok = rr > 2.0 * lr
        ok_todos &= ok
        print(f'direcao    {"PASSOU" if ok else "FALHOU"}: fonte à direita: canal direito {rr:.0f}, esquerdo {lr:.0f}')
        ls, rs = gravar(a.bin, pasta, 'silencio', [{'nome': 'caixa', 'forma': 'caixa', 'posicao': [0, 1, -2]}])
        ok = ls < 1.0 and rs < 1.0
        ok_todos &= ok
        print(f'silencio   {"PASSOU" if ok else "FALHOU"}: sem fonte, RMS {ls:.2f} / {rs:.2f}')
    sys.exit(0 if ok_todos else 1)


if __name__ == '__main__':
    main()
