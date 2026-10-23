#!/usr/bin/env python3
"""Mede a qualidade da sombra do sol nas cenas `shadows` (origem) e `shadows-far` (a 4 km da origem).

    python3 tools/sombras.py [--bin build/legacy] [--out pasta] [--sol 20,3]

Usa as visualizações de depuração (valor cru, sem denoiser nem TAA):
  - sombra falsa no terminator: % da luz do sol (Σ N·L) que as esferas deveriam receber e que a sombra
    apaga (nada faz sombra nelas). Esferas de 6, 8, 12, 16 e 24 segmentos.
  - acne: % dos pixels do chão em primeiro plano (sem nada que faça sombra) com visibilidade < 1.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

W, H = 1280, 720


def render(binario, cena, vista, elev, out):
    subprocess.run([binario, '--headless', '--scene', cena, '--debug-view', vista, '--sun-elevation', str(elev), '--sun-azimuth', '45',
                    '--width', str(W), '--height', str(H), '--frames', '2', '--out', out],
                   check=True, capture_output=True)
    a = np.asarray(Image.open(out).convert('L')).astype(np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def projetar(p, cam, alvo):
    f = alvo - cam; f /= np.linalg.norm(f)
    r = np.cross(f, [0, 1, 0]); r /= np.linalg.norm(r)
    u = np.cross(r, f)
    d = p - cam
    z = d @ f
    t = 1.0 / np.tan(np.radians(30))
    x = (d @ r) / z * t / (W / H); y = (d @ u) / z * t
    return (x * 0.5 + 0.5) * W, (0.5 - y * 0.5) * H, z


def mascaras_esferas():
    cam, alvo = np.array([0, 3, 13.0]), np.array([0, 0.8, 0.0])
    yy, xx = np.mgrid[0:H, 0:W]
    out = []
    for i in range(5):  # a 6ª (64 segmentos) fica parcialmente atrás da caixa
        c = np.array([-7.5 + 3 * i, 1.4, 0.0])
        px, py, z = projetar(c, cam, alvo)
        rpx = 1.0 / z / np.tan(np.radians(30)) * 0.5 * H
        out.append(np.hypot(xx - px, yy - py) < 0.85 * rpx)
    return out


def medir(binario, cena, elev, pasta):
    vis = render(binario, cena, 'sol', elev, os.path.join(pasta, f'{cena}_{elev}_sol.png'))
    nl = render(binario, cena, 'nl', elev, os.path.join(pasta, f'{cena}_{elev}_nl.png'))
    falsas = []
    for m in mascaras_esferas():  # energia perdida: luz que o sombreamento suave recebe mas a sombra apaga
        falsas.append(100.0 * (nl * (1.0 - vis))[m].sum() / max(nl[m].sum(), 1e-6))
    chao = np.zeros_like(vis, bool); chao[int(0.85 * H):, int(0.75 * W):] = True  # canto sem sombras projetadas
    acne = 100.0 * ((vis < 0.999) & chao & (nl > 0.0)).sum() / max((chao & (nl > 0.0)).sum(), 1)
    return falsas, acne


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--out')
    ap.add_argument('--sol', default='20,3', help='elevações do sol em graus')
    a = ap.parse_args()
    pasta = a.out or tempfile.mkdtemp()
    os.makedirs(pasta, exist_ok=True)
    pior_falsa, pior_acne = 0.0, 0.0
    for cena in ('shadows', 'shadows-far'):
        for elev in a.sol.split(','):
            falsas, acne = medir(a.bin, cena, elev, pasta)
            pior_falsa, pior_acne = max(pior_falsa, max(falsas)), max(pior_acne, acne)
            print(f'{cena:12s} sol {elev:>3s}°  luz perdida nas esferas (6/8/12/16/24 seg.): '
                  + ' '.join(f'{f:5.1f}%' for f in falsas) + f'   acne no chão: {acne:.3f}%')
    print(f'PIOR: luz perdida {pior_falsa:.1f}%, acne {pior_acne:.3f}%')
    sys.exit(0)


if __name__ == '__main__':
    main()
