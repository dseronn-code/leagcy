#!/usr/bin/env python3
"""Testes do retargeting (animação de um esqueleto tocada em outro). Uso (de engine/):

    python3 tools/retarget.py [--bin build/legacy] [--so direcao,...]

O teste gera uma variante do CesiumMan com outro esqueleto: ossos com outros nomes (prefixo "Var_"), pernas 30% mais
longas, braços 20% mais curtos, o tronco com outra rotação de repouso e sem nenhuma animação. O retargeting copia a
animação do CesiumMan original para ela; os dois andam lado a lado e um script registra a posição dos ossos.
  - direcao:  cada segmento de osso (coxa, canela, braço, antebraço) aponta para a mesma direção nos dois a cada
              quadro (a variante faz o mesmo movimento com proporções diferentes).
  - bacia:    o sobe-e-desce da bacia na variante é o da original vezes a razão das alturas (pernas mais longas).
  - mapa:     com um mapa explícito de só uma perna, a outra perna fica parada na pose de repouso.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

FPS = 30
PERNAS = ['leg_joint_L_2', 'leg_joint_L_3', 'leg_joint_R_2', 'leg_joint_R_3', 'leg_joint_L_5', 'leg_joint_R_5']
BRACOS = ['Skeleton_arm_joint_L__3_', 'Skeleton_arm_joint_L__2_', 'Skeleton_arm_joint_R__2_', 'Skeleton_arm_joint_R__3_']
SEGMENTOS = [('leg_joint_L_1', 'leg_joint_L_2'), ('leg_joint_L_2', 'leg_joint_L_3'),
             ('leg_joint_R_1', 'leg_joint_R_2'), ('leg_joint_R_2', 'leg_joint_R_3'),
             ('Skeleton_arm_joint_L__4_', 'Skeleton_arm_joint_L__3_'), ('Skeleton_arm_joint_L__3_', 'Skeleton_arm_joint_L__2_'),
             ('Skeleton_arm_joint_R', 'Skeleton_arm_joint_R__2_'), ('Skeleton_arm_joint_R__2_', 'Skeleton_arm_joint_R__3_')]
BACIA = 'Skeleton_torso_joint_1'


def variante(pasta):
    """CesiumMan com outro esqueleto (nomes, proporções, repouso do tronco) e sem animação."""
    src = os.path.abspath(os.path.join('assets', 'CesiumMan'))
    dst = os.path.join(pasta, 'variante')
    if os.path.isdir(dst):
        return os.path.join(dst, 'Variante.gltf')
    shutil.copytree(src, dst)
    with open(os.path.join(dst, 'CesiumMan.gltf'), encoding='utf-8') as f:
        g = json.load(f)
    juntas = set(g['skins'][0]['joints'])
    for i, n in enumerate(g['nodes']):
        nome = n.get('name', '')
        if nome in PERNAS:
            n['translation'] = [v * 1.3 for v in n.get('translation', [0, 0, 0])]
        if nome in BRACOS:
            n['translation'] = [v * 0.8 for v in n.get('translation', [0, 0, 0])]
        if nome == 'torso_joint_3':  # outra rotação de repouso (20° no eixo z local)
            q = n.get('rotation', [0, 0, 0, 1])
            s, c = math.sin(math.radians(10)), math.cos(math.radians(10))
            x, y, z, w = q
            n['rotation'] = [w * 0 + x * c + y * s - z * 0, y * c - x * s, z * c + w * s, w * c - z * s]
        if i in juntas:
            n['name'] = 'Var_' + nome
    g.pop('animations', None)
    caminho = os.path.join(dst, 'Variante.gltf')
    with open(caminho, 'w', encoding='utf-8') as f:
        json.dump(g, f)
    return caminho


def personagem(nome, modelo, x, retarget=None):
    v = {'modelo': modelo, 'parado': 'anim0', 'ik_pes': False}
    if retarget:
        v['retarget'] = retarget
    return {'nome': nome, 'posicao': [x, 0, 0],
            'personagem': {'altura': 1.8, 'raio': 0.3, 'controle': 'roteiro', 'roteiro': [], 'visual': v}}


def rodar(b, pasta, nome, retarget, segundos=2.0):
    var = variante(pasta)
    ossos = [a for s in SEGMENTOS for a in s] + [BACIA]
    ossos = list(dict.fromkeys(ossos))
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write('local ossos = {' + ', '.join(f'"{o}"' for o in ossos) + '}\n')
        f.write('function atualizar(e, dt)\n  local partes = {"OSSOS", tempo()}\n'
                '  for _, quem in ipairs({{"orig", ""}, {"var", "Var_"}}) do\n'
                '    local ent = achar(quem[1])\n'
                '    for _, o in ipairs(ossos) do local x, y, z = ent:osso(quem[2] .. o)\n'
                '      table.insert(partes, string.format("%s:%s:%.5f:%.5f:%.5f", quem[1], o, x or 0, y or 0, z or 0)) end\n'
                '  end\n  log(table.unpack(partes))\nend\n')
    c = {'menu': False, 'entidades': [
        {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [10, 0.5, 10], 'posicao': [0, -0.5, 0], 'fisica': {'tipo': 'estatico'}},
        personagem('orig', os.path.abspath(os.path.join('assets', 'CesiumMan', 'CesiumMan.gltf')), -1.0),
        personagem('var', var, 1.0, retarget),
        {'nome': 'diretor', 'script': lua}]}
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(c, f)
    p = subprocess.run([b, '--headless', '--scene', arq, '--width', '32', '--height', '18', '--fps', str(FPS),
                        '--frames', str(int(segundos * FPS)), '--out', os.path.join(pasta, f'{nome}.png')],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    quadros = []
    for l in (p.stdout + p.stderr).splitlines():
        if '[script] OSSOS ' not in l:
            continue
        partes = l.split('[script] OSSOS ', 1)[1].split()
        q = {'t': float(partes[0])}
        for item in partes[1:]:
            quem, osso, x, y, z = item.split(':')
            q[(quem, osso)] = (float(x), float(y), float(z))
        quadros.append(q)
    log = p.stdout + p.stderr
    if not quadros:
        raise RuntimeError('o script não registrou nada: ' + log[-400:])
    return quadros, log


def angulo(u, v):
    nu, nv = math.sqrt(sum(a * a for a in u)), math.sqrt(sum(a * a for a in v))
    if nu < 1e-9 or nv < 1e-9:
        return 0.0
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(a * b for a, b in zip(u, v)) / (nu * nv)))))


def seg(q, quem, a, b):
    pa, pb = q[(quem, a)], q[(quem, b)]
    return tuple(y - x for x, y in zip(pa, pb))


_cache = {}


def sessao(b, pasta):
    if 'auto' not in _cache:
        _cache['auto'] = rodar(b, pasta, 'auto', {'de': os.path.abspath(os.path.join('assets', 'CesiumMan', 'CesiumMan.gltf')),
                                                   'prefixo_destino': 'Var_'})
    return _cache['auto']


def teste_direcao(b, pasta):
    quadros, log = sessao(b, pasta)
    pior, qual = 0.0, ''
    for q in quadros[5:]:
        for a, c in SEGMENTOS:
            ang = angulo(seg(q, 'orig', a, c), seg(q, 'var', a, c))
            if ang > pior:
                pior, qual = ang, f'{a}->{c} em t = {q["t"]:.2f}'
    casados = [l.split('] ', 1)[-1] for l in log.splitlines() if 'Retargeting:' in l]
    ok = pior < 2.0 and bool(casados)
    return ok, f'{casados[0] if casados else "sem retargeting"}; maior diferença de direção de um osso: {pior:.2f}° ({qual})'


def teste_bacia(b, pasta):
    quadros, _ = sessao(b, pasta)
    yo = [q[('orig', BACIA)][1] for q in quadros[5:]]
    yv = [q[('var', BACIA)][1] for q in quadros[5:]]
    # Altura de repouso das pernas: do pé (tornozelo) à bacia no primeiro quadro, para a razão.
    ao, av = max(yo) - min(yo), max(yv) - min(yv)
    ho = sum(yo) / len(yo) - quadros[5][('orig', 'leg_joint_L_3')][1]
    hv = sum(yv) / len(yv) - quadros[5][('var', 'leg_joint_L_3')][1]
    razao = av / ao if ao > 1e-6 else 0.0
    esperado = hv / ho if ho > 1e-6 else 0.0
    ok = ao > 0.005 and abs(razao - esperado) < 0.12 * esperado
    return ok, (f'sobe-e-desce da bacia {100 * ao:.1f} cm na original e {100 * av:.1f} cm na variante: razão {razao:.2f} '
                f'(altura da bacia acima do tornozelo {esperado:.2f}x)')


def teste_mapa(b, pasta):
    mapa = {n: 'Var_' + n for n in ['Skeleton_torso_joint_1', 'leg_joint_L_1', 'leg_joint_L_2', 'leg_joint_L_3', 'leg_joint_L_5']}
    quadros, _ = rodar(b, pasta, 'mapa', {'de': os.path.abspath(os.path.join('assets', 'CesiumMan', 'CesiumMan.gltf')),
                                          'mapa': mapa})
    mexe_l = max(angulo(seg(quadros[5], 'var', 'leg_joint_L_2', 'leg_joint_L_3'), seg(q, 'var', 'leg_joint_L_2', 'leg_joint_L_3'))
                 for q in quadros[5:])
    mexe_r = max(angulo(seg(quadros[5], 'var', 'leg_joint_R_2', 'leg_joint_R_3'), seg(q, 'var', 'leg_joint_R_2', 'leg_joint_R_3'))
                 for q in quadros[5:])
    ok = mexe_l > 10 and mexe_r < mexe_l * 0.5
    return ok, f'a canela esquerda (no mapa) gira {mexe_l:.1f}° no passo; a direita (fora do mapa) {mexe_r:.1f}° (só segue a bacia)'


TESTES = {'direcao': teste_direcao, 'bacia': teste_bacia, 'mapa': teste_mapa}


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
