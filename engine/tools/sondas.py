#!/usr/bin/env python3
"""Testes das sondas de irradiância (caminho sem ray tracing). Uso (de engine/):

    python3 tools/sondas.py [--bin build/legacy] [--so sponza,parede,...]

As sondas são assadas com ray tracing (--assar-gi) e usadas com --no-rt no lugar do céu como luz ambiente.
  - sponza:  sem ray tracing, a imagem com as sondas fica mais perto da imagem com ray tracing do que sem elas (o
             erro médio cai para menos da metade).
  - parede:  sala fechada com o sol lá fora: por dentro, com as sondas, fica escuro como no ray tracing (a luz não
             vaza pelas paredes); só com o céu, ficaria clara.
  - reuso:   rodar de novo com o arquivo já assado lê as sondas sem assar outra vez.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

TAM = (240, 135)


def rodar(b, pasta, nome, extra, quadros=30, cena=None):
    out = os.path.join(pasta, f'{nome}.png')
    cmd = [b, '--headless', '--width', str(TAM[0]), '--height', str(TAM[1]), '--frames', str(quadros),
           '--no-motion-blur', '--out', out] + (['--scene', cena] if cena else []) + extra
    p = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    return out, p.stdout + p.stderr


def imagem(png):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(png).convert('RGB')).astype(float)


def teste_sponza(b, pasta):
    gi = os.path.join(pasta, 'sponza.gi')
    _, log = rodar(b, pasta, 'assar', ['--sondas-gi', gi, '--assar-gi'], quadros=5)
    rt, _ = rodar(b, pasta, 'rt', [])
    sem, _ = rodar(b, pasta, 'sem', ['--no-rt'])
    com, _ = rodar(b, pasta, 'com', ['--no-rt', '--sondas-gi', gi])
    ref = imagem(rt)
    e_sem, e_com = abs(imagem(sem) - ref).mean(), abs(imagem(com) - ref).mean()
    tempo = [l.split('assadas em ')[1].split(' s')[0] for l in log.splitlines() if 'assadas em' in l]
    return e_com < 0.5 * e_sem, (f'erro contra o ray tracing: {e_sem:.1f}/255 só com o céu, {e_com:.1f}/255 com as sondas '
                                 f'({e_sem / max(e_com, 1e-6):.1f}x menor); assadas em {tempo[0] if tempo else "?"} s')


def sala(pasta):
    # Sala de 6 x 3 x 6 m com paredes de 20 cm, fechada; sol forte lá fora; a câmera dentro olha para uma parede.
    m = 0.1
    ents = [{'nome': 'chao_fora', 'forma': 'caixa', 'meia_extensao': [30, 0.1, 30], 'posicao': [0, -0.1, 0],
             'material': 'claro', 'fisica': {'tipo': 'estatico'}}]
    for nome, he, pos in (('piso', [3, m, 3], [0, m, 0]), ('teto', [3, m, 3], [0, 3 - m, 0]),
                          ('px', [m, 1.5, 3], [3 - m, 1.5, 0]), ('nx', [m, 1.5, 3], [-3 + m, 1.5, 0]),
                          ('pz', [3, 1.5, m], [0, 1.5, 3 - m]), ('nz', [3, 1.5, m], [0, 1.5, -3 + m])):
        ents.append({'nome': nome, 'forma': 'caixa', 'meia_extensao': he, 'posicao': pos, 'material': 'claro',
                     'fisica': {'tipo': 'estatico'}})
    c = {'menu': False, 'camera': {'posicao': [0, 1.5, 2.0], 'alvo': [0, 1.2, -2.8]},
         'materiais': {'claro': {'cor': [0.8, 0.8, 0.8], 'rugosidade': 0.9}}, 'entidades': ents}
    arq = os.path.join(pasta, 'sala.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(c, f)
    return arq


def teste_parede(b, pasta):
    arq = sala(pasta)
    gi = os.path.join(pasta, 'sala.gi')
    rodar(b, pasta, 'sala_assar', ['--sondas-gi', gi, '--assar-gi', '--exposure', '12'], quadros=5, cena=arq)
    rt, _ = rodar(b, pasta, 'sala_rt', ['--exposure', '12'], cena=arq)
    sem, _ = rodar(b, pasta, 'sala_sem', ['--no-rt', '--exposure', '12'], cena=arq)
    com, _ = rodar(b, pasta, 'sala_com', ['--no-rt', '--sondas-gi', gi, '--exposure', '12'], cena=arq)
    l_rt, l_sem, l_com = imagem(rt).mean(), imagem(sem).mean(), imagem(com).mean()
    ok = l_com < 0.25 * l_sem and l_com < l_rt + 8
    return ok, (f'brilho médio dentro da sala fechada: {l_rt:.1f} com ray tracing, {l_sem:.1f} só com o céu, '
                f'{l_com:.1f} com as sondas (0 = preto)')


def teste_reuso(b, pasta):
    arq = sala(pasta)
    gi = os.path.join(pasta, 'reuso.gi')
    _, log1 = rodar(b, pasta, 'reuso1', ['--sondas-gi', gi], quadros=5, cena=arq)
    _, log2 = rodar(b, pasta, 'reuso2', ['--sondas-gi', gi], quadros=5, cena=arq)
    ok = 'assando' in log1 and 'assando' not in log2 and os.path.exists(gi)
    return ok, (f'1ª vez assou: {"assando" in log1}; 2ª vez assou: {"assando" in log2}; arquivo de '
                f'{os.path.getsize(gi) // 1024 if os.path.exists(gi) else 0} KB')


TESTES = {'sponza': teste_sponza, 'parede': teste_parede, 'reuso': teste_reuso}


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--so', default='')
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    nomes = [n for n in a.so.split(',') if n] or list(TESTES)
    falhas = 0
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as pasta:
        for n in nomes:
            try:
                ok, msg = TESTES[n](a.bin, pasta)
            except Exception as ex:  # noqa: BLE001
                ok, msg = False, f'erro: {ex}'
            falhas += not ok
            print(f'{n:<7} {"PASSOU" if ok else "FALHOU"}: {msg}', flush=True)
    print(f'PASSOU {len(nomes) - falhas} FALHOU {falhas}')
    sys.exit(1 if falhas else 0)


if __name__ == '__main__':
    main()
