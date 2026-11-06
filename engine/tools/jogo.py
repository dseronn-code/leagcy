#!/usr/bin/env python3
"""Testes da camada de jogo: entrada (teclado, mouse, controle), menus, pausa e salvar/carregar. Uso (de engine/):

    python3 tools/jogo.py [--bin build/legacy] [--so botao,teclado,...]

Sem janela, com entradas simuladas (--simular) no tempo do quadro; os scripts registram o que aconteceu no log.
  - botao:     clique no meio do botão B aciona só o B, uma vez; clique fora não aciona nada.
  - teclado:   seta para baixo + Enter aciona o 2º botão (o foco começa no 1º); Tab também anda.
  - controle:  direcional para baixo + A no controle aciona o 2º botão.
  - deslizar:  clique a 75% da barra põe o valor em 0,75; depois seta para a direita soma 1/20 (0,80).
  - alternar:  clique inverte o valor; Enter com foco inverte de novo.
  - pausa:     Esc abre o menu padrão e pausa (a bola para no ar e o tempo de jogo congela, o tempo real anda);
               Enter em "Continuar" despausa e a queda continua de onde parou.
  - salvar:    salva no meio da queda (com dados do script: número, texto com acento, lista, tabela), deixa cair,
               carrega: a bola volta exatamente à posição e velocidade salvas e os dados voltam iguais.
  - analogico: analógico esquerdo para a frente anda o jogador para a frente da câmera; W segurado também.
  - particulas (imagem 480x270): o fogo preso a uma tocha que anda vai junto com ela; e:particulas(false) apaga;
               explosao() no script acende o clarão e a bola de fogo na hora.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

AQUI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MENU = os.path.join(AQUI, 'scripts', 'menu_padrao.lua')
FPS = 30


def rodar(binario, pasta, nome, entidades, segundos, simular='', lua=None, menu=False, extra=None):
    if lua is not None:
        caminho = os.path.join(pasta, f'{nome}.lua')
        with open(caminho, 'w', encoding='utf-8') as f:
            f.write(lua)
        entidades = entidades + [{'nome': 'teste', 'forma': 'esfera', 'raio': 0.01, 'posicao': [0, -50, 0],
                                  'script': caminho}]
    cena = {'camera': {'posicao': [0, 2, 10], 'alvo': [0, 2, 0]}, 'entidades': entidades,
            'menu': MENU if menu else False}
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(cena, f, ensure_ascii=False)
    cmd = [binario, '--headless', '--scene', arq, '--width', '32', '--height', '18', '--frames',
           str(int(segundos * FPS)), '--out', os.path.join(pasta, f'{nome}.png')]
    if simular:
        cmd += ['--simular', simular]
    if extra:
        cmd += ['--save-world', os.path.join(pasta, extra)]
    p = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=pasta)
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    return [l.split('[script] ', 1)[1].strip() for l in (p.stdout + p.stderr).splitlines() if '[script] ' in l]


BOTOES = '''
function atualizar(e, dt)
  if hud.botao("A", 0.1, 0.1, 0.2, 0.1) then log("ACIONOU A") end
  if hud.botao("B", 0.1, 0.3, 0.2, 0.1) then log("ACIONOU B") end
  if hud.botao("C", 0.1, 0.5, 0.2, 0.1) then log("ACIONOU C") end
end
'''


def acionados(linhas):
    return [l.split()[-1] for l in linhas if l.startswith('ACIONOU')]


def teste_botao(b, pasta):
    a = acionados(rodar(b, pasta, 'botao', [], 1.5, '0.5:clique:0.2,0.35;1.0:clique:0.6,0.35', BOTOES))
    return a == ['B'], f'acionados: {a} (esperado só B)'


def teste_teclado(b, pasta):
    a1 = acionados(rodar(b, pasta, 'teclado', [], 1.5, '0.3:tecla:baixo;0.6:tecla:enter', BOTOES))
    a2 = acionados(rodar(b, pasta, 'teclado2', [], 1.5, '0.3:tecla:tab;0.5:tecla:tab;0.8:tecla:enter', BOTOES))
    return a1 == ['B'] and a2 == ['C'], f'baixo+Enter: {a1} (esperado B); Tab+Tab+Enter: {a2} (esperado C)'


def teste_controle(b, pasta):
    a = acionados(rodar(b, pasta, 'controle', [], 1.5, '0.3:tecla:pad_baixo;0.6:tecla:pad_a', BOTOES))
    return a == ['B'], f'direcional+A: {a} (esperado B)'


def teste_deslizar(b, pasta):
    lua = '''
local v = 0.2
function atualizar(e, dt)
  local novo = hud.deslizar("Volume", 0.1, 0.4, 0.4, 0.1, v, 0, 1)
  if math.abs(novo - v) > 1e-6 then v = novo; log("VALOR", string.format("%.4f", v)) end
end
'''
    linhas = rodar(b, pasta, 'deslizar', [], 1.5, '0.3:clique:0.4,0.45;0.8:tecla:direita', lua)
    vals = [float(l.split()[-1]) for l in linhas if l.startswith('VALOR')]
    ok = len(vals) == 2 and abs(vals[0] - 0.75) < 1e-3 and abs(vals[1] - 0.80) < 1e-3
    return ok, f'valores: {vals} (esperado 0,75 e 0,80)'


def teste_alternar(b, pasta):
    lua = '''
local v = false
function atualizar(e, dt)
  local novo = hud.alternar("Motion blur", 0.1, 0.4, 0.4, 0.1, v)
  if novo ~= v then v = novo; log("VALOR", tostring(v)) end
end
'''
    linhas = rodar(b, pasta, 'alternar', [], 1.5, '0.3:clique:0.3,0.45;0.8:tecla:enter', lua)
    vals = [l.split()[-1] for l in linhas if l.startswith('VALOR')]
    return vals == ['true', 'false'], f'valores: {vals} (esperado true, false)'


BOLA = {'nome': 'bola', 'forma': 'esfera', 'raio': 0.25, 'posicao': [0, 30, 0], 'fisica': {'tipo': 'dinamico'}}
CHAO = {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [20, 0.5, 20], 'posicao': [0, -0.5, 0],
        'fisica': {'tipo': 'estatico'}}


def teste_pausa(b, pasta):
    lua = '''
function atualizar(e, dt)
  local x, y, z = achar("bola"):posicao()
  log("Q", string.format("%.4f %.5f %.5f %s", y, tempo(), tempo_real(), tostring(pausado())))
end
'''
    linhas = rodar(b, pasta, 'pausa', [CHAO, dict(BOLA)], 2.5, '0.5:tecla:esc;1.5:tecla:enter', lua, menu=True)
    q = [l.split()[1:] for l in linhas if l.startswith('Q ')]
    q = [(float(y), float(t), float(tr), p == 'true') for y, t, tr, p in q]
    pausados = [x for x in q if x[3]]
    if not pausados:
        return False, 'o jogo não pausou'
    ys = {round(x[0], 4) for x in pausados}
    ts = {round(x[1], 5) for x in pausados}
    real = pausados[-1][2] - pausados[0][2]
    depois = [x for x in q if not x[3] and x[2] > pausados[-1][2]]
    caiu_depois = depois and depois[-1][0] < pausados[-1][0] - 0.5
    ok = len(ys) == 1 and len(ts) == 1 and real > 0.8 and caiu_depois
    return ok, (f'{len(pausados)} quadros pausados: bola parada em y = {pausados[0][0]:.3f} ({len(ys)} valor), tempo de '
                f'jogo congelado ({len(ts)} valor), tempo real andou {real:.2f} s; depois de Continuar a bola '
                f'{"voltou a cair" if caiu_depois else "NÃO voltou a cair"}')


def teste_salvar(b, pasta):
    lua = '''
local feito_s, feito_c = false, false
function atualizar(e, dt)
  local bola = achar("bola")
  if not feito_s and tempo_real() >= 0.5 then
    feito_s = true
    local x, y, z = bola:posicao()
    local vx, vy, vz = bola:velocidade()
    salvar_jogo("teste", {pontos = 42, nome = "ação", lista = {1, 2, 3}, sub = {ok = true, x = 1.5}})
    log("SALVO", string.format("%.6f %.6f", y, vy))
  elseif feito_s and not feito_c and tempo_real() >= 1.5 then
    feito_c = true
    local x0, y0 = bola:posicao()
    local d = carregar_jogo("teste")
    local x, y, z = bola:posicao()
    local vx, vy, vz = bola:velocidade()
    log("ANTES", string.format("%.6f", y0))
    log("CARREGADO", string.format("%.6f %.6f", y, vy))
    log("DADOS", d.pontos, d.nome, #d.lista, d.lista[3], tostring(d.sub.ok), d.sub.x)
  end
end
'''
    linhas = rodar(b, pasta, 'salvar', [CHAO, dict(BOLA)], 2.0, '', lua)
    pega = lambda k: next((l.split()[1:] for l in linhas if l.startswith(k + ' ')), None)
    s, c, antes, d = pega('SALVO'), pega('CARREGADO'), pega('ANTES'), pega('DADOS')
    if not (s and c and d and antes):
        return False, f'faltou registro: {linhas[-5:]}'
    ys, vs, yc, vc, ya = float(s[0]), float(s[1]), float(c[0]), float(c[1]), float(antes[0])
    arquivo = os.path.isfile(os.path.join(pasta, 'saves', 'teste.json'))
    ok = abs(yc - ys) < 1e-5 and abs(vc - vs) < 1e-4 and d == ['42', 'ação', '3', '3', 'true', '1.5'] and arquivo
    return ok, (f'salvo y = {ys:.3f} (vy {vs:.2f}); 1 s depois caiu até {ya:.3f}; carregado y = {yc:.3f} (vy {vc:.2f}); '
                f'dados {d}')


def teste_analogico(b, pasta):
    jog = {'nome': 'heroi', 'posicao': [0, 0, 0], 'personagem': {'controle': 'jogador', 'velocidade': 4.5}}
    lua = '''
function atualizar(e, dt)
  local x, y, z = achar("heroi"):posicao()
  log("P", string.format("%.4f %.4f", x, z))
end
'''
    pos = lambda ls: [tuple(map(float, l.split()[1:])) for l in ls if l.startswith('P ')]
    a = pos(rodar(b, pasta, 'analogico', [CHAO, jog], 2.0, '0:eixo:esquerdo_y:-1', lua))
    w = pos(rodar(b, pasta, 'teclaw', [CHAO, jog], 2.0, '0:segurar:w:1', lua))
    # Câmera em z = 10 olhando para -z: "frente" = -z.
    da, dw = a[-1][1] - a[0][1], w[-1][1] - w[0][1]
    ok = da < -7.0 and abs(a[-1][0]) < 0.05 and -5.0 < dw < -4.0
    return ok, f'analógico 2 s: andou {-da:.2f} m para a frente (esperado ~9); W por 1 s: {-dw:.2f} m (esperado ~4,5)'


def imagem(binario, pasta, nome, entidades, quadros, lua):
    """Renderiza a cena (480x270, exposição fixa) até o quadro dado e devolve a luminância por pixel."""
    import numpy as np
    from PIL import Image
    caminho = os.path.join(pasta, f'{nome}.lua')
    with open(caminho, 'w', encoding='utf-8') as f:
        f.write(lua)
    cena = {'camera': {'posicao': [0, 2.5, 8], 'alvo': [0, 1, -1]}, 'menu': False,
            'sol': {'elevacao': -12, 'azimute': 120, 'iluminancia': 20000},
            'entidades': entidades + [{'nome': 'ctl', 'forma': 'esfera', 'raio': 0.01, 'posicao': [0, -5, 0],
                                       'script': caminho}]}
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(cena, f)
    out = os.path.join(pasta, f'{nome}_{quadros}.png')
    subprocess.run([binario, '--headless', '--scene', arq, '--width', '480', '--height', '270', '--frames', str(quadros),
                    '--exposure', '3', '--no-motion-blur', '--out', out], check=True, capture_output=True, cwd=pasta)
    a = np.asarray(Image.open(out).convert('RGB')).astype(np.float32) / 255.0
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def teste_particulas(b, pasta):
    import numpy as np
    chao = {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [20, 0.5, 20], 'posicao': [0, -0.5, 0],
            'fisica': {'tipo': 'estatico'}}
    tocha = {'nome': 'tocha', 'forma': 'caixa', 'meia_extensao': [0.1, 0.3, 0.1], 'posicao': [-2, 0.3, 0],
             'fisica': {'tipo': 'cinematico'}, 'particulas': {'tipo': 'fogo', 'desvio': [0, 0.3, 0], 'escala': 0.6,
                                                              'raio': 0.15}}
    # 1) O fogo acompanha a entidade: a tocha anda de x = -2 até x = +1,6; o centro do brilho vai junto.
    anda = 'function atualizar(e, dt) achar("tocha"):mover_para(-2 + 2 * tempo(), 0.3, 0) end'
    def centro(L):  # centro do brilho (peso L^4: o fogo domina)
        w = L.astype(np.float64) ** 4
        return float((w.sum(axis=0) * np.arange(L.shape[1])).sum() / max(w.sum(), 1e-12) / L.shape[1])
    c0 = centro(imagem(b, pasta, 'anda', [chao, dict(tocha)], 6, anda))
    c1 = centro(imagem(b, pasta, 'anda', [chao, dict(tocha)], 54, anda))
    # 2) Desligar pelo script apaga o fogo (e a luz dele).
    apaga = 'function atualizar(e, dt) if tempo() > 0.5 then achar("tocha"):particulas(false) end end'
    aceso = imagem(b, pasta, 'aceso', [chao, dict(tocha)], 12, 'function atualizar(e, dt) end')
    apagado = imagem(b, pasta, 'apagado', [chao, dict(tocha)], 75, apaga)  # 2 s depois de apagar
    # 3) Explosão de um tiro em t = 1 s: antes, nada; logo depois, clarão e bola de fogo.
    boom = 'function atualizar(e, dt) if tempo() >= 1.0 and not f then f = true; explosao(1.5, 0.5, -3) end end'
    antes = imagem(b, pasta, 'boom', [chao], 27, boom)
    depois = imagem(b, pasta, 'boom', [chao], 33, boom)
    regiao = (slice(60, 200), slice(240, 420))  # em volta do ponto da explosão na tela
    r_fogo = (slice(90, 220), slice(150, 330))
    ok = (c1 - c0 > 0.1 and aceso[r_fogo].mean() > 1.5 * apagado[r_fogo].mean()
          and depois[regiao].mean() > 1.5 * antes[regiao].mean())
    return ok, (f'fogo preso à tocha: brilho em x = {c0:.2f} → {c1:.2f} da tela; desligado: brilho na região '
                f'{aceso[r_fogo].mean():.3f} → {apagado[r_fogo].mean():.3f}; explosão: {antes[regiao].mean():.3f} '
                f'antes → {depois[regiao].mean():.3f} 0,1 s depois')


TESTES = {'botao': teste_botao, 'teclado': teste_teclado, 'controle': teste_controle, 'deslizar': teste_deslizar,
          'alternar': teste_alternar, 'pausa': teste_pausa, 'salvar': teste_salvar, 'analogico': teste_analogico, 'particulas': teste_particulas}


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--so')
    a = ap.parse_args()
    binario = os.path.abspath(a.bin)
    oks = []
    with tempfile.TemporaryDirectory() as pasta:
        for n in (a.so.split(',') if a.so else TESTES):
            try:
                ok, info = TESTES[n](binario, pasta)
            except Exception as e:  # noqa: BLE001
                ok, info = False, f'erro: {e}'
            print(f'{n:10s} {"PASSOU" if ok else "FALHOU"}: {info}', flush=True)
            oks.append(ok)
    sys.exit(0 if all(oks) else 1)


if __name__ == '__main__':
    main()
