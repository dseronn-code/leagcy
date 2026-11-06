#!/usr/bin/env python3
"""Mede a física do cabelo (shaders/hair_sim.comp). Uso (de dentro de engine/):

    python3 tools/cabelo.py [--bin build/legacy]

Roda a cena `hair` (a cabeça vira e balança) e lê as posições simuladas dos fios (LEGACY_HAIR_DUMP):
  - comprimento: cada segmento tem que manter o comprimento de repouso (fio inextensível);
  - colisão: nenhum ponto dentro do couro (elipsoide de raio 1,02 no espaço do objeto);
  - movimento secundário: as pontas não seguem a cabeça rigidamente (distância à pose rígida cresce da raiz à ponta).
"""
import argparse
import os
import struct
import subprocess
import tempfile

import numpy as np


def ler(path):
    with open(path, 'rb') as f:
        strands, points = struct.unpack('II', f.read(8))
        model = np.frombuffer(f.read(64), np.float32).reshape(4, 4).T  # glm é coluna-major
        n = strands * points
        rest = np.frombuffer(f.read(16 * n), np.float32).reshape(strands, points, 4)
        pos = np.frombuffer(f.read(16 * n), np.float32).reshape(strands, points, 4)
    return model, rest[..., :3], pos[..., :3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--frames', type=int, default=40)
    a = ap.parse_args()
    pasta = tempfile.mkdtemp()
    dump = os.path.join(pasta, 'cabelo.bin')
    subprocess.run([a.bin, '--headless', '--scene', 'hair', '--frames', str(a.frames), '--width', '160', '--height', '90',
                    '--out', os.path.join(pasta, 'c.png')], check=True, capture_output=True,
                   env=dict(os.environ, LEGACY_HAIR_DUMP=dump))
    model, rest, pos = ler(dump)
    restW = rest @ model[:3, :3].T + model[:3, 3]  # pose rígida: o repouso levado pela cabeça
    segRest = np.linalg.norm(np.diff(restW, axis=1), axis=2)
    segNow = np.linalg.norm(np.diff(pos, axis=1), axis=2)
    erroComp = np.abs(segNow / segRest - 1.0)
    inv = np.linalg.inv(model)
    po = pos @ inv[:3, :3].T + inv[:3, 3]
    r = np.linalg.norm(po, axis=2)
    dentro = (r[:, 2:] < 1.0).mean()  # a pele fica no raio ~0,97; o couro em 1,02
    desvio = np.linalg.norm(pos - restW, axis=2).mean(axis=0) * 1000.0  # mm, por ponto ao longo do fio
    print(f'comprimento: erro médio {erroComp.mean() * 100:.3f}%, máximo {erroComp.max() * 100:.3f}%')
    print(f'pontos dentro da cabeça (raio < 1,0): {dentro * 100:.3f}%')
    print('distância à pose rígida (mm), raiz -> ponta: ' + ' '.join(f'{d:.1f}' for d in desvio))


if __name__ == '__main__':
    main()
