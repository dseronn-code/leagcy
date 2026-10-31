#!/usr/bin/env python3
"""Mede a colisão do tecido consigo mesmo (shaders/cloth.comp). Uso (de dentro de engine/):

    python3 tools/tecido.py [--bin build/legacy] [--frames 150]

Roda a cena `cloth` (toalha caindo sobre uma bola e o chão) com e sem a colisão própria (LEGACY_NO_CLOTH_SELF) e lê as
posições finais do pano (LEGACY_CLOTH_DUMP). Entre pontos que não são vizinhos na grade (mais de 2 de distância):
menor distância e quantos pares estão a menos de meio espaçamento (pano atravessando a si mesmo).
"""
import argparse
import os
import struct
import subprocess
import tempfile

import numpy as np


def medir(binario, frames, sem):
    pasta = tempfile.mkdtemp()
    dump = os.path.join(pasta, 'pano.bin')
    env = dict(os.environ, LEGACY_CLOTH_DUMP=dump)
    if sem:
        env['LEGACY_NO_CLOTH_SELF'] = '1'
    subprocess.run([binario, '--headless', '--scene', 'cloth', '--frames', str(frames), '--width', '160', '--height', '90',
                    '--out', os.path.join(pasta, 'p.png')], check=True, capture_output=True, env=env)
    with open(dump, 'rb') as f:
        w, h = struct.unpack('II', f.read(8))
        spacing = struct.unpack('f', f.read(4))[0]
        p = np.frombuffer(f.read(16 * w * h), np.float32).reshape(w * h, 4)[:, :3]
    gx, gy = np.arange(w * h) % w, np.arange(w * h) // w
    far = np.maximum(np.abs(gx[:, None] - gx[None, :]), np.abs(gy[:, None] - gy[None, :])) > 2
    d = np.linalg.norm(p[:, None, :] - p[None, :, :], axis=2)
    dd = d[far]
    return dd.min() / spacing, int((dd < 0.5 * spacing).sum() // 2), int((dd < 0.8 * spacing).sum() // 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--frames', type=int, default=150)
    a = ap.parse_args()
    for nome, sem in (('sem colisão própria', True), ('com colisão própria', False)):
        mn, cruz, perto = medir(a.bin, a.frames, sem)
        print(f'{nome:20s}: menor distância {mn:.2f} espaçamento, pares < 0,5: {cruz}, pares < 0,8: {perto}')


if __name__ == '__main__':
    main()
