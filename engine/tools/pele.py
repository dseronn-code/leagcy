#!/usr/bin/env python3
"""Pele com um scan real (cena `scan`: a cabeça do Lee Perry-Smith, Infinite-Realities, CC BY 3.0). Uso (de dentro de
engine/; os arquivos do scan em assets/LeePerrySmith, do exemplo do three.js):

    python3 tools/pele.py [--bin build/legacy]

O espalhamento subsuperficial separável só redistribui a luz pela pele: a média do rosto com e sem (--no-sss) tem
que ser a mesma em cada canal (conservação de energia), de frente e em contraluz.
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image


def lin(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    a = ap.parse_args()
    pasta = tempfile.mkdtemp()
    for cena in ('scan', 'scan-backlit'):
        medias = []
        for extra in ([], ['--no-sss']):
            out = os.path.join(pasta, f'{cena}{len(extra)}.png')
            subprocess.run([a.bin, '--headless', '--scene', cena, '--pause', '--frames', '32', '--exposure', '13', '--width', '480',
                            '--height', '270', '--out', out] + extra, check=True, capture_output=True)
            medias.append(lin(out)[80:250, 185:290].mean(axis=(0, 1)))  # recorte do rosto
        r = medias[0] / medias[1]
        print(f'{cena:13s} média com SSS / sem: R {r[0]:.3f} G {r[1]:.3f} B {r[2]:.3f}')


if __name__ == '__main__':
    main()
