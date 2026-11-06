#!/usr/bin/env python3
"""Compara denoisers em movimento e parado. Uso (de dentro de engine/):

    python3 tools/denoiser.py [--bin build/legacy] [--out pasta] [--candidatos svgf,relax,dlss-rr]

Sponza à noite (512 luzes, ReSTIR) com a cena congelada (--pause):
Referência = VERDADE por acumulação (--reference): sem denoiser, média de N frames (1024 por padrão) da
iluminação ruidosa com a câmera parada na mesma pose — converge para a imagem correta sem favorecer nenhum denoiser.
  - EM MOVIMENTO: a câmera anda 1,5 m/s por 24 frames; o último frame contra a referência da pose final.
    O erro junta tudo que um denoiser erra em movimento: ruído que sobra, rastro (ghosting) e detalhe borrado.
      erro: diferença relativa média por pixel (luminância linear) — menor é melhor;
      brilhinhos: % de pixels mais de 2x acima da referência (vagalumes).
  - PARADO: 40 frames com a câmera parada; erro do último frame contra a referência da pose inicial (detalhe
    borrado e viés), e nos 16 últimos a cintilação (desvio/média por pixel) e os brilhinhos (pixels que piscam).
  - Cena animada (--cena sponza-noite-animada): os personagens e luzes se movem durante a medição; a referência
    congela a animação no instante do frame medido (--pause-at) — mede rastros e desoclusões de objetos.
    (A cintilação dela mistura movimento de verdade; vale o erro.)
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
BASE = ['--headless', '--no-motion-blur', '--width', str(W), '--height', str(H)]
CENAS = {'sponza-noite': ['--pause', '--night', '--exposure', '2'],
         'reflections': ['--pause', '--scene', 'reflections', '--exposure', '12'],
         # Câmera andando E personagens animados (raposa, CesiumMan, luzes que se movem): desoclusões e rastros.
         'sponza-noite-animada': ['--night', '--exposure', '2']}
ANIMADA = {'sponza-noite-animada'}


def lum(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32)


CENA = CENAS['sponza-noite']


def rodar(binario, args, out):
    subprocess.run([binario] + BASE + CENA + args + ['--out', out], check=True, capture_output=True)
    return lum(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--out')
    ap.add_argument('--candidatos', default='svgf,relax')
    ap.add_argument('--ref-frames', type=int, default=1024, help='frames acumulados na referência')
    ap.add_argument('--cena', default='sponza-noite', choices=list(CENAS))
    ap.add_argument('--extra', default='', help='argumentos extras para os candidatos (use --extra=...)')
    a = ap.parse_args()
    global CENA
    CENA = CENAS[a.cena]
    pasta = a.out or tempfile.mkdtemp()
    os.makedirs(pasta, exist_ok=True)
    candidatos = a.candidatos.split(',')

    # Referências: a da pose final do trajeto (anda os mesmos 24 frames e para) e a da pose inicial (parado).
    mover = ['--move', str(VEL), '--move-stop', str(FRAMES)]

    def referencia(nome, args, frames):
        npy = os.path.join(pasta, f'ref_{a.cena}_{nome}_{a.ref_frames}.npy')
        if not os.path.exists(npy):
            np.save(npy, rodar(a.bin, args + ['--reference', '--frames', str(frames)], os.path.join(pasta, f'ref_{nome}.png')))
        return np.load(npy)

    # Cena animada: a referência congela a animação no mesmo instante do frame medido (--pause-at) e só então acumula.
    anim = a.cena in ANIMADA
    R = referencia('andando', mover + (['--pause-at', str(FRAMES)] if anim else []), FRAMES + a.ref_frames)
    R0 = referencia('parado', ['--pause-at', '40'] if anim else [], (40 if anim else 0) + a.ref_frames)
    valido, valido0 = R > 0.002, R0 > 0.002

    for d in candidatos:
        X = rodar(a.bin, ['--denoiser', d, '--move', str(VEL), '--frames', str(FRAMES)] + a.extra.split(),
                  os.path.join(pasta, f'mov_{d}.png'))
        erro = float((np.abs(X - R)[valido] / (R[valido] + 0.01)).mean())
        vaga = float(100.0 * (X[valido] > 2.0 * R[valido] + 0.02).mean())

        sub = os.path.join(pasta, f'parado_{d}')
        os.makedirs(sub, exist_ok=True)
        for f in glob.glob(os.path.join(sub, '*.png')):
            os.remove(f)
        rodar(a.bin, ['--denoiser', d, '--frames', '40', '--save-every', '1'] + a.extra.split(), os.path.join(sub, 'f.png'))
        os.replace(os.path.join(sub, 'f.png'), os.path.join(sub, 'f_0040.png'))
        arquivos = sorted(glob.glob(os.path.join(sub, 'f_*.png')))
        X0 = lum(arquivos[-1])
        erro0 = float((np.abs(X0 - R0)[valido0] / (R0[valido0] + 0.01)).mean())
        L = np.stack([lum(f) for f in arquivos[-16:]])
        cint = float((L.std(0) / (L.mean(0) + 0.01)).mean())
        brilho = float(100 * (L > 2.0 * np.median(L, 0) + 0.02).any(0).mean())
        print(f'{d:8s} andando: erro {erro:.4f}, brilhinhos {vaga:.2f}%  |  parado: erro {erro0:.4f}, '
              f'cintilação {cint:.4f}, brilhinhos {brilho:.2f}%', flush=True)


if __name__ == '__main__':
    main()
