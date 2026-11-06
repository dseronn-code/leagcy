#!/usr/bin/env python3
"""Mede as nuvens do motor (clouds.glsl) contra a referência por Monte Carlo volumétrico (clouds_reference.comp,
--clouds-reference: espalhamento múltiplo exato com a mesma densidade, fase, sol e céu). Uso (de dentro de engine/):

    python3 tools/nuvens.py [--bin build/legacy] [--ref-frames 1024]

Cena externa com nuvens a 50%, câmera olhando o céu, exposição fixa; compara a faixa de céu em blocos (tira o ruído
que sobra na referência): viés de luminância, erro relativo médio e correlação.
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image

ARGS = ['--headless', '--scene', 'outdoor', '--clouds', '0.5', '--pause', '--exposure', '13', '--no-lens-flare',
        '--no-motion-blur', '--cam', '0,3,0', '--look', '0,60,-100', '--width', '160', '--height', '90']


def lum(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    y = a[:48] @ np.array([0.2126, 0.7152, 0.0722])  # só o céu (sem postes nem chão)
    return y.reshape(8, 6, 20, 8).mean(axis=(1, 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--ref-frames', type=int, default=1024)
    a = ap.parse_args()
    pasta = tempfile.mkdtemp()
    ref, eng = os.path.join(pasta, 'ref.png'), os.path.join(pasta, 'motor.png')
    subprocess.run([a.bin] + ARGS + ['--frames', str(a.ref_frames), '--clouds-reference', '--out', ref], check=True, capture_output=True)
    subprocess.run([a.bin] + ARGS + ['--frames', '64', '--out', eng], check=True, capture_output=True)
    r, e = lum(ref), lum(eng)
    print(f'viés {100 * (e.mean() / r.mean() - 1):+.1f}%, erro relativo {(np.abs(e - r) / (r + 0.01)).mean():.3f}, '
          f'correlação {np.corrcoef(e.ravel(), r.ravel())[0, 1]:.2f}')


if __name__ == '__main__':
    main()
