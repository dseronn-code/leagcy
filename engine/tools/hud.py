#!/usr/bin/env python3
"""Teste da interface de jogo (HUD montado por script Lua). Uso (de dentro de engine/):

    python3 tools/hud.py [--bin build/legacy]

Abre a janela (no Linux sem tela, use xvfb-run), deixa um script desenhar um retângulo vermelho, uma barra verde
pela metade sobre fundo azul e um texto branco centralizado, grava a tela e confere os pixels:
  - cores: os pixels saem exatamente na cor pedida (sRGB), sem desvio de gama;
  - posição: retângulo e barra no lugar pedido em fração da tela; a barra cheia até a metade;
  - texto: há texto claro em volta do centro da linha e ele está centralizado (mesma folga dos dois lados).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

from PIL import Image

LUA = '''
function atualizar(e, dt)
  hud.retangulo(0.05, 0.05, 0.2, 0.1, {1, 0, 0, 1})
  hud.barra(0.05, 0.8, 0.4, 0.05, 0.5, {0, 1, 0}, {0, 0, 1, 1})
  hud.texto("Velocidade: 120 km/h", 0.5, 0.4, {tamanho = 0.08, cor = {1, 1, 1}, alinhar = "centro"})
end
'''


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    a = ap.parse_args()
    W, H = 800, 450
    with tempfile.TemporaryDirectory() as pasta:
        script = os.path.join(pasta, 'hud.lua')
        with open(script, 'w', encoding='utf-8') as f:
            f.write(LUA)
        cena = {'camera': {'posicao': [0, 3, 10], 'alvo': [0, 0, 0]},
                'entidades': [{'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [20, 0.5, 20], 'posicao': [0, -0.5, 0],
                               'fisica': {'tipo': 'estatico'}},
                              {'nome': 'ui', 'forma': 'caixa', 'meia_extensao': [0.1, 0.1, 0.1], 'posicao': [0, 0.1, 0],
                               'script': script}]}
        caminho = os.path.join(pasta, 'hud.json')
        with open(caminho, 'w', encoding='utf-8') as f:
            json.dump(cena, f)
        tela = os.path.join(pasta, 'tela.png')
        subprocess.run([a.bin, '--scene', caminho, '--width', str(W), '--height', str(H), '--quit-after', '2',
                        '--screenshot', tela], check=True, capture_output=True)
        img = Image.open(tela).convert('RGB')
        px = lambda fx, fy: img.getpixel((int(fx * W), int(fy * H)))
        ok_todos = True

        def checar(nome, ok, info):
            nonlocal ok_todos
            ok_todos &= ok
            print(f'{nome:10s} {"PASSOU" if ok else "FALHOU"}: {info}')

        perto = lambda c, alvo: max(abs(x - y) for x, y in zip(c, alvo)) <= 2
        r, g, b = px(0.15, 0.10), px(0.15, 0.825), px(0.40, 0.825)
        checar('cores', perto(r, (255, 0, 0)) and perto(g, (0, 255, 0)) and perto(b, (0, 0, 255)),
               f'retângulo {r}, barra {g}, fundo da barra {b}')
        # Bordas: logo fora do retângulo não é vermelho; a barra troca de verde para azul na metade (x = 0,25).
        fora = px(0.27, 0.10)
        troca = [x for x in range(int(0.06 * W), int(0.44 * W)) if img.getpixel((x, int(0.825 * H)))[1] < 128]
        meio = troca[0] / W if troca else -1
        checar('posicao', not perto(fora, (255, 0, 0)) and abs(meio - 0.25) < 0.005,
               f'fora do retângulo {fora}; a barra fica azul a partir de x = {meio:.3f} (esperado 0,250)')
        # Texto: pixels quase brancos na faixa da linha (y de 0,40 a 0,48).
        claros = [x for y in range(int(0.40 * H), int(0.48 * H)) for x in range(W)
                  if min(img.getpixel((x, y))) > 235]
        if claros:
            esq, dir_ = min(claros) / W, max(claros) / W
            checar('texto', len(claros) > 300 and abs((esq + dir_) / 2 - 0.5) < 0.01,
                   f'{len(claros)} pixels de texto, de x = {esq:.3f} a {dir_:.3f} (centro {(esq + dir_) / 2:.3f})')
        else:
            checar('texto', False, 'nenhum pixel de texto')
    sys.exit(0 if ok_todos else 1)


if __name__ == '__main__':
    main()
