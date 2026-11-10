#!/usr/bin/env python3
"""Testes das linhas do tempo em JSON ("linhas_do_tempo" na cena, no estilo do Sequencer). Uso (de engine/):

    python3 tools/linha.py [--bin build/legacy] [--so objetos,...]

Cena: uma porta (caixa) e um elevador. A linha "porta" (tocada por um script com tocar_linha em t = 1 s) gira a
porta de 0 a 90 graus em 2 s e escreve eventos; a linha "elevador" toca ao iniciar e sobe o elevador 3 m em 3 s
passando por uma chave do meio.
  - objetos:  a porta está a 0° antes de tocar, ~45° no meio (curva suave) e exatamente 90° no fim; o elevador passa
              pela chave do meio (y = 1 em 1,5 s) e para em y = 3.
  - eventos:  os eventos da linha (log e lua) rodam na hora certa a partir do início dela e "fim" quando acaba.
  - camera:   durante a linha com câmera a imagem é a da chave (vermelho no centro: olha a porta vermelha de perto);
              depois volta a câmera do jogo.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

FPS = 30

LUA = '''
function atualizar(e, dt)
  local t = tempo()
  if t >= 1.0 and not pedido then pedido = true; tocar_linha("porta") end
  local _, ry = achar("porta"):rotacao()
  local _, ey = achar("elevador"):posicao()
  log("L", string.format("%.3f %.3f %.3f -", t, ry, ey))
end
'''


def cena(pasta, nome='linha'):
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write(LUA)
    c = {'menu': False, 'camera': {'posicao': [0, 2, 12], 'alvo': [0, 1, 0]},
         'materiais': {'verm': {'cor': [0.9, 0.05, 0.05]}},
         'entidades': [
             {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [20, 0.5, 20], 'posicao': [0, -0.5, 0]},
             {'nome': 'porta', 'forma': 'caixa', 'meia_extensao': [0.5, 1, 0.1], 'posicao': [0, 1, 0], 'material': 'verm'},
             {'nome': 'elevador', 'forma': 'caixa', 'meia_extensao': [1, 0.1, 1], 'posicao': [5, 0, 0]},
             {'nome': 'diretor', 'script': lua}],
         'linhas_do_tempo': {
             'porta': {'barras': False, 'pular': False, 'travar': False,
                       'objetos': [{'nome': 'porta', 'chaves': [{'t': 0, 'rot': [0, 0, 0]}, {'t': 2, 'rot': [0, 90, 0]}]}],
                       'camera': [{'t': 0, 'pos': [0, 1, 1.5], 'alvo': [0, 1, 0]}, {'t': 2, 'pos': [0, 1, 1.5], 'alvo': [0, 1, 0]}],
                       'eventos': [{'t': 0.5, 'log': 'meio segundo'}, {'t': 1.5, 'lua': 'log("LINHA", "lua rodou")'}]},
             'elevador': {'tocar_ao_iniciar': True, 'barras': False, 'pular': False, 'travar': False,
                          'objetos': [{'nome': 'elevador', 'chaves': [{'t': 0, 'pos': [5, 0, 0]}, {'t': 1.5, 'pos': [5, 1, 0]},
                                                                       {'t': 3, 'pos': [5, 3, 0]}]}]}}}
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(c, f)
    return arq


_cache = {}


def sessao(b, pasta):
    if 'log' not in _cache:
        arq = cena(pasta)
        out = {}
        for nome, quadros in (('meio', int(2.0 * FPS)), ('fim', int(4.0 * FPS))):
            png = os.path.join(pasta, f'{nome}.png')
            p = subprocess.run([b, '--headless', '--scene', arq, '--width', '64', '--height', '36', '--frames', str(quadros),
                                '--fps', str(FPS), '--no-motion-blur', '--out', png], capture_output=True, text=True,
                               encoding='utf-8', errors='replace')
            if p.returncode:
                raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
            out[nome] = (p.stdout + p.stderr, png)
        _cache['log'] = out
    return _cache['log']


def amostras(log):
    r = []
    for l in log.splitlines():
        if '[script] L ' in l:
            v = l.split('[script] L ', 1)[1].split()
            r.append((float(v[0]), float(v[1]), float(v[2]), v[3]))
    return r


def em(a, t):
    return min(a, key=lambda x: abs(x[0] - t))


def teste_objetos(b, pasta):
    a = amostras(sessao(b, pasta)['fim'][0])
    antes, meio, fim = em(a, 0.9)[1], em(a, 2.0)[1], em(a, 3.9)[1]
    e_meio, e_fim = em(a, 1.5)[2], em(a, 3.9)[2]
    ok = abs(antes) < 0.1 and 35 < meio < 55 and abs(fim - 90) < 0.1 and abs(e_meio - 1) < 0.05 and abs(e_fim - 3) < 1e-3
    return ok, (f'porta: {antes:.1f}° antes, {meio:.1f}° no meio, {fim:.2f}° no fim (90); elevador: y = {e_meio:.3f} em '
                f'1,5 s (1) e {e_fim:.3f} no fim (3)')


def teste_eventos(b, pasta):
    log = sessao(b, pasta)['fim'][0]
    tempos = {}
    for l in log.splitlines():
        if '[script] LINHA ' in l:
            tempos.setdefault(l.split('[script] LINHA ', 1)[1].strip(), len([x for x in log[:log.find(l)].splitlines()
                                                                               if '[script] L ' in x]))
    a = amostras(log)
    quando = lambda k: a[tempos[k]][0] if k in tempos and tempos[k] < len(a) else None
    t_meio, marcado = quando('meio segundo'), quando('lua rodou')
    fim_porta = 'fim porta' in tempos and 'fim elevador' in tempos
    ok = t_meio is not None and abs(t_meio - 1.5) < 0.1 and marcado is not None and abs(marcado - 2.5) < 0.1 and fim_porta
    return ok, (f'"meio segundo" em t = {t_meio} (1,5: 1 s + 0,5 s); evento lua rodou em t = {marcado} (2,5); '
                f'"fim" das duas linhas: {fim_porta}')


def teste_camera(b, pasta):
    import numpy as np
    from PIL import Image
    s = sessao(b, pasta)
    centro = []
    for nome in ('meio', 'fim'):
        img = np.asarray(Image.open(s[nome][1]).convert('RGB')).astype(float)
        h, w, _ = img.shape
        centro.append(img[h // 2 - 2:h // 2 + 3, w // 2 - 2:w // 2 + 3].reshape(-1, 3).mean(0))
    verm = lambda c: c[0] > 1.8 * c[1] and c[0] > 1.8 * c[2]
    ok = verm(centro[0]) and not verm(centro[1])
    return ok, (f'centro durante a linha {tuple(int(v) for v in centro[0])} (perto da porta vermelha), depois '
                f'{tuple(int(v) for v in centro[1])} (câmera do jogo, longe)')


TESTES = {'objetos': teste_objetos, 'eventos': teste_eventos, 'camera': teste_camera}


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
            print(f'{n:<8} {"PASSOU" if ok else "FALHOU"}: {msg}', flush=True)
    print(f'PASSOU {len(nomes) - falhas} FALHOU {falhas}')
    sys.exit(1 if falhas else 0)


if __name__ == '__main__':
    main()
