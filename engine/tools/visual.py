#!/usr/bin/env python3
"""Testes do script visual (grafo de nós em JSON compilado para Lua). Uso (de engine/):

    python3 tools/visual.py [--bin build/legacy] [--so eventos,...]

Cena: uma caixa com um script visual e o jogador longe; um diretor em Lua leva o jogador para perto da caixa em 2 s.
  - eventos:  "ao_iniciar" roda uma vez; "repetir" 3 vezes com o índice; "a_cada 0,5 s" soma 1 numa variável e o "se"
              escreve quando ela chega a 3 (em ~1,5 s); "quando" dispara uma vez só quando ela passa de 5.
  - perto:    "ao_chegar_perto" do jogador (2 m) dispara em ~2 s; "mover" relativo +2 m em 1 s (latente: o jogo segue)
              e "esperar" 0,5 s; depois a caixa está 2 m acima.
  - recarga:  com o jogo rodando, o .json muda (texto novo): o Lua é gerado de novo e o texto novo aparece.
  - erro:     um grafo com ciclo (sem "repetir") é recusado com mensagem, sem derrubar o motor.
  - editor:   o editor de grafos (janela) abre o .vs.json, liga um nó novo e salva; o script editado roda.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

FPS = 30

GRAFO = {
    'nos': [
        {'id': 1, 'tipo': 'ao_iniciar'},
        {'id': 2, 'tipo': 'log', 'texto': 'inicio'},
        {'id': 3, 'tipo': 'definir', 'variavel': 'moedas', 'valor': 0},
        {'id': 4, 'tipo': 'repetir', 'vezes': 3},
        {'id': 5, 'tipo': 'log', 'texto': 'volta {$indice}'},
        {'id': 10, 'tipo': 'a_cada', 'segundos': 0.5},
        {'id': 11, 'tipo': 'somar', 'variavel': 'moedas', 'valor': 1},
        {'id': 12, 'tipo': 'se', 'a': '$moedas', 'op': '==', 'b': 3},
        {'id': 13, 'tipo': 'log', 'texto': 'tres t={#tempo}'},
        {'id': 20, 'tipo': 'quando', 'a': '$moedas', 'op': '>=', 'b': 5},
        {'id': 21, 'tipo': 'log', 'texto': 'cinco'},
        {'id': 30, 'tipo': 'ao_chegar_perto', 'alvo': 'jogador', 'distancia': 2},
        {'id': 31, 'tipo': 'log', 'texto': 'perto t={#tempo}'},
        {'id': 32, 'tipo': 'mover', 'relativo': True, 'y': 2, 'duracao': 1},
        {'id': 33, 'tipo': 'esperar', 'segundos': 0.5},
        {'id': 34, 'tipo': 'log', 'texto': 'subiu y={@eu.y} t={#tempo}'},
        {'id': 40, 'tipo': 'a_cada', 'segundos': 1},
        {'id': 41, 'tipo': 'log', 'texto': 'versao A'},
    ],
    'ligacoes': [[1, 2], [2, 3], [3, 4], [4, 5, 'corpo'], [10, 11], [11, 12], [12, 13, 'sim'], [20, 21],
                 [30, 31], [31, 32], [32, 33], [33, 34], [40, 41]],
}

DIRETOR = '''
function atualizar(e, dt)
  if tempo() >= 2.0 and not feito then feito = true; achar("jogador"):mover_para(1.0, 0.9, 0) end
end
'''


def cena(pasta, grafo, nome='visual'):
    vs = os.path.join(pasta, f'{nome}.vs.json')
    with open(vs, 'w', encoding='utf-8') as f:
        json.dump(grafo, f)
    lua = os.path.join(pasta, f'{nome}_diretor.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write(DIRETOR)
    c = {'menu': False, 'entidades': [
        {'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [20, 0.5, 20], 'posicao': [0, -0.5, 0],
         'fisica': {'tipo': 'estatico'}},
        {'nome': 'caixa', 'forma': 'caixa', 'meia_extensao': [0.5, 0.5, 0.5], 'posicao': [0, 0.5, 0], 'script_visual': vs},
        {'nome': 'jogador', 'posicao': [12, 0, 0], 'personagem': {'controle': 'jogador'}},
        {'nome': 'diretor', 'script': lua, 'sempre_carregado': True}]}
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(c, f)
    return arq, vs


def comando(b, arq, segundos, extra=()):
    return [b, '--headless', '--scene', arq, '--width', '32', '--height', '18', '--frames', str(int(segundos * FPS)),
            '--fps', str(FPS)] + list(extra)


def mensagens(texto):
    return [l.split('[script] VS ', 1)[1].strip() for l in texto.splitlines() if '[script] VS ' in l]


_cache = {}


def sessao(b, pasta):
    if 'log' not in _cache:
        arq, _ = cena(pasta, GRAFO)
        p = subprocess.run(comando(b, arq, 5.0), capture_output=True, text=True, encoding='utf-8', errors='replace')
        if p.returncode:
            raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
        _cache['log'] = p.stdout + p.stderr
    return _cache['log']


def valor(m, chave):
    for parte in m.split():
        if parte.startswith(chave + '='):
            return float(parte.split('=', 1)[1])
    return None


def teste_eventos(b, pasta):
    ms = mensagens(sessao(b, pasta))
    inicio = ms.count('inicio')
    voltas = [m for m in ms if m.startswith('volta')]
    tres = [m for m in ms if m.startswith('tres')]
    cinco = ms.count('cinco')
    t3 = valor(tres[0], 't') if tres else None
    ok = inicio == 1 and voltas == ['volta 1', 'volta 2', 'volta 3'] and len(tres) == 1 and t3 is not None and \
        abs(t3 - 1.5) < 0.1 and cinco == 1
    return ok, (f'inicio {inicio}x; {voltas}; "tres" {len(tres)}x em t = {t3}; "cinco" {cinco}x '
                f'(esperado: 1x, voltas 1-3, 1x em 1,5 s, 1x)')


def teste_perto(b, pasta):
    ms = mensagens(sessao(b, pasta))
    perto = [m for m in ms if m.startswith('perto')]
    subiu = [m for m in ms if m.startswith('subiu')]
    tp = valor(perto[0], 't') if perto else None
    y, ts = (valor(subiu[0], 'y'), valor(subiu[0], 't')) if subiu else (None, None)
    ok = len(perto) == 1 and tp is not None and abs(tp - 2.0) < 0.1 and y is not None and abs(y - 2.5) < 0.01 and \
        ts is not None and abs(ts - 3.5) < 0.1
    return ok, f'perto em t = {tp} (2,0); subiu para y = {y} (2,5) em t = {ts} (3,5: 1 s movendo + 0,5 s esperando)'


def teste_recarga(b, pasta):
    arq, vs = cena(pasta, GRAFO, 'recarga')
    p = subprocess.Popen(comando(b, arq, 6.0, ['--tempo-real']), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding='utf-8', errors='replace')
    linhas = []
    t = threading.Thread(target=lambda: linhas.extend(p.stdout))
    t.start()
    while not any('versao A' in l for l in linhas) and p.poll() is None:
        time.sleep(0.1)
    novo = json.loads(json.dumps(GRAFO))
    for n in novo['nos']:
        if n['id'] == 41:
            n['texto'] = 'versao B'
    with open(vs, 'w', encoding='utf-8') as f:
        json.dump(novo, f)
    p.wait(timeout=120)
    t.join()
    ms = mensagens(''.join(linhas))
    a, bb = ms.count('versao A'), ms.count('versao B')
    gerou = any('Lua gerado de novo' in l for l in linhas)
    return a >= 1 and bb >= 1 and gerou, f'"versao A" {a}x, depois de editar o .json "versao B" {bb}x; recompilou: {gerou}'


def teste_erro(b, pasta):
    ruim = {'nos': [{'id': 1, 'tipo': 'ao_iniciar'}, {'id': 2, 'tipo': 'log', 'texto': 'a'},
                    {'id': 3, 'tipo': 'log', 'texto': 'b'}], 'ligacoes': [[1, 2], [2, 3], [3, 2]]}
    arq, _ = cena(pasta, ruim, 'erro')
    p = subprocess.run(comando(b, arq, 1.0), capture_output=True, text=True, encoding='utf-8', errors='replace')
    log = p.stdout + p.stderr
    msg = [l for l in log.splitlines() if 'Script visual' in l]
    ok = p.returncode == 0 and any('ciclo' in l for l in msg)
    return ok, f'código {p.returncode}; mensagem: {msg[0].split("] ", 1)[-1] if msg else "nenhuma"}'


def teste_editor(b, pasta):
    """Editor de grafos (janela; no Linux sem tela, xvfb-run): abre o .vs.json, um nó "log" novo é ligado ao primeiro
    evento e salvo; o grafo salvo continua compilando e o nó novo roda."""
    import shutil
    if sys.platform.startswith('linux') and not os.environ.get('DISPLAY') and not shutil.which('xvfb-run'):
        return True, 'sem tela nem xvfb-run: pulado'
    arq, vs = cena(pasta, GRAFO, 'editor')
    cmd = [b, '--editor', '--scene', arq, '--grafo', vs, '--width', '640', '--height', '360', '--quit-after', '5']
    if sys.platform.startswith('linux') and not os.environ.get('DISPLAY'):
        cmd = ['xvfb-run', '-a', '-s', '-screen 0 1280x720x24'] + cmd
    subprocess.run(cmd, capture_output=True, timeout=600, env=dict(os.environ, LEGACY_EDITOR_TESTE='grafo'), cwd=pasta)
    with open(vs, encoding='utf-8') as f:
        g = json.load(f)
    novo = [n for n in g['nos'] if n['tipo'] == 'log' and n.get('texto') == 'oi']
    ligado = bool(novo) and any(l[:2] == [1, novo[0]['id']] for l in g['ligacoes'] if isinstance(l, list))
    posicoes = all('pos_editor' in n for n in g['nos'])
    p = subprocess.run(comando(b, arq, 0.5), capture_output=True, text=True, encoding='utf-8', errors='replace')
    rodou = 'oi' in mensagens(p.stdout + p.stderr)
    return bool(novo) and ligado and posicoes and rodou, (f'nó novo: {bool(novo)}, ligado ao evento 1: {ligado}, posições '
                                                          f'gravadas: {posicoes}; o script editado roda o nó novo: {rodou}')


TESTES = {'eventos': teste_eventos, 'perto': teste_perto, 'recarga': teste_recarga, 'erro': teste_erro, 'editor': teste_editor}


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
