#!/usr/bin/env python3
"""Compara denoisers em movimento e parado. Uso (de dentro de engine/):

    python3 tools/denoiser.py [--bin build/legacy] [--out pasta] [--candidatos svgf,relax,dlss-rr]

Sponza à noite (512 luzes, ReSTIR) com a cena congelada (--pause):
  - EM MOVIMENTO: a câmera anda 1,5 m/s por 24 frames e o último frame é comparado com a referência — mesma
    trajetória, depois parada por 96 frames (média do SVGF e do NRD ReLAX convergidos, para não favorecer nenhum).
    O erro junta tudo que um denoiser erra em movimento: ruído que sobra, rastro (ghosting) e detalhe borrado.
      erro: diferença relativa média por pixel (luminância linear) — menor é melhor;
      brilhinhos: % de pixels mais de 2x acima da referência (vagalumes).
  - PARADO: 40 frames com a câmera parada; nos 16 últimos mede a cintilação (desvio/média por pixel) e os
    brilhinhos (pixels que piscam 2x acima da mediana).
dlss-rr (DLSS Ray Reconstruction) só roda em GPU RTX; sem ela o motor volta ao SVGF (o resultado sai igual ao dele).
"""
import argparse
import glob
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image

W, H = 640, 360
VEL, FRAMES = 1.5, 24
BASE = ['--headless', '--pause', '--night', '--exposure', '2', '--width', str(W), '--height', str(H)]


def lum(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def rodar(binario, args, out):
    subprocess.run([binario] + BASE + args + ['--out', out], check=True, capture_output=True)
    return lum(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--out')
    ap.add_argument('--candidatos', default='svgf,relax')
    a = ap.parse_args()
    pasta = a.out or tempfile.mkdtemp()
    os.makedirs(pasta, exist_ok=True)
    candidatos = a.candidatos.split(',')

    # Referência: anda os mesmos 24 frames e para; depois converge parada (mesma pose final por construção).
    ref_png = os.path.join(pasta, 'referencia')
    mover = ['--move', str(VEL), '--move-stop', str(FRAMES)]
    if os.path.exists(ref_png + '.npy'):
        R = np.load(ref_png + '.npy')
    else:
        conv = [rodar(a.bin, mover + ['--denoiser', d, '--frames', str(FRAMES + 96)], os.path.join(pasta, f'conv_{d}.png'))
                for d in ('svgf', 'relax')]
        R = 0.5 * (conv[0] + conv[1])
        np.save(ref_png + '.npy', R)
    valido = R > 0.002

    for d in candidatos:
        X = rodar(a.bin, ['--denoiser', d, '--move', str(VEL), '--frames', str(FRAMES)], os.path.join(pasta, f'mov_{d}.png'))
        erro = float((np.abs(X - R)[valido] / (R[valido] + 0.01)).mean())
        vaga = float(100.0 * (X[valido] > 2.0 * R[valido] + 0.02).mean())

        sub = os.path.join(pasta, f'parado_{d}')
        os.makedirs(sub, exist_ok=True)
        for f in glob.glob(os.path.join(sub, '*.png')):
            os.remove(f)
        rodar(a.bin, ['--denoiser', d, '--frames', '40', '--save-every', '1'], os.path.join(sub, 'f.png'))
        os.replace(os.path.join(sub, 'f.png'), os.path.join(sub, 'f_0040.png'))
        L = np.stack([lum(f) for f in sorted(glob.glob(os.path.join(sub, 'f_*.png')))[-16:]])
        cint = float((L.std(0) / (L.mean(0) + 0.01)).mean())
        brilho = float(100 * (L > 2.0 * np.median(L, 0) + 0.02).any(0).mean())
        print(f'{d:8s} andando: erro {erro:.4f}, brilhinhos {vaga:.2f}%  |  parado: cintilação {cint:.4f}, '
              f'brilhinhos {brilho:.2f}%', flush=True)


if __name__ == '__main__':
    main()
