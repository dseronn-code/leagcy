#!/usr/bin/env python3
"""Mede a névoa do motor (froxels de fog_inject/fog_integrate + trecho analítico além de FOG_FAR) contra a referência
por pixel (fog_reference.comp, --fog-reference: integral de espalhamento simples sem grade, transmitância exata, sol
com raio de sombra em cada amostra e céu amostrado pela mesma fase). Uso (de dentro de engine/):

    python3 tools/nevoa.py [--bin build/legacy] [--ref-frames 256]

O resto da imagem é o mesmo nos três renders (mesma iluminação e mesmo ruído por frame); a diferença é só a névoa.
Mede em blocos de 8×8 (tira o ruído que sobra na referência): viés de luminância, e o erro relativo ao efeito da
névoa (|motor − ref| / |ref − sem névoa|: 0 = igual à referência, 1 = tão errado quanto não ter névoa).
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image

BASE = ['--headless', '--pause', '--no-lens-flare', '--no-motion-blur', '--no-bloom', '--width', '160', '--height', '96']
CASOS = [
    ('sponza_raios_de_luz', ['--fog', '0.02', '--exposure', '12']),
    ('externa_longe', ['--scene', 'outdoor', '--fog', '0.004', '--exposure', '14', '--sun-elevation', '25',
                       '--cam', '0,3,20', '--look', '0,8,-60']),
]


def lum(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    y = a @ np.array([0.2126, 0.7152, 0.0722])
    h, w = y.shape
    return y[:h // 8 * 8, :w // 8 * 8].reshape(h // 8, 8, w // 8, 8).mean(axis=(1, 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--ref-frames', type=int, default=256)
    ap.add_argument('--pasta', default=tempfile.mkdtemp())
    ap.add_argument('--caso', help='só este caso (nome)')
    a = ap.parse_args()
    for nome, args in CASOS:
        if a.caso and nome != a.caso:
            continue
        arq = {k: os.path.join(a.pasta, f'{nome}_{k}.png') for k in ('ref', 'motor', 'sem')}
        run = lambda extra, out: subprocess.run([a.bin] + BASE + args + extra + ['--out', out], check=True,
                                                capture_output=True)
        run(['--frames', str(a.ref_frames), '--fog-reference'], arq['ref'])
        run(['--frames', str(a.ref_frames)], arq['motor'])
        semArgs = ['--fog', '0'] if '--fog' in args else []
        subprocess.run([a.bin] + BASE + [x for i, x in enumerate(args) if not (x == '--fog' or (i and args[i - 1] == '--fog'))]
                       + semArgs + ['--frames', str(a.ref_frames), '--out', arq['sem']], check=True, capture_output=True)
        r, e, s = lum(arq['ref']), lum(arq['motor']), lum(arq['sem'])
        efeito = np.abs(r - s)
        m = efeito > 0.01 * r.max()  # só onde a névoa muda a imagem de forma visível
        print(f'{nome}: viés {100 * (e.mean() / r.mean() - 1):+.1f}%, erro/efeito da névoa '
              f'{(np.abs(e - r)[m]).sum() / efeito[m].sum():.3f} ({m.sum()} blocos), '
              f'correlação {np.corrcoef(e.ravel(), r.ravel())[0, 1]:.3f}')


if __name__ == '__main__':
    main()
