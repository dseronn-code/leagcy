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


TESTES = {'queda': teste_queda, 'quique': teste_quique, 'pilha': teste_pilha, 'tunel': teste_tunel,
          'deterministico': teste_deterministico}


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
