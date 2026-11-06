#!/usr/bin/env python3
"""Testes da animação avançada (personagem com "visual": mistura por velocidade, ações e IK dos pés). Uso (de engine/):

    python3 tools/animacao.py [--bin build/legacy] [--so frente,patinar,...]

Personagem: a raposa (assets/Fox, CC-BY 4.0) com parado/andar/correr. Um script registra, a cada quadro, a posição dos
ossos (e:osso) e o teste confere:
  - frente:   andando para +X, a cabeça fica à frente da bacia na direção do movimento.
  - patinar:  o clipe toca acelerado pela velocidade do corpo: a pata de trás apoiada escorrega exatamente o que o
              próprio clipe já escorrega (previsto a partir do clipe tocado no lugar). Nesta raposa a pata traseira
              direita da corrida pisa a 1,9 m/s e a esquerda a 2,9: nenhuma velocidade deixa as duas paradas.
  - mistura:  passa de andar para correr: nenhum osso salta de um quadro para o outro mais do que na corrida normal.
  - acao:     e:animar("Survey") no meio da caminhada e e:parar_animacao(): a transição também não dá saltos.
  - degrau:   parada com as patas da frente sobre um degrau de 15 cm: com o IK, as quatro patas ficam no chão (mesma
              altura acima do piso de cada uma que no chão plano); sem o IK, as da frente entrariam no degrau.
  - rampa:    o mesmo numa rampa de 15°.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

FPS = 120  # quadros por segundo simulados: o apoio de uma pata na corrida dura poucos centésimos
PATAS = {'tras_esq': 'b_LeftFoot02_018', 'tras_dir': 'b_RightFoot02_022',
         'frente_esq': 'b_LeftHand_011', 'frente_dir': 'b_RightHand_08'}
OSSOS = ['b_Hip_01', 'b_Head_05'] + list(PATAS.values())
PERNAS = [['b_LeftLeg01_015', 'b_LeftLeg02_016', 'b_LeftFoot01_017'],
          ['b_RightLeg01_019', 'b_RightLeg02_020', 'b_RightFoot01_021'],
          ['b_LeftUpperArm_09', 'b_LeftForeArm_010', 'b_LeftHand_011'],
          ['b_RightUpperArm_06', 'b_RightForeArm_07', 'b_RightHand_08']]


def raposa(pos, roteiro, ik=True, script=None):
    e = {'nome': 'raposa', 'posicao': pos,
         'personagem': {'altura': 0.8, 'raio': 0.05, 'velocidade': 1.5, 'velocidade_correndo': 2.7,
                        'controle': 'roteiro', 'roteiro': roteiro,
                        'visual': {'modelo': 'assets/Fox/Fox.gltf', 'escala': 0.01, 'parado': 'Survey',
                                   'andar': 'Walk', 'correr': 'Run', 'velocidade_andar': 1.5,
                                   'velocidade_correr': 2.7, 'ik_pes': ik, 'pernas': PERNAS}}}
    if script:
        e['script'] = script
    return e


def chao():
    return {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [30, 0.5, 30], 'posicao': [0, -0.5, 0],
            'fisica': {'tipo': 'estatico'}}


def rodar(binario, pasta, nome, entidades, segundos, extra_lua=''):
    """Roda a cena e devolve [(t, {osso: (x, y, z)})] de cada quadro."""
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write('local ossos = {' + ', '.join(f'"{o}"' for o in OSSOS) + '}\n')
        f.write(extra_lua + '\n')
        f.write('function atualizar(e, dt)\n  if acao then acao(e) end\n  local partes = {"OSSO", tempo()}\n'
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
        t = float(partes[0])
        ossos = {}
        for i in range(1, len(partes), 4):
            ossos[partes[i]] = tuple(float(v) for v in partes[i + 1:i + 4])
        quadros.append((t, ossos))
    if not quadros:
        raise RuntimeError('o script não registrou nenhum osso')
    return quadros


def teste_frente(b, pasta):
    q = rodar(b, pasta, 'frente', [chao(), raposa([0, 0, 0], [{'t': 0, 'andar': [1, 0, 0]}])], 2.0)
    t, o = q[-1]
    hx, _, hz = o['b_Head_05']
    bx, _, bz = o['b_Hip_01']
    d = (hx - bx, hz - bz)
    c = d[0] / math.hypot(*d)
    return c > 0.95, f'direção bacia→cabeça · movimento = {c:.3f} (1 = olhando para onde anda)'


def apoios(q, desde):
    """Para cada pata de trás: velocidade horizontal (m/s) no instante em que ela está mais baixa em cada passada."""
    q = [x for x in q if x[0] >= desde]
    res = {}
    for nome in ('tras_esq', 'tras_dir'):
        osso = PATAS[nome]
        ys = [o[osso][1] for _, o in q]
        piso = min(ys)
        vs = []
        for i in range(12, len(q) - 12):
            if ys[i] > piso + 0.03 or ys[i] != min(ys[i - 12:i + 13]):
                continue
            (ta, a), (tb, bb) = q[i - 2], q[i + 2]  # 1/30 s em volta do ponto mais baixo
            vs.append(math.hypot(bb[osso][0] - a[osso][0], bb[osso][2] - a[osso][2]) / (tb - ta))
        res[nome] = sum(vs) / len(vs) if vs else float('nan')
    return res


def teste_patinar(b, pasta):
    """O clipe toca na velocidade do corpo: a pata apoiada escorrega só o que o próprio clipe já escorrega.

    Parado no lugar e tocando o clipe na velocidade original, a pata apoiada anda para trás a uma velocidade u (a do
    clipe: nesta raposa as duas patas de trás nem pisam igual). Andando a s m/s com o clipe acelerado por s/ref, ela
    deve escorregar |u·s/ref − s| no mundo. O teste confere a medida contra essa previsão na velocidade de referência
    (clipe puro; entre dois pontos da mistura os clipes se somam e a previsão simples não vale)."""
    textos, ok = [], True
    for clipe, ref, correr in (('Walk', 1.5, False), ('Run', 2.7, True)):
        lua = f'function acao(e)\n  if not p then e:animar("{clipe}", 0, true); p = true end\nend'
        parado = apoios(rodar(b, pasta, f'lugar_{clipe}', [chao(), raposa([0, 0, 0], [{'t': 0, 'andar': [0, 0, 0]}],
                                                                           ik=False)], 3.0, lua), 1.0)
        for s, ref_usada in ((ref, ref),):
            r = raposa([0, 0, 0], [{'t': 0, 'andar': [1, 0, 0], 'correr': correr}], ik=False)
            r['personagem']['velocidade' if not correr else 'velocidade_correndo'] = s
            r['personagem']['visual']['velocidade_andar' if not correr else 'velocidade_correr'] = ref_usada
            mundo = apoios(rodar(b, pasta, f'mundo_{clipe}_{s}_{ref_usada}', [chao(), r], 3.5), 1.5)
            for pata in ('tras_esq', 'tras_dir'):
                prev = abs(parado[pata] * s / ref_usada - s)
                erro = abs(mundo[pata] - prev)
                ok &= erro < 0.25
                textos.append(f'{clipe} a {s:.2f} m/s (ref {ref_usada}) {pata}: {mundo[pata]:.2f} m/s, previsto {prev:.2f}')
    return ok, '; '.join(textos) + ' (critério: medido = previsto ± 0,25 m/s)'


def saltos(q, desde, ate):
    """Maior deslocamento de um osso entre dois quadros, descontado o movimento da bacia (m)."""
    maior = 0.0
    seq = [x for x in q if desde <= x[0] <= ate]
    for (_, a), (_, bb) in zip(seq, seq[1:]):
        ha, hb = a['b_Hip_01'], bb['b_Hip_01']
        for osso in OSSOS[1:]:
            da = [a[osso][i] - ha[i] for i in range(3)]
            db = [bb[osso][i] - hb[i] for i in range(3)]
            maior = max(maior, math.dist(da, db))
    return maior


def teste_mistura(b, pasta):
    q = rodar(b, pasta, 'mistura', [chao(), raposa([0, 0, 0], [{'t': 0, 'andar': [1, 0, 0]},
                                                               {'t': 2.0, 'andar': [1, 0, 0], 'correr': True}])],
              4.5)
    andando, transicao, correndo = saltos(q, 1.0, 1.95), saltos(q, 1.95, 2.8), saltos(q, 2.8, 4.5)
    ok = transicao <= 1.15 * max(andando, correndo)
    return ok, (f'maior salto por quadro (osso em relação à bacia): andando {100 * andando:.1f} cm, na transição '
                f'{100 * transicao:.1f} cm, correndo {100 * correndo:.1f} cm')


def teste_acao(b, pasta):
    lua = ('function acao(e)\n  if tempo() > 1.5 and not pediu then e:animar("Survey", 0.3, true); pediu = true end\n'
           '  if tempo() > 3.0 and not parou then e:parar_animacao(0.3); parou = true end\nend')
    q = rodar(b, pasta, 'acao', [chao(), raposa([0, 0, 0], [{'t': 0, 'andar': [1, 0, 0]}])], 4.5, lua)
    andando = saltos(q, 0.8, 1.45)
    entrada, saida = saltos(q, 1.45, 2.0), saltos(q, 2.95, 3.5)
    ok = max(entrada, saida) <= 1.15 * andando
    return ok, (f'maior salto por quadro: andando {100 * andando:.1f} cm, entrando na ação {100 * entrada:.1f} cm, '
                f'saindo {100 * saida:.1f} cm')


def apoio(b, pasta, nome, terreno, chao_em, ik, y0):
    """Erro de altura de cada pata: (altura acima do chão dela) − (a mesma no chão plano), média do último segundo."""
    plano = rodar(b, pasta, f'{nome}_plano', [chao(), raposa([0, 0, 0], [{'t': 0, 'andar': [0, 0, 0]}])], 3.0)
    q = rodar(b, pasta, f'{nome}_{"ik" if ik else "sem"}',
              [chao()] + terreno + [raposa([0, y0, 0], [{'t': 0, 'andar': [0, 0, 0]}], ik=ik)], 3.0)
    erros = {}
    for nome_pata, osso in PATAS.items():
        es = []
        for (t, o), (_, p) in zip(q, plano):
            if t < 2.0:
                continue
            x, y, z = o[osso]
            es.append((y - chao_em(x, z)) - p[osso][1])
        erros[nome_pata] = sum(es) / len(es)
    return erros


def teste_terreno(b, pasta, nome, terreno, chao_em, y0):
    com = apoio(b, pasta, nome, terreno, chao_em, True, y0)
    sem = apoio(b, pasta, nome, terreno, chao_em, False, y0)
    pior_com = max(abs(v) for v in com.values())
    pior_sem = max(abs(v) for v in sem.values())
    detalhe = ', '.join(f'{k} {100 * v:+.1f}' for k, v in com.items())
    return pior_com < 0.02, (f'com IK, pior pata a {100 * pior_com:.1f} cm do chão ({detalhe} cm); '
                             f'sem IK, pior a {100 * pior_sem:.1f} cm')


def teste_degrau(b, pasta):
    # Degrau de 15 cm sob as patas da frente (a raposa olha para +Z: patas da frente em z = +0,22, de trás em -0,32).
    degrau = {'nome': 'degrau', 'forma': 'caixa', 'meia_extensao': [2, 0.075, 1], 'posicao': [0, 0.075, 1.1],
              'fisica': {'tipo': 'estatico'}}
    return teste_terreno(b, pasta, 'degrau', [degrau], lambda x, z: 0.15 if z > 0.1 else 0.0, 0.3)


def teste_rampa(b, pasta):
    a = math.radians(15)
    n = (0.0, math.cos(a), -math.sin(a))  # normal do topo (rotação de -15° em X: +Z sobe)
    rampa = {'nome': 'rampa', 'forma': 'caixa', 'meia_extensao': [3, 0.1, 3], 'rotacao_graus': [-15, 0, 0],
             'posicao': [0, 0.5 - 0.1 * n[1], -0.1 * n[2]], 'fisica': {'tipo': 'estatico'}}
    return teste_terreno(b, pasta, 'rampa', [rampa], lambda x, z: 0.5 + math.tan(a) * z, 0.8)


TESTES = {'frente': teste_frente, 'patinar': teste_patinar, 'mistura': teste_mistura, 'acao': teste_acao,
          'degrau': teste_degrau, 'rampa': teste_rampa}


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--so')
    a = ap.parse_args()
    nomes = a.so.split(',') if a.so else list(TESTES)
    oks = []
    with tempfile.TemporaryDirectory() as pasta:
        for n in nomes:
            try:
                ok, info = TESTES[n](a.bin, pasta)
            except Exception as e:  # noqa: BLE001
                ok, info = False, f'erro: {e}'
            print(f'{n:8s} {"PASSOU" if ok else "FALHOU"}: {info}', flush=True)
            oks.append(ok)
    sys.exit(0 if all(oks) else 1)


if __name__ == '__main__':
    main()
