#!/usr/bin/env python3
"""Mede nitidez e fidelidade do TAA/upscaling contra uma referência supersampleada.

    python3 tools/taa.py [--bin build/legacy] [--out pasta] [--scale 1.0] [--extra "--upscaler dlss"]

Sponza de dia, cena congelada, exposição manual, sem motion blur. A referência é a mesma imagem renderizada
com 4x os pixels (2x em cada eixo) e reduzida por média de caixa — o "ground truth" das bordas e texturas.
  - nitidez: energia de gradiente da imagem / da referência (1,00 = tão nítida quanto; < 1 = mole; > 1 = ruído
    ou serrilhado a mais);
  - erro: diferença média absoluta relativa por pixel (luminância linear).
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image

W, H = 640, 360
BASE = ['--headless', '--pause', '--exposure', '12', '--no-motion-blur', '--frames', '32']


def lum(a):
    a = np.asarray(a).astype(np.float32) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def render(binario, w, h, extra, out):
    subprocess.run([binario] + BASE + ['--width', str(w), '--height', str(h), '--out', out] + extra,
                   check=True, capture_output=True)
    return Image.open(out).convert('RGB')


def gradiente(L):
    gx, gy = np.diff(L, axis=1)[:-1, :], np.diff(L, axis=0)[:, :-1]
    return np.sqrt(gx * gx + gy * gy).mean()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--out')
    ap.add_argument('--scale', default='1.0', help='resolução interna (TAAU) da imagem testada')
    ap.add_argument('--extra', default='', help='argumentos extras para a imagem testada')
    a = ap.parse_args()
    pasta = a.out or tempfile.mkdtemp()
    os.makedirs(pasta, exist_ok=True)
    ref_png = os.path.join(pasta, 'ref_2x.png')
    ref = Image.open(ref_png) if os.path.exists(ref_png) else render(a.bin, 2 * W, 2 * H, ['--sharpness', '0'], ref_png)
    R = lum(ref).reshape(H, 2, W, 2).mean(axis=(1, 3))  # caixa 2x2 em luminância linear
    nome = f"teste_{a.scale}_{a.extra.replace(' ', '_').replace('-', '')}.png"
    X = lum(render(a.bin, W, H, ['--scale', a.scale] + a.extra.split(), os.path.join(pasta, nome)))
    nit = gradiente(X) / gradiente(R)
    erro = float((np.abs(X - R) / (R + 0.02)).mean())
    print(f'escala {a.scale} {a.extra}: nitidez {nit:.3f}  erro {erro:.4f}', flush=True)


if __name__ == '__main__':
    main()
