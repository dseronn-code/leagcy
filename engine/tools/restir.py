#!/usr/bin/env python3
"""Mede ruído e viés do ReSTIR contra a referência por força bruta (todas as luzes, com sombra).

    python3 tools/restir.py [--bin build/legacy] [--out pasta] [--quality ultra]

Cenas noturnas congeladas (--pause): Sponza (512 luzes num átrio) e externa (luzes espalhadas por 50 m).
  - relMSE de 1 frame: erro quadrático relativo médio de cada frame cru (sem denoiser) — o ruído que o
    denoiser precisa limpar. Quanto menor, melhor.
  - viés: diferença entre a média de 16 frames e a referência, no brilho total da imagem (o ideal é 0%).
"""
import argparse
import glob
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image

W, H = 480, 270
CENAS = {'sponza': ['--night'], 'externa': ['--scene', 'outdoor', '--night']}
EXPOSICAO = {'sponza': '2', 'externa': '0'}


def lum(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    a = np.minimum(a, 254.5 / 255.0)
    a = a / (1.0 - a)  # o motor grava v/(1+v) para não saturar
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def rodar(binario, args):
    subprocess.run([binario, '--headless', '--pause', '--width', str(W), '--height', str(H)] + args,
                   check=True, capture_output=True)


def medir(binario, cena, pasta, extra=()):
    base = CENAS[cena] + ['--exposure', EXPOSICAO[cena]] + list(extra)
    ref = os.path.join(pasta, f'{cena}_ref.png')
    if not os.path.exists(ref):
        rodar(binario, base + ['--debug-view', 'luzes-ref', '--frames', '1', '--out', ref])
    R = lum(ref)
    d = os.path.join(pasta, f'{cena}_restir')
    os.makedirs(d, exist_ok=True)
    for f in glob.glob(os.path.join(d, '*.png')):
        os.remove(f)
    rodar(binario, base + ['--debug-view', 'luzes', '--frames', '24', '--save-every', '1', '--out', os.path.join(d, 'f.png')])
    quadros = sorted(glob.glob(os.path.join(d, 'f_*.png')))[-16:]
    X = np.stack([lum(f) for f in quadros])
    valido = R > 0.002
    X = np.minimum(X, 50.0 * R.max())  # pixels saturados no PNG (raros) não dominam a média
    relmse = float((((X - R) ** 2)[:, valido] / (R[valido] ** 2 + 1e-2)).mean())
    vies = float(100.0 * (X.mean(0)[valido].sum() - R[valido].sum()) / R[valido].sum())
    return relmse, vies


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--out')
    ap.add_argument('--quality', default='ultra')
    a = ap.parse_args()
    pasta = a.out or tempfile.mkdtemp()
    os.makedirs(pasta, exist_ok=True)
    for cena in CENAS:
        relmse, vies = medir(a.bin, cena, pasta, ['--quality', a.quality])
        print(f'{cena:8s} relMSE de 1 frame: {relmse:.4f}   viés: {vies:+.2f}%', flush=True)


if __name__ == '__main__':
    main()
