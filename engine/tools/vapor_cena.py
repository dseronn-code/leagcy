#!/usr/bin/env python3
"""Sincroniza as fumaças de vapor da cena da Times Square com os pontos gerados pela cidade. Uso (de engine/):

    python3 tools/vapor_cena.py [--cena cenas/times_square.json] [--max 40] [--raio 260]

tools/times_square.py grava cenas/times_square/vapor.json com os pontos de vapor (bueiros, chaminés laranja e branco,
chapas dos carrinhos de comida). Este script troca as entidades "vaporNN" da cena por uma por ponto (os mais perto da
praça primeiro, até --max), mantendo os comentários do começo do arquivo e o resto da cena como está.
"""
import argparse
import json
import math
import os
import re

MOTOR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cena', default=os.path.join(MOTOR, 'cenas', 'times_square.json'))
    ap.add_argument('--max', type=int, default=40)
    ap.add_argument('--raio', type=float, default=260.0, help='só pontos até esta distância do centro da praça (m)')
    a = ap.parse_args()
    texto = open(a.cena, encoding='utf-8').read()
    cabecalho = ''.join(l for l in texto.splitlines(True) if l.lstrip().startswith('//') and texto.index(l) < texto.index('{'))
    corpo = re.sub(r'^\s*//.*$', '', texto, flags=re.M)
    cena = json.loads(corpo)
    pontos = json.load(open(os.path.join(os.path.dirname(a.cena), 'times_square', 'vapor.json')))
    pontos = sorted((p for p in pontos if math.hypot(p[0], p[1]) < a.raio), key=lambda p: math.hypot(p[0], p[1]))[:a.max]
    resto = [e for e in cena['entidades'] if not str(e.get('nome', '')).startswith('vapor')]
    i = next((k for k, e in enumerate(resto) if e.get('nome') == 'cidade'), 0) + 1
    novos = [{'nome': f'vapor{k:02d}', 'posicao': [round(x, 2), 0.02, round(z, 2)],
              'particulas': {'tipo': 'fumaca', 'raio': 0.45, 'escala': 0.9, 'quantidade': 220}}
             for k, (x, z) in enumerate(pontos)]
    cena['entidades'] = resto[:i] + novos + resto[i:]
    with open(a.cena, 'w', encoding='utf-8') as f:
        f.write(cabecalho + json.dumps(cena, ensure_ascii=False, indent=1) + '\n')
    print(f'{len(novos)} fumaças de vapor na cena ({len(pontos)} pontos até {a.raio:.0f} m da praça)')


if __name__ == '__main__':
    main()
