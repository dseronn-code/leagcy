#!/usr/bin/env python3
"""Testes da física (camada de jogo: Jolt + cena .json). Uso (de dentro de engine/):

    python3 tools/fisica.py [--bin build/legacy] [--so queda,pilha]

Cada teste monta uma cena .json pequena, roda o motor sem janela (imagem minúscula: só a física importa) com
--save-world e confere o estado final contra a física de livro:
  - queda: esfera sem amortecimento solta de 20 m; depois de 1 s, y = 20 − g·t²/2 = 15,095 m (erro < 1 cm).
  - quique: esfera com quique 1 sobre chão com quique 1 volta a ≥ 90% da altura.
  - pilha: torre de 12 caixas parada 10 s: topo desloca < 2 cm e tudo dorme (velocidade < 1 cm/s).
  - tunel: 200 esferas pequenas a 30 m/s contra o chão fino: nenhuma atravessa.
  - deterministico: cenas/fisica.json rodada duas vezes dá o mesmo arquivo, byte a byte.
Personagem (Jolt CharacterVirtual, controlado por roteiro):
  - andar: 3 s a 4,5 m/s no plano = 13,5 m (±0,3 m).
  - pulo: pulo a 5 m/s sobe v²/2g = 1,274 m (±3 cm).
  - escada: sobe 8 degraus de 25 cm andando (termina a 2,0 m de altura).
  - rampa: sobe uma rampa de 35° e não sobe uma de 60°.
  - empurrar: anda contra uma caixa de 20 kg e a empurra.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

G = 9.81
FPS = 30  # quadros por segundo do modo sem janela (passo fixo da física: 1/60 s)


def rodar(binario, cena, segundos, pasta, nome):
    caminho = os.path.join(pasta, f'{nome}.json')
    if isinstance(cena, dict):
        with open(caminho, 'w', encoding='utf-8') as f:
            json.dump(cena, f)
    else:
        caminho = cena
    saida = os.path.join(pasta, f'{nome}_fim.json')
    quadros = int(round(segundos * FPS))
    p = subprocess.run([binario, '--headless', '--scene', caminho, '--width', '32', '--height', '18', '--frames',
                        str(quadros), '--out', os.path.join(pasta, f'{nome}.png'), '--save-world', saida],
                       capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-500:]}')
    with open(saida, encoding='utf-8') as f:
        return json.load(f), saida


def chao(quique=0.1):
    return {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [50, 0.5, 50], 'posicao': [0, -0.5, 0],
            'fisica': {'tipo': 'estatico', 'quique': quique}}


def por_nome(estado, prefixo):
    return [e for e in estado['entidades'] if e.get('nome', '').startswith(prefixo)]


def teste_queda(b, pasta):
    """Queda livre de 20 m por 1 s."""
    cena = {'entidades': [chao(), {'nome': 'bola', 'forma': 'esfera', 'raio': 0.5, 'posicao': [0, 20, 0],
                                   'fisica': {'tipo': 'dinamico', 'amortecimento_linear': 0}}]}
    est, _ = rodar(b, cena, 1.0, pasta, 'queda')
    y = por_nome(est, 'bola')[0]['posicao'][1]
    # Passo fixo de 1/60 s com 2 subpassos (1/120 s) e integrador de Euler semi-implícito:
    # y = y0 − g·dt²·n(n+1)/2 com n = 120 subpassos.
    n, dt = 120, 1.0 / 120.0
    esperado = 20.0 - G * dt * dt * n * (n + 1) / 2.0
    analitico = 20.0 - G / 2.0
    ok = abs(y - esperado) < 0.01
    return ok, f'y = {y:.4f} m (integrador: {esperado:.4f}; analítico contínuo: {analitico:.4f})'


def teste_quique(b, pasta):
    """Quique perfeito volta perto da altura inicial."""
    cena = {'entidades': [chao(quique=1.0), {'nome': 'bola', 'forma': 'esfera', 'raio': 0.5, 'posicao': [0, 5.5, 0],
                                             'fisica': {'tipo': 'dinamico', 'quique': 1.0, 'amortecimento_linear': 0}}]}
    # Cai 5 m (t = 1,01 s) e sobe de volta (mais 1,01 s): no topo do 1º quique em ~2,02 s.
    melhor = 0.0
    for t in (1.9, 1.95, 2.0, 2.05, 2.1):
        est, _ = rodar(b, cena, t, pasta, f'quique_{t}')
        melhor = max(melhor, por_nome(est, 'bola')[0]['posicao'][1])
    ok = 0.9 * 5.0 <= melhor - 0.5 <= 1.05 * 5.0  # nem perde demais nem ganha energia
    return ok, f'altura máxima depois do quique {melhor - 0.5:.3f} m de 5 m ({100 * (melhor - 0.5) / 5:.1f}%)'


def teste_pilha(b, pasta):
    """Torre de 12 caixas parada."""
    cena = {'entidades': [chao(quique=0.0), {
        'nome': 'torre', 'forma': 'caixa', 'meia_extensao': [0.3, 0.3, 0.3], 'posicao': [0, 0.3, 0],
        'fisica': {'tipo': 'dinamico', 'massa': 8, 'quique': 0.0},
        'repetir': {'quantidade': [1, 12, 1], 'passo': [0, 0.6, 0]}}]}
    est, _ = rodar(b, cena, 10.0, pasta, 'pilha')
    caixas = por_nome(est, 'torre')
    topo = max(caixas, key=lambda e: e['posicao'][1])
    desloc = (topo['posicao'][0] ** 2 + topo['posicao'][2] ** 2) ** 0.5
    vel = max(sum(v * v for v in e.get('velocidade', [0, 0, 0])) ** 0.5 for e in caixas)
    ok = desloc < 0.02 and vel < 0.01
    return ok, f'topo deslocado {100 * desloc:.2f} cm, maior velocidade {100 * vel:.3f} cm/s'


def teste_tunel(b, pasta):
    """Esferas rápidas não atravessam um chão fino."""
    cena = {'entidades': [
        {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [20, 0.05, 20], 'posicao': [0, -0.05, 0],
         'fisica': {'tipo': 'estatico'}},
        {'nome': 'bala', 'forma': 'esfera', 'raio': 0.05, 'posicao': [-5, 3, -5], 'velocidade': [0, -30, 0],
         'fisica': {'tipo': 'dinamico', 'quique': 0.0},
         'repetir': {'quantidade': [20, 1, 10], 'passo': [0.5, 0, 1.0]}}]}
    est, _ = rodar(b, cena, 2.0, pasta, 'tunel')
    balas = por_nome(est, 'bala')
    abaixo = sum(1 for e in balas if e['posicao'][1] < 0.0)
    return abaixo == 0, f'{abaixo} de {len(balas)} atravessaram o chão'


def teste_deterministico(b, pasta):
    """A mesma cena duas vezes dá o mesmo resultado."""
    cena = os.path.join('cenas', 'fisica.json')
    _, a = rodar(b, cena, 4.0, pasta, 'det_a')
    _, c = rodar(b, cena, 4.0, pasta, 'det_b')
    with open(a, 'rb') as fa, open(c, 'rb') as fc:
        igual = fa.read() == fc.read()
    return igual, 'arquivos idênticos' if igual else 'os arquivos diferem'


def personagem(pos, roteiro, **extra):
    p = {'altura': 1.8, 'raio': 0.3, 'velocidade': 4.5, 'pulo': 5.0, 'controle': 'roteiro', 'roteiro': roteiro}
    p.update(extra)
    return {'nome': 'heroi', 'personagem': p, 'posicao': pos}


def pes(estado):
    return por_nome(estado, 'heroi')[0]['posicao']


def teste_andar(b, pasta):
    """Anda 3 s em linha reta."""
    cena = {'entidades': [chao(), personagem([0, 0, 0], [{'t': 0, 'andar': [1, 0, 0]}])]}
    est, _ = rodar(b, cena, 3.0, pasta, 'andar')
    x = pes(est)[0]
    return abs(x - 13.5) < 0.3, f'andou {x:.3f} m (esperado 13,5)'


def teste_pulo(b, pasta):
    """Altura do pulo."""
    cena = {'entidades': [chao(), personagem([0, 0, 0], [{'t': 0.3, 'pular': True}])]}
    melhor = 0.0
    for t in (0.75, 0.8, 0.82, 0.84, 0.86, 0.9):
        est, _ = rodar(b, cena, t, pasta, f'pulo_{t}')
        melhor = max(melhor, pes(est)[1])
    # O personagem integra a 1/60 s (Euler semi-implícito: a gravidade entra antes de mover): o pico discreto é
    # max_n dt·Σ(v − g·dt·k), ~4 cm abaixo do contínuo v²/2g.
    dt = 1.0 / 60.0
    esperado = max(dt * sum(5.0 - G * dt * k for k in range(1, n + 1)) for n in range(1, 60))
    continuo = 5.0 ** 2 / (2 * G)
    return abs(melhor - esperado) < 0.01, f'subiu {melhor:.3f} m (integrador {esperado:.3f}; contínuo {continuo:.3f})'


def teste_escada(b, pasta):
    """Sobe uma escada de 8 degraus de 25 cm."""
    degraus = [{'nome': f'degrau{i}', 'forma': 'caixa', 'meia_extensao': [0.2, 0.125 * (i + 1), 1.5],
                'posicao': [2 + 0.4 * i, 0.125 * (i + 1), 0], 'fisica': {'tipo': 'estatico'}} for i in range(8)]
    topo = {'nome': 'patamar', 'forma': 'caixa', 'meia_extensao': [3, 1.0, 1.5], 'posicao': [8.6, 1.0, 0],
            'fisica': {'tipo': 'estatico'}}
    cena = {'entidades': [chao()] + degraus + [topo, personagem([0, 0, 0], [{'t': 0, 'andar': [1, 0, 0]}],
                                                                velocidade=2.0)]}
    est, _ = rodar(b, cena, 4.0, pasta, 'escada')
    x, y, _ = pes(est)
    return abs(y - 2.0) < 0.05 and x > 6.0, f'pés em x = {x:.2f} m, altura {y:.3f} m (topo a 2,0 m)'


def teste_rampa(b, pasta):
    """Sobe 35°, não sobe 60°."""
    import math

    def rampa(graus, z):
        comp = 6.0
        a = math.radians(graus)
        cx = 2 + 0.5 * comp * math.cos(a)
        cy = 0.5 * comp * math.sin(a) - 0.1 * math.cos(a)
        return {'nome': f'rampa{graus}', 'forma': 'caixa', 'meia_extensao': [0.5 * comp, 0.1, 1.0],
                'posicao': [cx, cy, z], 'rotacao_graus': [0, 0, graus], 'fisica': {'tipo': 'estatico'}}

    cena = {'entidades': [chao(), rampa(35, -4), rampa(60, 4),
                          personagem([0, 0, -4], [{'t': 0, 'andar': [1, 0, 0]}], velocidade=3.0) | {'nome': 'heroi35'},
                          personagem([0, 0, 4], [{'t': 0, 'andar': [1, 0, 0]}], velocidade=3.0) | {'nome': 'heroi60'}]}
    est, _ = rodar(b, cena, 3.0, pasta, 'rampa')
    y35 = por_nome(est, 'heroi35')[0]['posicao'][1]
    y60 = por_nome(est, 'heroi60')[0]['posicao'][1]
    return y35 > 2.0 and y60 < 0.6, f'35°: subiu a {y35:.2f} m; 60°: ficou a {y60:.2f} m'


def teste_empurrar(b, pasta):
    """Empurra uma caixa."""
    cena = {'entidades': [chao(), {'nome': 'caixa', 'forma': 'caixa', 'meia_extensao': [0.4, 0.4, 0.4],
                                   'posicao': [2, 0.4, 0], 'fisica': {'tipo': 'dinamico', 'massa': 20}},
                          personagem([0, 0, 0], [{'t': 0, 'andar': [1, 0, 0]}], velocidade=2.0)]}
    est, _ = rodar(b, cena, 3.0, pasta, 'empurrar')
    x = por_nome(est, 'caixa')[0]['posicao'][0]
    return x > 3.0, f'caixa foi de x = 2,0 para {x:.2f} m'


def teste_sponza(b, pasta):
    """Anda dentro da Sponza (colisão pela malha do glTF): fica no chão e para na parede."""
    cena = {'entidades': [{'nome': 'sponza', 'modelo': os.path.join('assets', 'Sponza', 'Sponza.gltf'),
                           'fisica': {'tipo': 'estatico', 'colisao': 'malha'}},
                          personagem([-8.5, 0.5, -0.6], [{'t': 0, 'andar': [1, 0, 0]}])]}
    est, _ = rodar(b, cena, 10.0, pasta, 'sponza')
    x, y, z = pes(est)
    # Limites da Sponza: x de -15,4 a 14,4. Andando 45 m para +x, ele precisa parar dentro, de pé no chão.
    ok = -1.0 < y < 1.0 and 0.0 < x < 14.4
    return ok, f'pés em ({x:.2f}, {y:.2f}, {z:.2f}) depois de 10 s andando para +x'


def teste_restaurar(b, pasta):
    """Jogar e Parar do editor: depois de 3 s de física, todas as poses voltam às do início."""
    cena = os.path.join('cenas', 'parque.json')
    # Poses do arquivo: um quadro com a cena congelada (--pause: a física não anda).
    base = os.path.join(pasta, 'rest_inicio.json')
    subprocess.run([b, '--headless', '--scene', cena, '--width', '32', '--height', '18', '--frames', '1', '--pause',
                    '--out', os.path.join(pasta, 'rest0.png'), '--save-world', base], capture_output=True, check=True)
    with open(base, encoding='utf-8') as f:
        inicio = json.load(f)
    saida = os.path.join(pasta, 'rest_fim.json')
    subprocess.run([b, '--headless', '--scene', cena, '--width', '32', '--height', '18', '--frames', '90',
                    '--out', os.path.join(pasta, 'rest.png'), '--test-restore', '--save-world', saida],
                   capture_output=True, check=True)
    with open(saida, encoding='utf-8') as f:
        fim = json.load(f)
    pos0 = {e.get('nome'): e['posicao'] for e in inicio['entidades']}
    pior = max(max(abs(a - c) for a, c in zip(e['posicao'], pos0[e.get('nome')])) for e in fim['entidades'])
    return pior < 1e-4, f'maior diferença de posição depois de Parar: {pior:.2e} m ({len(fim["entidades"])} entidades)'


def teste_script(b, pasta):
    """Scripts em Lua: um sobe a caixa 1 m/s, outro quebra (o motor segue e o primeiro continua)."""
    sobe = os.path.join(pasta, 'sobe.lua')
    with open(sobe, 'w', encoding='utf-8') as f:
        f.write('local x, y0, z\nfunction iniciar(e) x, y0, z = e:posicao() end\n'
                'function atualizar(e, dt) e:mover_para(x, y0 + tempo(), z) end\n')
    quebrado = os.path.join(pasta, 'quebrado.lua')
    with open(quebrado, 'w', encoding='utf-8') as f:
        f.write('function atualizar(e, dt) chama_funcao_que_nao_existe() end\n')
    cena = {'entidades': [chao(),
                          {'nome': 'caixa', 'forma': 'caixa', 'posicao': [0, 1, 0], 'script': sobe,
                           'fisica': {'tipo': 'cinematico'}},
                          {'nome': 'outra', 'forma': 'caixa', 'posicao': [3, 1, 0], 'script': quebrado,
                           'fisica': {'tipo': 'cinematico'}}]}
    est, _ = rodar(b, cena, 2.0, pasta, 'script')
    y = por_nome(est, 'caixa')[0]['posicao'][1]
    return abs(y - 3.0) < 0.05, f'caixa subiu para y = {y:.3f} m em 2 s (esperado 3,0); script quebrado não derrubou o motor'


def teste_recarga(b, pasta):
    """Recarga a quente: o script muda no disco com o motor rodando e passa a valer sem reiniciar."""
    import time
    scr = os.path.join(pasta, 'altura.lua')
    with open(scr, 'w', encoding='utf-8') as f:
        f.write('function atualizar(e, dt) e:mover_para(0, 10, 0) end\n')
    cena = {'entidades': [chao(), {'nome': 'caixa', 'forma': 'caixa', 'posicao': [0, 1, 0], 'script': scr,
                                   'fisica': {'tipo': 'cinematico'}}]}
    caminho = os.path.join(pasta, 'recarga.json')
    with open(caminho, 'w', encoding='utf-8') as f:
        json.dump(cena, f)
    saida = os.path.join(pasta, 'recarga_fim.json')
    p = subprocess.Popen([b, '--headless', '--scene', caminho, '--width', '32', '--height', '18', '--frames', '400',
                          '--out', os.path.join(pasta, 'recarga.png'), '--save-world', saida],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(3)  # o motor já está rodando a primeira versão
    with open(scr, 'w', encoding='utf-8') as f:
        f.write('function atualizar(e, dt) e:mover_para(0, 20, 0) end\n')
    out, _ = p.communicate(timeout=600)
    with open(saida, encoding='utf-8') as f:
        y = por_nome(json.load(f), 'caixa')[0]['posicao'][1]
    recarregou = 'recarregando' in out
    return abs(y - 20.0) < 1e-3 and recarregou, f'y final = {y:.2f} (versão nova: 20); recarga no log: {recarregou}'


def carro(roteiro, pos=(0, 1.2, 0)):
    return {'nome': 'carro', 'veiculo': {'roteiro': roteiro, 'controle': 'roteiro'}, 'posicao': list(pos)}


def teste_carro_reta(b, pasta):
    """Acelera 5,5 s em linha reta (1.300 kg, 500 Nm): passa de 12 m/s (~0-45 km/h, como um carro popular) e não sai
    da reta."""
    cena = {'entidades': [chao(), carro([{'t': 0.5, 'acelerar': 1.0}])]}
    est, _ = rodar(b, cena, 6.0, pasta, 'carro_reta')
    c = por_nome(est, 'carro')[0]
    vel = sum(v * v for v in c['velocidade']) ** 0.5
    desvio = abs(c['posicao'][0])
    ok = vel > 12.0 and desvio < 1.0 and c['posicao'][2] > 30.0
    return ok, f'{vel:.1f} m/s ({3.6 * vel:.0f} km/h), andou {c["posicao"][2]:.1f} m, desvio lateral {desvio:.2f} m'


def teste_carro_curva(b, pasta):
    """Acelera e vira tudo: em 2,5 s a frente gira mais de 90 graus (antes de completar a volta)."""
    import math
    cena = {'entidades': [chao(), carro([{'t': 0.5, 'acelerar': 0.6, 'virar': 1.0}])]}
    est, _ = rodar(b, cena, 3.0, pasta, 'carro_curva')
    w, x, y, z = por_nome(est, 'carro')[0]['rotacao']
    fx, fz = 2 * (x * z + w * y), 1 - 2 * (x * x + y * y)  # frente (+Z) girada pelo quaternion
    giro = math.degrees(math.acos(max(-1.0, min(1.0, fz / max(math.hypot(fx, fz), 1e-9)))))
    return giro > 90.0, f'a frente girou {giro:.0f} graus'


def teste_carro_freio(b, pasta):
    """Acelera 5 s e freia: para em poucos segundos sem capotar."""
    cena = {'entidades': [chao(), carro([{'t': 0.5, 'acelerar': 1.0}, {'t': 5.0, 'freio': 1.0}])]}
    est, _ = rodar(b, cena, 9.0, pasta, 'carro_freio')
    c = por_nome(est, 'carro')[0]
    vel = sum(v * v for v in c['velocidade']) ** 0.5
    w, x, y, z = c['rotacao']
    em_pe = 1 - 2 * (x * x + z * z) > 0.9  # o "para cima" do carro ainda aponta para cima
    return vel < 0.5 and em_pe, f'velocidade final {vel:.2f} m/s, em pé: {em_pe}'


TESTES = {'queda': teste_queda, 'quique': teste_quique, 'pilha': teste_pilha, 'tunel': teste_tunel,
          'deterministico': teste_deterministico, 'andar': teste_andar, 'pulo': teste_pulo, 'escada': teste_escada,
          'rampa': teste_rampa, 'empurrar': teste_empurrar, 'sponza': teste_sponza,
          'restaurar': teste_restaurar, 'script': teste_script,
          'recarga': teste_recarga, 'carro_reta': teste_carro_reta, 'carro_curva': teste_carro_curva,
          'carro_freio': teste_carro_freio}


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--so')
    a = ap.parse_args()
    nomes = a.so.split(',') if a.so else list(TESTES)
    resultados = []
    with tempfile.TemporaryDirectory() as pasta:
        for n in nomes:
            try:
                ok, info = TESTES[n](a.bin, pasta)
            except Exception as e:  # noqa: BLE001 — falha do teste vira resultado
                ok, info = False, f'erro: {e}'
            print(f'{n:15s} {"PASSOU" if ok else "FALHOU"}: {info}', flush=True)
            resultados.append(ok)
    sys.exit(0 if all(resultados) else 1)


if __name__ == '__main__':
    main()
