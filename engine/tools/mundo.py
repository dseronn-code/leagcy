#!/usr/bin/env python3
"""Testes do mundo aberto ("mundo_aberto" na cena: células carregadas pela distância do jogador). Uso (de engine/):

    python3 tools/mundo.py [--bin build/legacy] [--so inicio,anda,...]

Cena: chão de 4 km (sempre carregado: maior que uma célula), 40 caixas vermelhas paradas a cada 50 m em x (0 a 1950),
células de 50 m que carregam a 150 m e descarregam a 200 m. Um script de lógica (marcado "sempre_carregado": sem isso, a caixa padrão dele dormiria com a célula de x = 0) leva o jogador para
x = 1000 em 3 s, x = 1950 em 6 s e de volta a 0 em 9 s, e registra o estado.
  - inicio:   com o jogador em x = 0, a caixa de x = 0 está acordada e a de x = 500, dormindo; as células carregadas são
              só as perto dele.
  - anda:     com o jogador em x = 1000, a caixa de x = 0 dorme e a de x = 1000 acorda.
  - fisica:   uma bola solta sobre a caixa de x = 1000 (acordada) para em cima dela; outra, sobre a caixa de x = 1500
              (dormindo: sem corpo na física), cai até o chão.
  - volta:    de volta a x = 0, a caixa de x = 0 acorda de novo e a de x = 1950 dorme.
  - imagem:   a câmera parada olha a caixa de x = 500: com o jogador em x = 0 ela não aparece (dormindo); com o jogador
              em x = 500, aparece (vermelho no centro da imagem).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

FPS = 30

LUA = '''
function atualizar(e, dt)
  local t = tempo()
  local jog = achar("jogador")
  %(roteiro)s
  local carregadas, total = celulas()
  local function estado(n) local c = achar(n); return c and (c:dormindo() and "D" or "A") or "?" end
  local b1x, b1y = achar("bola1"):posicao()
  local b2x, b2y = achar("bola2"):posicao()
  local jx = jog:posicao()
  log("M", string.format("%%.3f %%d %%d %%s %%s %%s %%s %%.3f %%.3f %%.1f", t, carregadas, total, estado("caixa_0_0_0"),
      estado("caixa_10_0_0"), estado("caixa_20_0_0"), estado("caixa_39_0_0"), b1y, b2y, jx))
end
'''

ROTEIRO_ANDA = '''
  if t >= 3.0 and not p1 then p1 = true; jog:mover_para(1000, 0.9, 3) end
  if t >= 4.0 and not b1 then b1 = true; achar("bola1"):mover_para(1000, 4, 0) end
  if t >= 4.0 and not b2 then b2 = true; achar("bola2"):mover_para(1500, 4, 0) end
  if t >= 6.0 and not p2 then p2 = true; jog:mover_para(1950, 0.9, 3) end
  if t >= 9.0 and not p3 then p3 = true; jog:mover_para(0, 0.9, 3) end
'''


def cena(pasta, nome, roteiro, jogador_x=0.0, camera=None):
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write(LUA % {'roteiro': roteiro})
    c = {
        'mundo_aberto': {'celula': 50, 'carregar': 150, 'descarregar': 200},
        'menu': False,
        'materiais': {'verm': {'cor': [0.9, 0.03, 0.03], 'rugosidade': 0.8},
                      'cinza': {'cor': [0.4, 0.4, 0.4], 'rugosidade': 0.9}},
        'entidades': [
            {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [2100, 0.5, 60], 'posicao': [950, -0.5, 0],
             'material': 'cinza', 'fisica': {'tipo': 'estatico'}},
            {'nome': 'caixa', 'forma': 'caixa', 'meia_extensao': [0.5, 0.5, 0.5], 'posicao': [0, 0.5, 0],
             'material': 'verm', 'fisica': {'tipo': 'estatico'},
             'repetir': {'quantidade': [40, 1, 1], 'passo': [50, 0, 0]}},
            {'nome': 'bola1', 'forma': 'esfera', 'raio': 0.25, 'posicao': [-30, 0.25, 8], 'fisica': {'tipo': 'dinamico'}},
            {'nome': 'bola2', 'forma': 'esfera', 'raio': 0.25, 'posicao': [-30, 0.25, -8], 'fisica': {'tipo': 'dinamico'}},
            {'nome': 'jogador', 'posicao': [jogador_x, 0.9, 3], 'personagem': {'controle': 'jogador'}},
            {'nome': 'diretor', 'script': lua, 'sempre_carregado': True},
        ]}
    if camera:
        c['camera'] = camera
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(c, f)
    return arq


def rodar(b, pasta, nome, segundos, roteiro, jogador_x=0.0, camera=None, tamanho=(32, 18)):
    arq = cena(pasta, nome, roteiro, jogador_x, camera)
    out = os.path.join(pasta, f'{nome}.png')
    p = subprocess.run([b, '--headless', '--scene', arq, '--width', str(tamanho[0]), '--height', str(tamanho[1]),
                        '--frames', str(int(segundos * FPS)), '--fps', str(FPS), '--no-motion-blur', '--out', out],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    linhas = []
    for l in (p.stdout + p.stderr).splitlines():
        if '[script] M ' in l:
            v = l.split('[script] M ', 1)[1].split()
            linhas.append({'t': float(v[0]), 'carregadas': int(v[1]), 'total': int(v[2]), 'c0': v[3], 'c500': v[4],
                           'c1000': v[5], 'c1950': v[6], 'b1': float(v[7]), 'b2': float(v[8]), 'jx': float(v[9])})
    if not linhas:
        raise RuntimeError('o script não registrou nada: ' + (p.stdout + p.stderr)[-400:])
    return linhas, out


_cache = {}


def sessao(b, pasta):
    if 'anda' not in _cache:
        _cache['anda'] = rodar(b, pasta, 'anda', 11.0, ROTEIRO_ANDA)[0]
    return _cache['anda']


def no_tempo(linhas, t):
    return min(linhas, key=lambda x: abs(x['t'] - t))


def teste_inicio(b, pasta):
    q = no_tempo(sessao(b, pasta), 1.0)
    ok = q['c0'] == 'A' and q['c500'] == 'D' and q['c1950'] == 'D' and 0 < q['carregadas'] < q['total']
    return ok, (f'jogador em x = 0: caixa de 0 {q["c0"]}, de 500 {q["c500"]}, de 1950 {q["c1950"]} (A acordada, D '
                f'dormindo); {q["carregadas"]} de {q["total"]} células carregadas')


def teste_anda(b, pasta):
    q = no_tempo(sessao(b, pasta), 5.0)
    ok = q['c0'] == 'D' and q['c1000'] == 'A' and abs(q['jx'] - 1000) < 1
    return ok, f'jogador em x = {q["jx"]:.0f}: caixa de 0 {q["c0"]}, de 1000 {q["c1000"]}; {q["carregadas"]} células carregadas'


def teste_fisica(b, pasta):
    q = no_tempo(sessao(b, pasta), 5.9)
    ok = abs(q['b1'] - 1.25) < 0.05 and abs(q['b2'] - 0.25) < 0.05
    return ok, (f'bola sobre a caixa acordada parou em y = {q["b1"]:.3f} (em cima: 1,25); sobre a dormindo, em '
                f'y = {q["b2"]:.3f} (no chão: 0,25)')


def teste_volta(b, pasta):
    q = no_tempo(sessao(b, pasta), 10.5)
    ok = q['c0'] == 'A' and q['c1950'] == 'D' and abs(q['jx']) < 1
    return ok, f'de volta em x = {q["jx"]:.0f}: caixa de 0 {q["c0"]}, de 1950 {q["c1950"]}'


def teste_imagem(b, pasta):
    import numpy as np
    from PIL import Image
    cam = {'posicao': [500, 1.5, 6], 'alvo': [500, 0.5, 0]}
    cores = {}
    for nome, x in (('longe', 0.0), ('perto', 490.0)):
        _, png = rodar(b, pasta, f'img_{nome}', 1.0, '', jogador_x=x, camera=cam, tamanho=(96, 54))
        img = np.asarray(Image.open(png).convert('RGB')).astype(float)
        h, w, _ = img.shape
        cores[nome] = img[h // 2 - 2:h // 2 + 3, w // 2 - 2:w // 2 + 3].reshape(-1, 3).mean(0)
    vermelho = lambda c: c[0] > 1.6 * c[1] and c[0] > 1.6 * c[2]
    ok = not vermelho(cores['longe']) and vermelho(cores['perto'])
    return ok, (f'centro da tela com o jogador longe {tuple(int(v) for v in cores["longe"])}, perto '
                f'{tuple(int(v) for v in cores["perto"])} (a caixa vermelha só aparece com a célula carregada)')


LUA_TERRENO = '''
function atualizar(e, dt)
  local t = tempo()
  local jog = achar("jogador")
  local function por(ent, x, z, dy) ent:mover_para(x, altura_terreno(x, z) + dy, z) end
  if t >= 0.1 and not p0 then p0 = true; por(jog, D, D, 1.0); por(achar("bola"), D - 400, D, 2.0) end
  if t >= 3.0 and not p1 then p1 = true; por(jog, D + 400, D + 150, 1.0) end
  local jx, jy, jz = jog:posicao()
  local bx, by, bz = achar("bola"):posicao()
  local carregadas, total = celulas()
  log("T", string.format("%.3f %d %d %.3f %.3f %.3f %.3f %.3f", t, carregadas, total, jx - D, jy - altura_terreno(jx, jz),
      jz, by, altura_terreno(bx, bz)))
end
'''


def rodar_terreno(b, pasta, segundos, camera=None, tamanho=(32, 18), nome='terreno', desloc=0):
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write(f'D = {desloc}\n' + LUA_TERRENO)
    c = {'mundo_aberto': {'celula': 64, 'carregar': 128, 'descarregar': 192}, 'menu': False,
         'terreno': {'tamanho': 1024, 'altura': 60, 'escala': 300, 'resolucao': 32, 'material': 'chao',
                     'centro': [desloc, desloc]},
         'materiais': {'chao': {'cor': [0.3, 0.4, 0.2], 'rugosidade': 0.9, 'terreno': True}},
         'entidades': [
             {'nome': 'bola', 'forma': 'esfera', 'raio': 0.25, 'posicao': [-400, 100, 0], 'fisica': {'tipo': 'dinamico'}},
             {'nome': 'jogador', 'posicao': [0, 100, 0], 'personagem': {'controle': 'jogador'}},
             {'nome': 'diretor', 'script': lua, 'sempre_carregado': True}]}
    if camera:
        c['camera'] = camera
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(c, f)
    out = os.path.join(pasta, f'{nome}.png')
    p = subprocess.run([b, '--headless', '--scene', arq, '--width', str(tamanho[0]), '--height', str(tamanho[1]),
                        '--frames', str(int(segundos * FPS)), '--fps', str(FPS), '--no-motion-blur', '--out', out],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    linhas = []
    for l in (p.stdout + p.stderr).splitlines():
        if '[script] T ' in l:
            v = l.split('[script] T ', 1)[1].split()
            linhas.append({'t': float(v[0]), 'carregadas': int(v[1]), 'total': int(v[2]), 'jx': float(v[3]),
                           'jdy': float(v[4]), 'jz': float(v[5]), 'by': float(v[6]), 'bh': float(v[7])})
    if not linhas:
        raise RuntimeError('o script não registrou nada: ' + (p.stdout + p.stderr)[-400:])
    return linhas, out, p.stdout + p.stderr


def teste_terreno(b, pasta, desloc=0):
    linhas, _, log = rodar_terreno(b, pasta, 6.0, nome=f'terreno{desloc}', desloc=desloc)
    a, d = no_tempo(linhas, 2.8), no_tempo(linhas, 5.9)
    # O centro da cápsula fica a meia altura (0,9 m) acima do chão; a bola, no ladrilho dormindo, atravessa o chão.
    ok = (abs(a['jdy'] - 0.9) < 0.15 and abs(d['jdy'] - 0.9) < 0.15 and abs(d['jx'] - 400) < 2 and
          d['by'] < d['bh'] - 5 and 0 < a['carregadas'] < a['total'])
    m = re.search(r'Terreno: .*', log)
    return ok, (f'{m.group(0) if m else "?"}; jogador em pé no terreno em x = 0 (centro {a["jdy"]:.2f} m acima do chão) '
                f'e em x = 400 ({d["jdy"]:.2f} m); bola no ladrilho dormindo caiu a {d["bh"] - d["by"]:.0f} m abaixo '
                f'do chão; {a["carregadas"]} de {a["total"]} células carregadas')


def teste_longe(b, pasta):
    # O mesmo a 20 km da origem (o float tem ~2 mm de passo lá): física e posições continuam certas.
    ok, msg = teste_terreno(b, pasta, 20000)
    return ok, 'a 20 km: ' + msg.split('; ', 1)[1]


def teste_horizonte(b, pasta):
    import numpy as np
    from PIL import Image
    # Do alto, olhando para o horizonte: os ladrilhos de longe (malha simples) cobrem o chão até a borda do terreno.
    cam = {'posicao': [0, 140, 0], 'alvo': [300, 90, 0]}
    _, png, _ = rodar_terreno(b, pasta, 1.0, camera=cam, tamanho=(160, 90), nome='horizonte')
    img = np.asarray(Image.open(png).convert('RGB')).astype(float)
    baixo = img[img.shape[0] * 3 // 4:]
    ceu = (baixo[..., 2] > baixo[..., 0] + 20).mean()  # azul do céu na parte de baixo = buraco no terreno
    return ceu < 0.02, f'{100 * ceu:.1f}% de céu no quarto de baixo da imagem (buracos entre ladrilhos de perto e de longe)'


TESTES = {'inicio': teste_inicio, 'anda': teste_anda, 'fisica': teste_fisica, 'volta': teste_volta,
          'imagem': teste_imagem, 'terreno': teste_terreno, 'longe': teste_longe, 'horizonte': teste_horizonte}


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
