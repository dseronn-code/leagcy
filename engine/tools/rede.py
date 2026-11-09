#!/usr/bin/env python3
"""Teste do multijogador: anfitrião e cliente na mesma máquina (localhost). Uso (de engine/):

    python3 tools/rede.py [--bin build/legacy] [--porta 7790]

Cena com chão, uma bola caindo e o jogador "heroi" ("multijogador": 2 vagas). Os dois motores rodam sem janela:
  - o anfitrião (--host) simula tudo;
  - o cliente (--conectar) ocupa a vaga 1 ("heroi_2", a raposa animada, nasce em x = 3) e segura W por 1,5 s.
Um script nos dois lados registra as posições a cada quadro. Confere:
  - entrada pela rede: no anfitrião, o boneco do cliente andou para a frente (> 2 m) e parou;
  - estado replicado: no fim (tudo parado), o cliente mostra o boneco e a bola na mesma posição que o anfitrião
    (< 2 cm), e a bola caiu até o chão nos dois;
  - animação replicada: no cliente (sem física), a pata da raposa se mexe em relação à bacia enquanto ela anda;
  - previsão: com 150 ms de atraso simulado em cada sentido, o boneco do cliente reage na hora (com previsão) e não
    só depois da volta pela rede (sem previsão); no fim, os dois lados concordam;
  - rodas: um carro dirigido no anfitrião aparece igual no cliente (imagem final, chassi e rodas).
  - ragdoll: uma raposa cai em ragdoll no anfitrião (1 s depois de o cliente entrar) e levanta 4 s depois: no
    cliente ela cai junto (bacia perto do chão), deitada no mesmo lugar (< 2 cm) e de pé de novo no fim.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
import threading
import time

LUA = '''
function atualizar(e, dt)
  local h = achar("heroi_2")
  local b = achar("bola")
  if h and b then
    local hx, hy, hz = h:posicao()
    local bx, by, bz = b:posicao()
    log("POS", string.format("%.4f %.4f %.4f %.4f %.4f %.4f %.4f", hx, hy, hz, bx, by, bz, tempo_real()))
    local px, py, pz = h:osso("b_LeftFoot02_018")
    local qx, qy, qz = h:osso("b_Hip_01")
    if px then log("OSSO", string.format("%.4f %.4f %.4f", px - qx, py - qy, pz - qz)) end
  end
end
'''


def sessao(binario, pasta, arq, porta, simular, env_cliente=None):
    """Roda anfitrião e cliente; devolve (linhas do anfitrião, linhas do cliente)."""
    base = [binario, '--headless', '--scene', arq, '--width', '32', '--height', '18']
    anfitriao = subprocess.Popen(base + ['--host', str(porta), '--frames', '900', '--out', os.path.join(pasta, 'h.png')],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                 errors='replace')
    linhas_h = []
    pronto = threading.Event()

    def ler():
        for l in anfitriao.stdout:
            linhas_h.append(l)
            if 'anfitrião na porta' in l:
                pronto.set()
    t = threading.Thread(target=ler)
    t.start()
    if not pronto.wait(300):
        anfitriao.kill()
        raise RuntimeError('o anfitrião não abriu a porta')
    time.sleep(0.5)
    env = dict(os.environ, **(env_cliente or {}))
    cli = subprocess.run(base + ['--conectar', f'127.0.0.1:{porta}', '--frames', '900', '--simular', simular, '--out',
                                 os.path.join(pasta, 'c.png')], capture_output=True, text=True, encoding='utf-8',
                         errors='replace', env=env)
    anfitriao.wait(timeout=900)
    t.join()
    return linhas_h, (cli.stdout + cli.stderr).splitlines()


def posicoes(texto):
    return [tuple(map(float, l.split('POS', 1)[1].split())) for l in texto if 'POS ' in l]


def reacao(pc, aperto):
    """Segundos (tempo do cliente) entre apertar W e o boneco andar 1 cm na tela do cliente."""
    antes = [p for p in pc if p[6] <= aperto and p[1] > -100]
    if not antes:
        return float('nan')
    x0 = antes[-1][:3]
    for p in pc:
        if p[6] > aperto and math.dist(p[:3], x0) > 0.01:
            return p[6] - aperto
    return float('nan')


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--porta', type=int, default=7790)
    a = ap.parse_args()
    binario = os.path.abspath(a.bin)
    oks = []
    with tempfile.TemporaryDirectory() as pasta:
        lua = os.path.join(pasta, 'pos.lua')
        with open(lua, 'w', encoding='utf-8') as f:
            f.write(LUA)
        cena = {'camera': {'posicao': [0, 3, 12], 'alvo': [0, 1, 0]}, 'menu': False,
                'multijogador': {'vagas': 2, 'nascer': [[3, 0, 0], [-3, 0, 0]]},
                'entidades': [
                    {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [30, 0.5, 30], 'posicao': [0, -0.5, 0],
                     'fisica': {'tipo': 'estatico'}},
                    {'nome': 'bola', 'forma': 'esfera', 'raio': 0.25, 'posicao': [0, 8, -2],
                     'fisica': {'tipo': 'dinamico'}},
                    {'nome': 'heroi', 'posicao': [0, 0, 0], 'personagem': {
                        'controle': 'jogador', 'altura': 0.8, 'raio': 0.2, 'velocidade': 1.5,
                        'visual': {'modelo': os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                          'assets', 'Fox', 'Fox.gltf'),
                                   'escala': 0.01, 'parado': 'Survey', 'andar': 'Walk', 'correr': 'Run',
                                   'velocidade_andar': 1.5, 'velocidade_correr': 2.7}}},
                    {'nome': 'log', 'forma': 'esfera', 'raio': 0.01, 'posicao': [0, -20, 0], 'script': lua}]}
        arq = os.path.join(pasta, 'rede.json')
        with open(arq, 'w', encoding='utf-8') as f:
            json.dump(cena, f)

        # 1) sem atraso: estado igual nos dois lados e animação no cliente
        lh, lc = sessao(binario, pasta, arq, a.porta, '3:segurar:w:1.5')
        ph, pc = posicoes(lh), posicoes(lc)
        conectou = any('conectado na vaga 0' in l for l in lc)
        if not ph or not pc or not conectou:
            print(f'rede       FALHOU: conectou={conectou}, registros anfitrião {len(ph)}, cliente {len(pc)}')
            print('\n'.join(lc[-30:]))
            sys.exit(1)
        h, c = ph[-1], pc[-1]
        ossos = [tuple(map(float, l.split('OSSO', 1)[1].split())) for l in lc if 'OSSO ' in l]
        import statistics
        passos = statistics.pstdev([o[2] for o in ossos]) if len(ossos) > 2 else 0.0
        ativos = [p for p in ph if p[1] > -100]
        andou = math.dist(ativos[0][:3], h[:3]) if ativos else 0.0
        dif_heroi, dif_bola = math.dist(h[:3], c[:3]), math.dist(h[3:6], c[3:6])
        ok = andou > 1.0 and dif_heroi < 0.02 and dif_bola < 0.02 and abs(h[4] - 0.25) < 0.05 and passos > 0.02
        oks.append(ok)
        print(f'rede       {"PASSOU" if ok else "FALHOU"}: o boneco do cliente andou {andou:.2f} m no anfitrião; no fim, '
              f'cliente e anfitrião: boneco a {100 * dif_heroi:.2f} cm, bola a {100 * dif_bola:.2f} cm (bola no chão, '
              f'y = {h[4]:.3f} m); animação no cliente: a pata oscila {100 * passos:.1f} cm em relação à bacia')

        # 2) atraso de 150 ms em cada sentido: com previsão o boneco reage na hora; sem, só depois da volta
        res = {}
        for nome, env in (('com previsão', {'LEGACY_REDE_ATRASO_MS': '150'}),
                          ('sem previsão', {'LEGACY_REDE_ATRASO_MS': '150', 'LEGACY_REDE_SEM_PREVISAO': '1'})):
            lh, lc = sessao(binario, pasta, arq, a.porta, '3:segurar:w:1.5', env)
            ph, pc = posicoes(lh), posicoes(lc)
            res[nome] = (reacao(pc, 3.0), math.dist(ph[-1][:3], pc[-1][:3]) if ph and pc else float('nan'))
        rp, dp = res['com previsão']
        rs, ds = res['sem previsão']
        ok = rp < 0.1 and rs > 0.25 and dp < 0.05 and ds < 0.05
        oks.append(ok)
        print(f'previsao   {"PASSOU" if ok else "FALHOU"}: com 150 ms de atraso em cada sentido, o boneco reage em '
              f'{1000 * rp:.0f} ms com previsão e {1000 * rs:.0f} ms sem; no fim, cliente e anfitrião a {100 * dp:.1f} cm '
              f'(com) e {100 * ds:.1f} cm (sem)')

        # 3) carro com roteiro no anfitrião: no cliente o chassi E as rodas (posição, giro, esterço) iguais
        carro = {'camera': {'posicao': [6, 2.5, 8], 'alvo': [0, 0.5, 6]}, 'menu': False, 'multijogador': {'vagas': 1},
                 'entidades': [
                     {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [80, 0.5, 80], 'posicao': [0, -0.5, 0],
                      'fisica': {'tipo': 'estatico', 'atrito': 1}},
                     {'nome': 'heroi', 'posicao': [-6, 0, 0], 'personagem': {'controle': 'jogador'}},
                     {'nome': 'carro', 'posicao': [0, 1.2, 0], 'veiculo': {'controle': 'roteiro', 'roteiro': [
                         {'t': 0, 'acelerar': 0.6, 'virar': 0.5}, {'t': 1.5, 'acelerar': 0, 'virar': 0.5, 'freio': 1}]}}]}
        arq2 = os.path.join(pasta, 'carro.json')
        with open(arq2, 'w', encoding='utf-8') as f:
            json.dump(carro, f)
        base = [binario, '--headless', '--scene', arq2, '--width', '320', '--height', '180']
        h = subprocess.Popen(base + ['--host', str(a.porta + 1), '--frames', '1500', '--out', os.path.join(pasta, 'ch.png')],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
        for l in h.stdout:
            if 'anfitrião na porta' in l:
                break
        threading.Thread(target=lambda: [None for _ in h.stdout]).start()
        time.sleep(0.5)
        subprocess.run(base + ['--conectar', f'127.0.0.1:{a.porta + 1}', '--frames', '300', '--out',
                               os.path.join(pasta, 'cc.png')], capture_output=True)
        h.wait(timeout=1800)
        import numpy as np
        from PIL import Image
        im_h = np.asarray(Image.open(os.path.join(pasta, 'ch.png'))).astype(float)
        im_c = np.asarray(Image.open(os.path.join(pasta, 'cc.png'))).astype(float)
        dif = np.abs(im_h - im_c).mean()
        ok = dif < 1.0
        oks.append(ok)
        print(f'rodas      {"PASSOU" if ok else "FALHOU"}: carro dirigido no anfitrião, imagem final do cliente igual à do '
              f'anfitrião (diferença média {dif:.2f}/255, chassi e rodas)')
        # 4) ragdoll no anfitrião, visto no cliente
        lua_rag = os.path.join(pasta, 'rag.lua')
        with open(lua_rag, 'w', encoding='utf-8') as f:
            f.write('''
function atualizar(e, dt)
  local h = achar("heroi_2")
  local _, hy = h:posicao()
  if hy > -100 and not entrou then entrou = tempo() end
  if entrou and not caiu and tempo() > entrou + 1 then caiu = true; e:ragdoll(30, 0, 10) end
  if caiu and not levantou and tempo() > entrou + 5 then levantou = true; e:levantar() end
  local x, y, z = e:osso("b_Hip_01")
  local a, b, c = e:osso("b_Head_05")
  if entrou then log("RAG", string.format("%.4f %.4f %.4f %.4f %.4f %.4f %.4f", tempo(), x, y, z, a, b, c)) end
end
''')
        rag = dict(cena)
        rag['entidades'] = [ent for ent in cena['entidades'] if ent['nome'] != 'bola'] + [
            {'nome': 'raposa', 'posicao': [0, 0.4, 3], 'script': lua_rag, 'personagem': {
                'controle': 'roteiro', 'roteiro': [], 'altura': 0.8, 'raio': 0.15, 'massa': 20,
                'visual': dict(cena['entidades'][2]['personagem']['visual'])}}]
        arq3 = os.path.join(pasta, 'rag.json')
        with open(arq3, 'w', encoding='utf-8') as f:
            json.dump(rag, f)
        lh, lc = sessao(binario, pasta, arq3, a.porta + 2, '0:segurar:w:0.01')
        reg = lambda ls: [tuple(map(float, l.split('RAG', 1)[1].split())) for l in ls if 'RAG ' in l]
        rh, rc = reg(lh), reg(lc)
        ok = bool(rh and rc)
        msg = 'sem registros'
        if ok:
            t0 = rh[0][0]
            deitado_h = [r for r in rh if t0 + 3.5 < r[0] < t0 + 4.9]
            deitado_c = [r for r in rc if t0 + 3.5 < r[0] < t0 + 4.9]
            if not deitado_h or not deitado_c:
                ok, msg = False, f'o cliente não cobriu o tempo da queda ({rc[0][0]:.1f}–{rc[-1][0]:.1f} s; queda em {t0 + 1:.1f} s)'
            else:
                hc = deitado_c[-1]
                hh = min(rh, key=lambda r: abs(r[0] - hc[0]))
                dif = max(math.dist(hh[1:4], hc[1:4]), math.dist(hh[4:7], hc[4:7]))
                baixo = hc[2]
                fim_c = rc[-1][2]
                ok = baixo < 0.15 and dif < 0.02 and fim_c > 0.35
                msg = (f'no cliente a bacia caiu a {baixo:.2f} m; deitada, cliente e anfitrião a {100 * dif:.2f} cm '
                       f'(bacia e cabeça); depois de levantar, bacia a {fim_c:.2f} m no cliente')
        oks.append(ok)
        print(f'ragdoll    {"PASSOU" if ok else "FALHOU"}: {msg}')
    sys.exit(0 if all(oks) else 1)


if __name__ == '__main__':
    main()
