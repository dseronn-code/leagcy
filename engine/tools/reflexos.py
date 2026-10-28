#!/usr/bin/env python3
"""Mede reflexos e luz indireta contra a verdade por path tracing. Uso (de dentro de engine/):

    python3 tools/reflexos.py [--bin build/legacy] [--out pasta] [--ref-frames 2048] [--extra "..."]

Referência: --reference-pt (sem denoiser, rebotes por path tracing até 8, média de N frames com a câmera parada) —
a imagem fisicamente correta. Motor: configuração padrão convergida parada (64 frames). Exposição manual igual.
Por cena:
  - erro: diferença relativa média por pixel (luminância linear, imagens reduzidas 2x para tirar o ruído que sobra na
    referência) — menor é melhor;
  - viés: (média do motor / média da referência − 1) — luz a mais (+) ou a menos (−) no geral;
  - erro no espelho / nas esferas (cena reflections): só os pixels do piso espelhado e das esferas de metal.
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image

W, H = 480, 270
CENAS = {  # nome: argumentos (exposição manual: as duas imagens com a mesma)
    'reflections': ['--scene', 'reflections', '--exposure', '12'],
    'sponza': ['--exposure', '12'],
    'sponza-noite': ['--night', '--exposure', '2'],
}


def lum(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    L = a @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    return L.reshape(H // 2, 2, W // 2, 2).mean(axis=(1, 3))


def rodar(binario, args, out):
    subprocess.run([binario, '--headless', '--pause', '--no-motion-blur', '--width', str(W), '--height', str(H)] + args
                   + ['--out', out], check=True, capture_output=True)
    return lum(out)


def erro_rel(X, R, m):
    return float((np.abs(X - R)[m] / (R[m] + 0.01)).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--out')
    ap.add_argument('--ref-frames', type=int, default=2048)
    ap.add_argument('--frames', type=int, default=64, help='frames do motor (parado)')
    ap.add_argument('--cenas', default=','.join(CENAS))
    ap.add_argument('--extra', default='', help='argumentos extras só para o motor')
    a = ap.parse_args()
    pasta = a.out or tempfile.mkdtemp()
    os.makedirs(pasta, exist_ok=True)
    for cena in a.cenas.split(','):
        args = CENAS[cena]
        npy = os.path.join(pasta, f'ref_{cena}_{a.ref_frames}.npy')
        if not os.path.exists(npy):
            np.save(npy, rodar(a.bin, args + ['--reference-pt', '--frames', str(a.ref_frames)], os.path.join(pasta, f'ref_{cena}.png')))
        R = np.load(npy)
        X = rodar(a.bin, args + ['--frames', str(a.frames)] + a.extra.split(), os.path.join(pasta, f'motor_{cena}.png'))
        valido = R > 0.002
        linha = f'{cena:13s} erro {erro_rel(X, R, valido):.4f}  viés {100 * (X[valido].mean() / R[valido].mean() - 1):+.1f}%'
        if cena == 'reflections':  # regiões em pixels da imagem reduzida (câmera fixa da cena)
            h, w = R.shape
            yy, xx = np.mgrid[0:h, 0:w]
            espelho = valido & (yy > 0.62 * h) & (yy < 0.80 * h) & (xx > 0.36 * w) & (xx < 0.64 * w)
            esferas = valido & (yy > 0.47 * h) & (yy < 0.62 * h) & (xx > 0.32 * w) & (xx < 0.68 * w)
            linha += f'  |  espelho {erro_rel(X, R, espelho):.4f}  esferas {erro_rel(X, R, esferas):.4f}'
        print(linha, flush=True)


if __name__ == '__main__':
    main()
