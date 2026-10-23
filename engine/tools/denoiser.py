#!/usr/bin/env python3
"""Compara denoisers (NRD ReLAX x SVGF) em movimento contra uma referência convergida.

    python3 tools/denoiser.py [--bin build/legacy] [--out pasta]

Sponza à noite com a cena congelada (--pause): a câmera anda 1,5 m/s para a frente por 24 frames e o último
frame é comparado com a referência — a mesma pose final, parada, 96 frames no preset Cinema. O erro junta
tudo que um denoiser erra em movimento: ruído que sobra, rastro (ghosting) e detalhe borrado.
  - erro relativo médio por pixel (luminância linear) — quanto menor, melhor;
  - brilhinhos: % de pixels mais de 2x acima da referência (vagalumes).
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image

W, H = 640, 360
CAM, LOOK = np.array([-8.5, 1.6, -0.6]), np.array([5.0, 3.4, 0.4])  # pose inicial do Sponza
VEL, FRAMES = 1.5, 24


def lum(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def rodar(binario, args, out):
    subprocess.run([binario, '--headless', '--pause', '--night', '--exposure', '2', '--width', str(W), '--height', str(H)]
                   + args + ['--out', out], check=True, capture_output=True)
    return lum(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--out')
    a = ap.parse_args()
    pasta = a.out or tempfile.mkdtemp()
    os.makedirs(pasta, exist_ok=True)
    fwd = (LOOK - CAM) / np.linalg.norm(LOOK - CAM)
    fim = CAM + fwd * VEL * FRAMES / 30.0  # o modo headless simula 30 fps
    pose = ['--cam', ','.join(f'{v:.5f}' for v in fim), '--look', ','.join(f'{v:.5f}' for v in fim + fwd)]
    ref_png = os.path.join(pasta, 'referencia.png')
    R = lum(ref_png) if os.path.exists(ref_png) else rodar(a.bin, pose + ['--quality', 'cinema', '--frames', '96'], ref_png)
    valido = R > 0.002
    for d in ('svgf', 'nrd'):
        X = rodar(a.bin, ['--denoiser', d, '--move', str(VEL), '--frames', str(FRAMES)], os.path.join(pasta, f'mov_{d}.png'))
        erro = float((np.abs(X - R)[valido] / (R[valido] + 0.01)).mean())
        vaga = float(100.0 * (X[valido] > 2.0 * R[valido] + 0.02).mean())
        print(f'{d:5s} em movimento: erro relativo médio {erro:.4f}, brilhinhos {vaga:.2f}%', flush=True)


if __name__ == '__main__':
    main()
