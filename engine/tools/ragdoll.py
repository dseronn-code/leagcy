#!/usr/bin/env python3
"""Testes do ragdoll (e:ragdoll / e:levantar). Uso (de engine/):

    python3 tools/ragdoll.py [--bin build/legacy] [--so cai,juntas,...]

A raposa (assets/Fox) parada no chão vira ragdoll em t = 1 s; um script registra a posição de todos os ossos a cada
quadro e o teste confere:
  - cai:       a bacia desce para perto do chão e nenhum osso atravessa o piso.
  - juntas:    a distância entre ossos ligados (pai e filho com corpo) não muda mais que 5%: as juntas seguram.
  - empurrao:  com um impulso para +X (3 m/s), o corpo vai para +X e cai para aquele lado.
  - repouso:   no fim, deitada, nada se mexe mais de 1 cm em meio segundo (não fica tremendo).
  - levantar:  e:levantar() em t = 5 s e o roteiro manda andar: a raposa volta a andar de pé, na altura normal.
  - degrau:    cai sobre a quina de um degrau de 30 cm: nenhum osso dentro do degrau nem do chão.
  - transicao: ao levantar, a pose caída se mistura com a animação em 0,5 s: nenhum osso salta mais de 5 cm de um
               quadro para o outro, e a raposa fica de pé virada para onde o tronco estava deitado.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

FPS = 60
OSSOS = ['b_Hip_01', 'b_Spine01_02', 'b_Spine02_03', 'b_Neck_04', 'b_Head_05', 'b_RightUpperArm_06',
         'b_RightForeArm_07', 'b_RightHand_08', 'b_LeftUpperArm_09', 'b_LeftForeArm_010', 'b_LeftHand_011',
         'b_Tail01_012', 'b_Tail02_013', 'b_Tail03_014', 'b_LeftLeg01_015', 'b_LeftLeg02_016', 'b_LeftFoot01_017',
         'b_LeftFoot02_018', 'b_RightLeg01_019', 'b_RightLeg02_020', 'b_RightFoot01_021', 'b_RightFoot02_022']
# Pares ligados por junta (os dois ossos com corpo próprio).
LIGADOS = [('b_Spine01_02', 'b_Spine02_03'), ('b_Spine02_03', 'b_Neck_04'), ('b_Neck_04', 'b_Head_05'),
           ('b_RightUpperArm_06', 'b_RightForeArm_07'), ('b_RightForeArm_07', 'b_RightHand_08'),
           ('b_LeftUpperArm_09', 'b_LeftForeArm_010'), ('b_LeftForeArm_010', 'b_LeftHand_011'),
           ('b_Tail01_012', 'b_Tail02_013'), ('b_Tail02_013', 'b_Tail03_014'),
           ('b_LeftLeg01_015', 'b_LeftLeg02_016'), ('b_LeftLeg02_016', 'b_LeftFoot01_017'),
           ('b_RightLeg01_019', 'b_RightLeg02_020'), ('b_RightLeg02_020', 'b_RightFoot01_021'),
           ('b_Hip_01', 'b_Spine01_02'), ('b_Hip_01', 'b_LeftLeg01_015'), ('b_Hip_01', 'b_Tail01_012')]
MASSA = 20.0  # kg


def raposa(pos, roteiro):
    return {'nome': 'raposa', 'posicao': pos,
            'personagem': {'altura': 0.8, 'raio': 0.15, 'massa': MASSA, 'velocidade': 1.5,
                           'velocidade_correndo': 2.7, 'controle': 'roteiro', 'roteiro': roteiro,
                           'visual': {'modelo': 'assets/Fox/Fox.gltf', 'escala': 0.01, 'parado': 'Survey',
                                      'andar': 'Walk', 'correr': 'Run', 'velocidade_andar': 1.5,
                                      'velocidade_correr': 2.7}}}


def chao():
    return {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [30, 0.5, 30], 'posicao': [0, -0.5, 0],
            'fisica': {'tipo': 'estatico'}}


def rodar(binario, pasta, nome, entidades, segundos, acao):
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write('local ossos = {' + ', '.join(f'"{o}"' for o in OSSOS) + '}\n')
        f.write('function atualizar(e, dt)\n' + acao + '\n  local partes = {"OSSO", tempo(), e:caido() and 1 or 0}\n'
                '  for _, o in ipairs(ossos) do\n    local x, y, z = e:osso(o)\n'
                '    if x then table.insert(partes, o); table.insert(partes, x); table.insert(partes, y);'
                ' table.insert(partes, z) end\n  end\n  log(table.unpack(partes))\nend\n')
    for e in entidades:
        if e.get('nome') == 'raposa':
            e['script'] = lua
    cena = os.path.join(pasta, f'{nome}.json')
    with open(cena, 'w', encoding='utf-8') as f:
        json.dump({'entidades': entidades}, f)
    p = subprocess.run([binario, '--headless', '--scene', cena, '--width', '32', '--height', '18', '--fps', str(FPS),
                        '--frames', str(int(segundos * FPS)), '--out', os.path.join(pasta, f'{nome}.png')],
                       capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    quadros = []
    for linha in p.stdout.splitlines() + p.stderr.splitlines():
        if 'OSSO' not in linha:
            continue
        partes = linha.split('OSSO', 1)[1].split()
        t, caido = float(partes[0]), partes[1] == '1'
        ossos = {}
        for i in range(2, len(partes), 4):
            ossos[partes[i]] = tuple(float(v) for v in partes[i + 1:i + 4])
        quadros.append((t, caido, ossos))
    if not quadros:
        raise RuntimeError('o script não registrou nenhum osso')
    return quadros


CAIR = '  if tempo() >= 1.0 and not cair_feito then cair_feito = true; e:ragdoll(%s) end'


def cena_queda(b, pasta, nome='queda', impulso='0, 0, 0', segundos=5.0):
    return rodar(b, pasta, nome, [chao(), raposa([0, 0.4, 0], [])], segundos, CAIR % impulso)


def antes_e_fim(q):
    antes = [o for t, c, o in q if not c]
    deitado = [o for t, c, o in q if c]
    return antes[-1], deitado


def teste_cai(b, pasta):
    q = cena_queda(b, pasta)
    em_pe, deitado = antes_e_fim(q)
    if not deitado:
        return False, 'e:ragdoll não ligou'
    y0 = em_pe['b_Hip_01'][1]
    y1 = deitado[-1]['b_Hip_01'][1]
    menor = min(o[n][1] for o in deitado for n in o)
    ok = y1 < 0.6 * y0 and menor > -0.02
    return ok, f'bacia {y0:.2f} m de pé -> {y1:.2f} m caída; osso mais baixo em toda a queda: {menor * 100:.1f} cm'


def teste_juntas(b, pasta):
    q = cena_queda(b, pasta, 'juntas', '0, 0, 0')
    em_pe, deitado = antes_e_fim(q)
    pior, qual = 0.0, ''
    for a, c in LIGADOS:
        d0 = math.dist(em_pe[a], em_pe[c])
        for o in deitado:
            r = abs(math.dist(o[a], o[c]) - d0) / d0
            if r > pior:
                pior, qual = r, f'{a}-{c}'
    return pior < 0.05, f'maior mudança de distância entre ossos ligados: {pior * 100:.2f}% ({qual})'


def teste_empurrao(b, pasta):
    q = cena_queda(b, pasta, 'empurrao', f'{3 * MASSA}, 0, 0')
    em_pe, deitado = antes_e_fim(q)
    dx = deitado[-1]['b_Hip_01'][0] - em_pe['b_Hip_01'][0]
    dz = deitado[-1]['b_Hip_01'][2] - em_pe['b_Hip_01'][2]
    ok = dx > 0.3 and abs(dz) < dx
    return ok, f'bacia andou {dx:.2f} m em X e {dz:.2f} m em Z com 3 m/s para +X'


def teste_repouso(b, pasta):
    q = cena_queda(b, pasta, 'repouso', segundos=6.0)
    fim = [o for t, c, o in q if c and t >= q[-1][0] - 0.5]
    pior = max(math.dist(fim[0][n], o[n]) for o in fim for n in fim[0])
    return pior < 0.01, f'maior movimento de um osso no último meio segundo: {pior * 100:.2f} cm'


def teste_levantar(b, pasta):
    acao = (CAIR % '0, 0, 0') + '\n  if tempo() >= 4.0 and not levantou then levantou = true; e:levantar() end'
    q = rodar(b, pasta, 'levantar', [chao(), raposa([0, 0.4, 0], [{'t': 4.5, 'andar': [1, 0, 0]}])], 7.0, acao)
    em_pe = [o for t, c, o in q if not c and t < 1.0][-1]
    deitado = [o for t, c, o in q if c]
    depois = [(t, o) for t, c, o in q if not c and t > 4.0]
    if not deitado or not depois:
        return False, 'não caiu ou não levantou'
    y0 = em_pe['b_Hip_01'][1]
    hips = [o['b_Hip_01'][1] for t, o in depois if t > 6.0]
    media = sum(hips) / len(hips)
    x4 = depois[0][1]['b_Hip_01'][0]
    x7 = depois[-1][1]['b_Hip_01'][0]
    ok = abs(media - y0) < 0.05 and x7 - x4 > 1.0
    return ok, f'bacia andando {media:.2f} m (de pé antes de cair: {y0:.2f}); andou {x7 - x4:.2f} m depois de levantar'


def teste_degrau(b, pasta):
    degrau = {'nome': 'degrau', 'forma': 'caixa', 'meia_extensao': [1, 0.15, 1], 'posicao': [1.15, 0.15, 0],
              'fisica': {'tipo': 'estatico'}}
    q = rodar(b, pasta, 'degrau', [chao(), degrau, raposa([0, 0.4, 0], [])], 5.0, CAIR % f'{2 * MASSA}, 0, 0')
    _, deitado = antes_e_fim(q)
    pior = 0.0
    for o in deitado:
        for n, (x, y, z) in o.items():
            pior = max(pior, -y)
            if 0.15 < x < 2.15 and abs(z) < 1.0:
                pior = max(pior, 0.3 - y)
    return pior < 0.02, f'maior entrada de um osso no degrau ou no chão: {pior * 100:.1f} cm'


def teste_transicao(b, pasta):
    acao = (CAIR % f'{2 * MASSA}, 0, {1 * MASSA}') + '\n  if tempo() >= 4.0 and not levantou then levantou = true; e:levantar() end'
    q = rodar(b, pasta, 'transicao', [chao(), raposa([0, 0.4, 0], [])], 5.5, acao)
    deitado = [o for t, c, o in q if c][-1]
    depois = [o for t, c, o in q if not c and t > 3.9]
    pior, qual = 0.0, ''
    seq = [deitado] + depois
    for i in range(1, len(seq)):
        for n in seq[i]:
            d = math.dist(seq[i][n], seq[i - 1][n])
            if d > pior:
                pior, qual = d, n
    def rumo(o):
        return math.degrees(math.atan2(o['b_Neck_04'][0] - o['b_Hip_01'][0], o['b_Neck_04'][2] - o['b_Hip_01'][2]))
    giro = abs((rumo(depois[-1]) - rumo(deitado) + 180) % 360 - 180)
    ok = pior < 0.05 and giro < 30
    return ok, f'maior salto de um osso por quadro ao levantar: {pior * 100:.1f} cm ({qual}); de pé virada a {giro:.0f}° de como estava deitada'


TESTES = {'cai': teste_cai, 'juntas': teste_juntas, 'empurrao': teste_empurrao, 'repouso': teste_repouso,
          'levantar': teste_levantar, 'degrau': teste_degrau, 'transicao': teste_transicao}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--so', default='')
    a = ap.parse_args()
    nomes = [n for n in a.so.split(',') if n] or list(TESTES)
    falhas = 0
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as pasta:
        for n in nomes:
            try:
                ok, msg = TESTES[n](a.bin, pasta)
            except Exception as ex:  # noqa: BLE001
                ok, msg = False, f'erro: {ex}'
            falhas += not ok
            print(f'{n:<10} {"PASSOU" if ok else "FALHOU"}: {msg}', flush=True)
    print(f'PASSOU {len(nomes) - falhas} FALHOU {falhas}')
    sys.exit(1 if falhas else 0)


if __name__ == '__main__':
    main()
