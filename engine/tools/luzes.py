#!/usr/bin/env python3
"""Mede as luzes de malha (triângulos emissivos amostrados como luz). Uso (de dentro de engine/):

    python3 tools/luzes.py [--bin build/legacy] [--out pasta] [--ref-frames 1024] [--cenas mesh-lights,bistro-noite]

Duas medições por cena, todas com exposição manual igual:
  - prova de energia: referência por path tracing COM luzes de malha x SEM (--no-mesh-lights: a luz dos emissivos só
    chega por raios que batem neles, o caminho ingênuo e sem viés). As duas têm que convergir para a mesma imagem;
    "viés" diferente de 0 seria luz contada a mais ou a menos pelas luzes de malha;
  - motor: configuração padrão convergida parada (64 frames) contra a referência com luzes de malha.
Erro: diferença relativa média por pixel (luminância linear, imagens reduzidas 4x para tirar o ruído que sobra).
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image

W, H = 480, 272
CENAS = {  # nome: argumentos
    'mesh-lights': ['--scene', 'mesh-lights', '--exposure', '6'],
    'bistro-noite': ['--scene', os.path.join('assets', 'Bistro', 'bistro.gltf'), '--night', '--exposure', '1'],
}


def lum(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    L = a @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    return L.reshape(H // 4, 4, W // 4, 4).mean(axis=(1, 3))


def rodar(binario, args, out):
    subprocess.run([binario, '--headless', '--pause', '--no-motion-blur', '--width', str(W), '--height', str(H)] + args
                   + ['--out', out], check=True, capture_output=True)
    return lum(out)


def comparar(X, R):
    m = R > 0.002
    return float((np.abs(X - R)[m] / (R[m] + 0.01)).mean()), 100 * (X[m].mean() / R[m].mean() - 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--out')
    ap.add_argument('--ref-frames', type=int, default=1024)
    ap.add_argument('--cenas', default=','.join(CENAS))
    a = ap.parse_args()
    pasta = a.out or tempfile.mkdtemp()
    os.makedirs(pasta, exist_ok=True)
    for cena in a.cenas.split(','):
        args = CENAS[cena]
        com = rodar(a.bin, args + ['--reference-pt', '--frames', str(a.ref_frames)], os.path.join(pasta, f'luzes_{cena}_ref.png'))
        # Sem amostragem de luz o ruído é bem maior: 4x mais frames.
        sem = rodar(a.bin, args + ['--reference-pt', '--no-mesh-lights', '--frames', str(4 * a.ref_frames)],
                    os.path.join(pasta, f'luzes_{cena}_ref_sem.png'))
        motor = rodar(a.bin, args + ['--frames', '64'], os.path.join(pasta, f'luzes_{cena}_motor.png'))
        e1, v1 = comparar(com, sem)
        e2, v2 = comparar(motor, com)
        print(f'{cena:13s} prova (com x sem luzes de malha): diferença {e1:.4f}, viés {v1:+.1f}%  |  '
              f'motor x referência: erro {e2:.4f}, viés {v2:+.1f}%', flush=True)


if __name__ == '__main__':
    main()
