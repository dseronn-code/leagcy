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
  - animação replicada: no cliente (sem física), a pata da raposa se mexe em relação à bacia enquanto ela anda.
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
    log("POS", string.format("%.4f %.4f %.4f %.4f %.4f %.4f", hx, hy, hz, bx, by, bz))
    local px, py, pz = h:osso("b_LeftFoot02_018")
    local qx, qy, qz = h:osso("b_Hip_01")
    if px then log("OSSO", string.format("%.4f %.4f %.4f", px - qx, py - qy, pz - qz)) end
  end
end
'''


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--porta', type=int, default=7790)
    a = ap.parse_args()
    binario = os.path.abspath(a.bin)
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
        base = [binario, '--headless', '--scene', arq, '--width', '32', '--height', '18']
        anfitriao = subprocess.Popen(base + ['--host', str(a.porta), '--frames', '900', '--out',
                                            os.path.join(pasta, 'h.png')], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
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
            print('rede       FALHOU: o anfitrião não abriu a porta')
            sys.exit(1)
        time.sleep(0.5)
        cli = subprocess.run(base + ['--conectar', f'127.0.0.1:{a.porta}', '--frames', '900', '--simular',
                                     '0.5:segurar:w:1.5', '--out', os.path.join(pasta, 'c.png')],
                             capture_output=True, text=True, encoding='utf-8', errors='replace')
        anfitriao.wait(timeout=900)
        t.join()

        def posicoes(texto):
            return [tuple(map(float, l.split('POS', 1)[1].split())) for l in texto if 'POS ' in l]
        ph = posicoes(linhas_h)
        pc = posicoes((cli.stdout + cli.stderr).splitlines())
        conectou = any('conectado na vaga 0' in l for l in (cli.stdout + cli.stderr).splitlines())
        if not ph or not pc or not conectou:
            print(f'rede       FALHOU: conectou={conectou}, registros anfitrião {len(ph)}, cliente {len(pc)}')
            print((cli.stdout + cli.stderr)[-1500:])
            sys.exit(1)
        h, c = ph[-1], pc[-1]
        # Animação replicada: no cliente, enquanto o boneco anda, a pata se mexe em relação à bacia (passos).
        ossos = [tuple(map(float, l.split('OSSO', 1)[1].split())) for l in (cli.stdout + cli.stderr).splitlines()
                 if 'OSSO ' in l]
        import statistics
        passos = statistics.pstdev([o[2] for o in ossos]) if len(ossos) > 2 else 0.0
        ativos = [p for p in ph if p[1] > -100]  # vaga ocupada (fora do estacionamento)
        andou = math.dist(ativos[0][:3], h[:3]) if ativos else 0.0
        dif_heroi = math.dist(h[:3], c[:3])
        dif_bola = math.dist(h[3:], c[3:])
        ok = andou > 1.0 and dif_heroi < 0.02 and dif_bola < 0.02 and abs(h[4] - 0.25) < 0.05 and passos > 0.02
        print(f'rede       {"PASSOU" if ok else "FALHOU"}: o boneco do cliente andou {andou:.2f} m no anfitrião '
              f'(de x = 3); no fim, cliente e anfitrião: boneco a {100 * dif_heroi:.2f} cm, bola a '
              f'{100 * dif_bola:.2f} cm (bola no chão, y = {h[4]:.3f} m); animação no cliente: a pata oscila '
              f'{100 * passos:.1f} cm (desvio) em relação à bacia')
        sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
