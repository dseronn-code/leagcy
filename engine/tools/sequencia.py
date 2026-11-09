#!/usr/bin/env python3
"""Testes do sequenciador de cutscenes (scripts/lib/sequencia.lua). Uso (de engine/):

    python3 tools/sequencia.py [--bin build/legacy] [--so camera,curva,...]

Cena: uma caixa vermelha em +X, uma azul em -X e o jogador na origem. Um script toca uma cena de 4 s com três chaves
de câmera (olhando a vermelha, de cima para o meio, olhando a azul) e eventos em 0,5 / 2 / 3,5 s.
  - camera:   imagem de verdade (sem janela): no começo o centro da tela é vermelho; no fim, azul; depois da cena a
              câmera volta para a do jogo.
  - curva:    a câmera passa pelas três chaves (a menos de um passo de quadro) e anda sem trancos: a velocidade muda
              pouco de um quadro para o outro.
  - eventos:  cada evento roda no quadro do seu tempo, na ordem, e ao_terminar em 4 s.
  - travar:   W segurado durante a cena não move o jogador; depois da cena move.
  - pular:    Enter em 1 s termina a cena na hora; os eventos `chamar` que faltavam rodam; o jogador volta a andar.
  - tarjas:   com janela (xvfb no Linux): tarjas pretas em cima e embaixo durante a cena, com a legenda.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

FPS = 30
LIB = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'scripts', 'lib', 'sequencia.lua'))

LUA = '''incluir("%(lib)s")
local s = sequencia.nova{
  camera = {
    {t = 0, pos = {0, 1.5, 8}, alvo = {5, 1, 0}, fov = 40},
    {t = 1.5, pos = {0, 4, 10}, alvo = {0, 1, 0}},
    {t = 3, pos = {0, 1.5, 8}, alvo = {-5, 1, 0}},
  },
  eventos = {
    {t = 0.5, chamar = function() log("EVENTO a", s_tempo()) end},
    {t = %(fala)s, fala = "Guarda: quem vem lá?", duracao = 2},
    {t = 2.0, chamar = function() log("EVENTO b", s_tempo()) end},
    {t = 3.5, chamar = function() log("EVENTO c", s_tempo()) end},
  },
  duracao = %(duracao)s,
}
function s_tempo() return s:tempo() end
s.ao_terminar = function() log("FIM", s:tempo()) end
function atualizar(e, dt)
  if not iniciou then iniciou = true; s:tocar() end
  s:atualizar(dt)
  local x, y, z = achar("jogador"):posicao()
  local p = s.cam_pos or {0, 0, 0}
  log("Q", tempo(), s:tempo(), s:tocando() and 1 or 0, p[1], p[2], p[3], x, z)
end
'''


def cena(pasta, nome, duracao=4, fala=1.0):
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write(LUA % {'lib': LIB.replace('\\', '/'), 'duracao': duracao, 'fala': fala})
    return {
        'camera': {'posicao': [0, 2, 12], 'alvo': [0, 1, 0]},
        'materiais': {'verm': {'cor': [0.9, 0.02, 0.02], 'rugosidade': 0.9},
                      'azul': {'cor': [0.02, 0.05, 0.9], 'rugosidade': 0.9},
                      'cinza': {'cor': [0.5, 0.5, 0.5], 'rugosidade': 0.9}},
        'entidades': [
            {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [30, 0.5, 30], 'posicao': [0, -0.5, 0],
             'material': 'cinza', 'fisica': {'tipo': 'estatico'}},
            {'nome': 'vermelha', 'forma': 'caixa', 'meia_extensao': [1, 1, 1], 'posicao': [5, 1, 0], 'material': 'verm',
             'fisica': {'tipo': 'estatico'}},
            {'nome': 'azul', 'forma': 'caixa', 'meia_extensao': [1, 1, 1], 'posicao': [-5, 1, 0], 'material': 'azul',
             'fisica': {'tipo': 'estatico'}},
            {'nome': 'jogador', 'posicao': [0, 0.9, 3], 'personagem': {'altura': 1.8, 'raio': 0.3, 'controle': 'jogador'}},
            {'nome': 'diretor', 'forma': 'esfera', 'raio': 0.01, 'posicao': [0, -50, 0], 'script': lua},
        ]}


def rodar(b, pasta, nome, segundos, simular='', largura=32, altura=18, duracao=4):
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(cena(pasta, nome, duracao), f, ensure_ascii=False)
    out = os.path.join(pasta, f'{nome}.png')
    cmd = [b, '--headless', '--scene', arq, '--width', str(largura), '--height', str(altura), '--fps', str(FPS),
           '--frames', str(int(round(segundos * FPS))), '--no-motion-blur', '--out', out]
    if simular:
        cmd += ['--simular', simular]
    p = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    linhas = [l.split('[script] ', 1)[1].strip() for l in (p.stdout + p.stderr).splitlines() if '[script] ' in l]
    quadros = []
    for l in linhas:
        if l.startswith('Q'):
            v = [float(x) for x in l.split()[1:]]
            quadros.append({'t': v[0], 'st': v[1], 'ativa': v[2] > 0.5, 'cam': tuple(v[3:6]), 'jog': (v[6], v[7])})
    return quadros, [l for l in linhas if not l.startswith('Q')], out


def centro(png):
    from PIL import Image
    img = Image.open(png).convert('RGB')
    w, h = img.size
    px = [img.getpixel((w // 2 + dx, h // 2 + dy)) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]
    return tuple(sum(c[i] for c in px) / len(px) for i in range(3))


def cor(rgb):
    r, g, b = rgb
    if r > 1.5 * b and r > 1.5 * g:
        return 'vermelho'
    if b > 1.5 * r and b > 1.2 * g:
        return 'azul'
    return 'outro'


def teste_camera(b, pasta):
    res = []
    for nome, seg in (('cam_inicio', 0.3), ('cam_fim', 3.8), ('cam_depois', 5.0)):
        _, _, png = rodar(b, pasta, nome, seg, largura=96, altura=54)
        res.append(cor(centro(png)))
    ok = res[0] == 'vermelho' and res[1] == 'azul' and res[2] == 'outro'
    return ok, f'centro da tela: 0,3 s {res[0]}, 3,8 s {res[1]}, depois da cena {res[2]} (câmera do jogo)'


def teste_curva(b, pasta):
    q, _, _ = rodar(b, pasta, 'curva', 4.0)
    q = [x for x in q if x['ativa']]
    chaves = {0.0: (0, 1.5, 8), 1.5: (0, 4, 10), 3.0: (0, 1.5, 8)}
    passos = [math.dist(q[i]['cam'], q[i - 1]['cam']) for i in range(1, len(q))]
    maior = max(passos)
    pior_chave = 0.0
    for tk, pk in chaves.items():
        perto = min(q, key=lambda x: abs(x['st'] - tk))
        pior_chave = max(pior_chave, math.dist(perto['cam'], pk) - maior)
    vel = [p * FPS for p in passos]
    tranco = max(abs(vel[i] - vel[i - 1]) for i in range(1, len(vel)))
    ok = pior_chave <= 0.0 and tranco < 0.1 * max(vel)
    return ok, (f'passa pelas chaves (a menos de 1 passo: {maior * 100:.1f} cm); maior mudança de velocidade entre '
                f'quadros {tranco:.3f} m/s (velocidade máxima {max(vel):.2f} m/s)')


def teste_eventos(b, pasta):
    _, ev, _ = rodar(b, pasta, 'eventos', 4.5)
    tempos = {}
    ordem = []
    for l in ev:
        p = l.split()
        if p[0] == 'EVENTO':
            tempos[p[1]] = float(p[2])
            ordem.append(p[1])
        elif p[0] == 'FIM':
            tempos['fim'] = float(p[1])
            ordem.append('fim')
    alvo = {'a': 0.5, 'b': 2.0, 'c': 3.5, 'fim': 4.0}
    erro = max((tempos[k] - v) if k in tempos else 99 for k, v in alvo.items())
    ok = ordem == ['a', 'b', 'c', 'fim'] and 0 <= erro <= 1.0 / FPS + 1e-4
    return ok, f'ordem {ordem}; maior atraso {erro * 1000:.0f} ms (um quadro = {1000 / FPS:.0f} ms)'


def teste_travar(b, pasta):
    q, _, _ = rodar(b, pasta, 'travar', 6.0, simular='0.2:segurar:w:5.5')
    dentro = [x for x in q if x['ativa']]
    fora = [x for x in q if not x['ativa'] and x['t'] > 4.2]
    mov_dentro = math.dist(dentro[0]['jog'], dentro[-1]['jog'])
    mov_fora = math.dist(fora[0]['jog'], fora[-1]['jog'])
    ok = mov_dentro < 0.05 and mov_fora > 0.5
    return ok, f'W segurado: andou {mov_dentro * 100:.1f} cm durante a cena e {mov_fora:.2f} m depois'


def teste_pular(b, pasta):
    q, ev, _ = rodar(b, pasta, 'pular', 3.0, simular='1.0:tecla:enter;1.3:segurar:w:1.5')
    fim_cena = next(x['t'] for x in q if not x['ativa'])
    chamados = [l.split()[1] for l in ev if l.startswith('EVENTO')]
    depois = [x for x in q if x['t'] > 1.4]
    andou = math.dist(depois[0]['jog'], depois[-1]['jog'])
    ok = abs(fim_cena - 1.0) <= 2.0 / FPS and chamados == ['a', 'b', 'c'] and andou > 0.5
    return ok, f'Enter em 1,0 s: cena acabou em {fim_cena:.2f} s; eventos chamados {chamados}; andou {andou:.2f} m depois'


def teste_tarjas(b, pasta):
    from PIL import Image
    arq = os.path.join(pasta, 'tarjas.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(cena(pasta, 'tarjas', duracao=60, fala=0.0), f, ensure_ascii=False)
    tela = os.path.join(pasta, 'tarjas.png')
    cmd = [b, '--scene', arq, '--width', '640', '--height', '360', '--quit-after', '3', '--screenshot', tela]
    if sys.platform.startswith('linux') and not os.environ.get('DISPLAY'):
        cmd = ['xvfb-run', '-a'] + cmd
    subprocess.run(cmd, check=True, capture_output=True)
    img = Image.open(tela).convert('RGB')
    w, h = img.size
    cima = max(max(img.getpixel((int(fx * w), int(0.04 * h)))) for fx in (0.1, 0.3, 0.7, 0.9))
    baixo = max(max(img.getpixel((int(fx * w), int(0.97 * h)))) for fx in (0.1, 0.3))
    meio = max(img.getpixel((w // 2, h // 2)))
    # Legenda: pixels claros na tarja de baixo, perto do centro.
    claros = sum(1 for x in range(int(0.3 * w), int(0.7 * w)) for y in range(int(0.89 * h), int(0.94 * h))
                 if min(img.getpixel((x, y))) > 180)
    ok = cima <= 3 and baixo <= 3 and meio > 20 and claros > 20
    return ok, f'tarja de cima {cima}, de baixo {baixo} (0 = preto); meio da tela {meio}; {claros} pixels de legenda'


TESTES = {'camera': teste_camera, 'curva': teste_curva, 'eventos': teste_eventos, 'travar': teste_travar,
          'pular': teste_pular, 'tarjas': teste_tarjas}


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
    with tempfile.TemporaryDirectory() as pasta:
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
