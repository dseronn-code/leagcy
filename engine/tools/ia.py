#!/usr/bin/env python3
"""Testes dos comportamentos de NPC (navegação + visão). Uso (de engine/):

    python3 tools/ia.py [--bin build/legacy] [--so patrulha,...]

Cada cena tem um chão, o NPC ("guarda", olhando para +Z no começo) e às vezes um "heroi" (personagem com roteiro) e
paredes. Um script registra, a cada quadro, a posição dos dois e o estado do NPC (e:estado_ia()).
  - patrulha:  passa pelos 3 pontos da patrulha, em ordem, e recomeça.
  - persegue:  o herói parado 8 m à frente: o NPC vê, corre até ele e para a ~1,5 m.
  - parede:    o mesmo, com uma parede no meio: não vê, não sai do lugar.
  - costas:    o herói 6 m atrás do NPC (fora do cone de visão de 140°): não vê.
  - procura:   o herói é visto e foge mais rápido que o NPC, até sair do alcance da visão: o NPC vai até onde o
               viu por último ("procurando") e depois volta à patrulha.
  - foge:      o herói anda na direção do NPC: ele foge (e continua fugindo de costas, pela memória de onde viu a
               ameaça): a distância entre os dois cresce.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

FPS = 30
LUA = '''
function atualizar(e, dt)
  local g = achar("guarda")
  local h = achar("heroi")
  local gx, gy, gz = g:posicao()
  local hx, hy, hz = 0, 0, 0
  if h then hx, hy, hz = h:posicao() end
  log("IA", string.format("%.3f %.3f %.3f %.3f %.3f", tempo(), gx, gz, hx, hz), g:estado_ia())
end
'''


def chao():
    return {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [40, 0.5, 40], 'posicao': [0, -0.5, 0],
            'fisica': {'tipo': 'estatico'}}


def guarda(ia, pos=(0, 0, 0)):
    return {'nome': 'guarda', 'posicao': list(pos),
            'personagem': {'controle': 'ia', 'velocidade': 2.0, 'velocidade_correndo': 4.5, 'ia': ia}}


def heroi(pos, roteiro=None):
    return {'nome': 'heroi', 'posicao': list(pos),
            'personagem': {'controle': 'roteiro', 'velocidade': 2.0, 'roteiro': roteiro or [{'t': 0, 'andar': [0, 0, 0]}]}}


def parede(pos, meia):
    return {'nome': f'parede_{pos[0]}_{pos[2]}', 'forma': 'caixa', 'meia_extensao': list(meia), 'posicao': list(pos),
            'fisica': {'tipo': 'estatico'}}


def rodar(b, pasta, nome, entidades, segundos):
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write(LUA)
    entidades = entidades + [{'nome': 'log', 'forma': 'esfera', 'raio': 0.01, 'posicao': [0, -30, 0], 'script': lua}]
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump({'menu': False, 'entidades': entidades}, f)
    p = subprocess.run([b, '--headless', '--scene', arq, '--width', '32', '--height', '18', '--frames',
                        str(int(segundos * FPS)), '--out', os.path.join(pasta, f'{nome}.png')],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-300:]}')
    out = []
    for l in (p.stdout + p.stderr).splitlines():
        if '[script] IA ' in l:
            parts = l.split('[script] IA ', 1)[1].split()
            out.append(tuple(map(float, parts[:5])) + (parts[5],))
    return out


def teste_patrulha(b, pasta):
    pts = [[4, 0, 0], [4, 0, 6], [-3, 0, 6]]
    q = rodar(b, pasta, 'patrulha', [chao(), guarda({'patrulha': pts})], 20)
    ordem = []
    for t, gx, gz, *_ in q:
        for i, p in enumerate(pts):
            if math.hypot(gx - p[0], gz - p[2]) < 0.8 and (not ordem or ordem[-1] != i):
                ordem.append(i)
    esperado = [0, 1, 2, 0]
    ok = ordem[:4] == esperado and all(x[5] == 'patrulhando' for x in q[5:])
    return ok, f'pontos visitados em ordem: {ordem[:6]} (esperado começar por {esperado}); estado "patrulhando"'


def teste_persegue(b, pasta):
    q = rodar(b, pasta, 'persegue', [chao(), guarda({'perseguir': 'heroi', 'parar_a': 1.5}), heroi([0, 0, 8])], 5)
    t, gx, gz, hx, hz, est = q[-1]
    d = math.hypot(gx - hx, gz - hz)
    viu = any(x[5] == 'perseguindo' for x in q)
    return viu and d < 2.0, f'estado "perseguindo": {viu}; distância final {d:.2f} m (para a 1,5)'


def teste_parede(b, pasta):
    q = rodar(b, pasta, 'parede', [chao(), guarda({'perseguir': 'heroi'}), heroi([0, 0, 8]),
                                    parede([0, 1.5, 4], [3, 1.5, 0.2])], 4)
    andou = math.hypot(q[-1][1] - q[0][1], q[-1][2] - q[0][2])
    viu = any(x[5] == 'perseguindo' for x in q)
    return not viu and andou < 0.1, f'com a parede no meio: viu = {viu}, andou {andou:.2f} m'


def teste_costas(b, pasta):
    q = rodar(b, pasta, 'costas', [chao(), guarda({'perseguir': 'heroi'}), heroi([0, 0, -6])], 3)
    viu = any(x[5] == 'perseguindo' for x in q)
    andou = math.hypot(q[-1][1] - q[0][1], q[-1][2] - q[0][2])
    return not viu and andou < 0.1, f'herói nas costas (fora do cone): viu = {viu}, andou {andou:.2f} m'


def teste_procura(b, pasta):
    # O herói começa visível 7 m à frente e corre para longe (8 m/s, mais rápido que o guarda): sai do alcance da
    # visão; o guarda vai até onde o viu por último ("procurando") e depois volta à patrulha.
    rot = [{'t': 0, 'andar': [0, 0, 0]}, {'t': 1.0, 'andar': [0, 0, 1], 'correr': True}, {'t': 4.0, 'andar': [0, 0, 0]}]
    ent = [chao(), guarda({'perseguir': 'heroi', 'patrulha': [[0, 0, 0], [0, 0, -1.5]], 'visao': 8}),
           heroi([0, 0, 7], rot)]
    q = rodar(b, pasta, 'procura', ent, 16)
    estados = []
    for x in q:
        if not estados or estados[-1] != x[5]:
            estados.append(x[5])
    ok = 'perseguindo' in estados and 'procurando' in estados and estados[-1] == 'patrulhando' and \
        estados.index('procurando') > estados.index('perseguindo')
    return ok, f'sequência de estados: {" → ".join(estados)}'


def teste_foge(b, pasta):
    ent = [chao(), guarda({'fugir_de': 'heroi', 'visao': 10}), heroi([0, 0, 6], [{'t': 0, 'andar': [0, 0, -1]}])]
    q = rodar(b, pasta, 'foge', ent, 5)
    d0 = math.hypot(q[3][1] - q[3][3], q[3][2] - q[3][4])
    d1 = math.hypot(q[-1][1] - q[-1][3], q[-1][2] - q[-1][4])
    fugiu = any(x[5] == 'fugindo' for x in q)
    return fugiu and d1 > d0, f'estado "fugindo": {fugiu}; distância {d0:.1f} m → {d1:.1f} m (o herói anda até ele)'


TESTES = {'patrulha': teste_patrulha, 'persegue': teste_persegue, 'parede': teste_parede, 'costas': teste_costas,
          'procura': teste_procura, 'foge': teste_foge}


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--so')
    a = ap.parse_args()
    b = os.path.abspath(a.bin)
    oks = []
    with tempfile.TemporaryDirectory() as pasta:
        for n in (a.so.split(',') if a.so else TESTES):
            try:
                ok, info = TESTES[n](b, pasta)
            except Exception as e:  # noqa: BLE001
                ok, info = False, f'erro: {e}'
            print(f'{n:9s} {"PASSOU" if ok else "FALHOU"}: {info}', flush=True)
            oks.append(ok)
    sys.exit(0 if all(oks) else 1)


if __name__ == '__main__':
    main()
