#!/usr/bin/env python3
"""Testes dos materiais por nós (grafo assado em texturas na carga da cena). Uso (de engine/):

    python3 tools/materiais.py [--bin build/legacy] [--so tijolos,...]

  - tijolos:  parede de tijolos (nó "tijolos" + ruído + rampa de cor): 8 fileiras na textura; a argamassa ocupa a
              fração esperada; a cor do tijolo e da argamassa batem com a rampa.
  - emenda:   a textura repete sem emenda: a diferença entre a última e a primeira coluna (e linha) é do tamanho da
              diferença entre vizinhas no meio da imagem.
  - metal:    metal enferrujado (Voronoi + ruído): onde há ferrugem o metal vai a 0 e a rugosidade sobe; o normal map
              sai do relevo (inclinado onde a altura muda).
  - imagem:   a cena renderizada mostra o padrão (a face com tijolos varia bem mais que a mesma caixa lisa).
  - erro:     um grafo com nó desconhecido é recusado com mensagem, sem derrubar o motor.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

TIJOLOS = {
    'resolucao': 256,
    'nos': [
        {'id': 1, 'tipo': 'tijolos', 'linhas': 8, 'colunas': 4, 'argamassa': 0.08},
        {'id': 2, 'tipo': 'ruido', 'escala': 16, 'oitavas': 4, 'semente': 3},
        {'id': 3, 'tipo': 'rampa', 'entrada': {'no': 2}, 'cores': [[0, '#8a3324'], [1, '#b5523a']]},
        {'id': 4, 'tipo': 'misturar', 'a': '#bdb6a8', 'b': {'no': 3}, 't': {'no': 1}},
        {'id': 5, 'tipo': 'misturar', 'a': 0.95, 'b': 0.75, 't': {'no': 1}},
    ],
    'saida': {'cor': {'no': 4}, 'rugosidade': {'no': 5}, 'altura': {'no': 1}, 'forca_relevo': 0.01},
}
METAL = {
    'resolucao': 256,
    'nos': [
        {'id': 1, 'tipo': 'voronoi', 'escala': 6, 'semente': 9},
        {'id': 2, 'tipo': 'ruido', 'escala': 8, 'oitavas': 5, 'semente': 4},
        {'id': 3, 'tipo': 'misturar', 'a': {'no': 1}, 'b': {'no': 2}, 't': 0.5},
        {'id': 4, 'tipo': 'contraste', 'entrada': {'no': 3}, 'min': 0.45, 'max': 0.55},  # 1 = ferrugem
        {'id': 5, 'tipo': 'misturar', 'a': '#a8aaad', 'b': '#7a3d1c', 't': {'no': 4}},
        {'id': 6, 'tipo': 'inverter', 'entrada': {'no': 4}},
        {'id': 7, 'tipo': 'misturar', 'a': 0.25, 'b': 0.9, 't': {'no': 4}},
    ],
    'saida': {'cor': {'no': 5}, 'metalico': {'no': 6}, 'rugosidade': {'no': 7}, 'altura': {'no': 4}},
}


def rodar(b, pasta, materiais, nome='m', tamanho=(320, 180), quadros=8):
    dump = os.path.join(pasta, f'dump_{nome}')
    os.makedirs(dump, exist_ok=True)
    c = {'menu': False, 'camera': {'posicao': [0, 1.2, 3.2], 'alvo': [0, 1.0, 0]}, 'sol': {'elevacao': 35, 'azimute': 160},
         'materiais': materiais,
         'entidades': [{'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [10, 0.1, 10], 'posicao': [0, -0.1, 0]},
                       {'nome': 'a', 'forma': 'caixa', 'meia_extensao': [0.6, 0.6, 0.6], 'posicao': [-0.75, 0.6, 0],
                        'material': 'a'},
                       {'nome': 'b', 'forma': 'caixa', 'meia_extensao': [0.6, 0.6, 0.6], 'posicao': [0.75, 0.6, 0],
                        'material': 'b'}]}
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(c, f)
    out = os.path.join(pasta, f'{nome}.png')
    env = dict(os.environ, LEGACY_DUMP_MATERIAIS=dump)
    p = subprocess.run([b, '--headless', '--scene', arq, '--width', str(tamanho[0]), '--height', str(tamanho[1]),
                        '--frames', str(quadros), '--no-motion-blur', '--out', out], capture_output=True, text=True,
                       encoding='utf-8', errors='replace', env=env)
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    return dump, out, p.stdout + p.stderr


_cache = {}


def assado(b, pasta):
    if 'm' not in _cache:
        _cache['m'] = rodar(b, pasta, {'a': TIJOLOS, 'b': METAL})
    return _cache['m']


def img(p):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(p).convert('RGB')).astype(float)


def teste_tijolos(b, pasta):
    import numpy as np
    dump, _, _ = assado(b, pasta)
    c = img(os.path.join(dump, 'a_cor.png'))
    lum = c.mean(2)
    argamassa = lum > 160  # argamassa clara (#bdb6a8) x tijolo escuro
    frac = argamassa.mean()
    # Fileiras: linhas horizontais de argamassa (quase toda a linha clara).
    linhas = (argamassa.mean(1) > 0.8).astype(int)
    fileiras = int(np.sum(np.diff(np.concatenate([[linhas[-1]], linhas])) == 1))
    tijolo = c[~argamassa].mean(0)
    ok = fileiras == 8 and 0.12 < frac < 0.35 and tijolo[0] > 1.6 * tijolo[2]
    return ok, (f'{fileiras} fileiras (8); argamassa em {100 * frac:.0f}% da textura; cor média do tijolo '
                f'{tuple(int(v) for v in tijolo)} (vermelho)')


def teste_emenda(b, pasta):
    import numpy as np
    dump, _, _ = assado(b, pasta)
    piores = []
    for nome in ('a_cor', 'b_cor', 'b_rugmetal'):
        c = img(os.path.join(dump, nome + '.png'))
        meio = np.abs(np.diff(c, axis=1)).mean()
        borda_x = np.abs(c[:, 0] - c[:, -1]).mean()
        borda_y = np.abs(c[0] - c[-1]).mean()
        piores.append((nome, meio, borda_x, borda_y))
    ok = all(bx < 2.0 * m + 1 and by < 2.0 * m + 1 for _, m, bx, by in piores)
    return ok, '; '.join(f'{n}: vizinhas {m:.1f}, emenda x {bx:.1f}, y {by:.1f}' for n, m, bx, by in piores)


def teste_metal(b, pasta):
    import numpy as np
    dump, _, _ = assado(b, pasta)
    c = img(os.path.join(dump, 'b_cor.png'))
    rm = img(os.path.join(dump, 'b_rugmetal.png'))
    nm = img(os.path.join(dump, 'b_normal.png'))
    ferrugem = c[..., 0] > c[..., 2] * 1.5
    metal_f, metal_l = rm[ferrugem, 2].mean(), rm[~ferrugem, 2].mean()
    rug_f, rug_l = rm[ferrugem, 1].mean(), rm[~ferrugem, 1].mean()
    inclinado = (np.abs(nm[..., 0] - 128) + np.abs(nm[..., 1] - 128)) > 20
    ok = 0.1 < ferrugem.mean() < 0.9 and metal_f < 40 and metal_l > 200 and rug_f > rug_l + 100 and 0.01 < inclinado.mean() < 0.5
    return ok, (f'ferrugem em {100 * ferrugem.mean():.0f}%; metal {metal_f:.0f} (ferrugem) x {metal_l:.0f}; rugosidade '
                f'{rug_f:.0f} x {rug_l:.0f}; normal inclinado em {100 * inclinado.mean():.1f}% (bordas da ferrugem)')


def teste_imagem(b, pasta):
    _, png, _ = assado(b, pasta)
    liso = {'resolucao': 64, 'nos': [], 'saida': {'cor': '#9a4a35', 'rugosidade': 0.85}}
    _, png_liso, _ = rodar(b, pasta, {'a': liso, 'b': liso}, 'liso')
    a, l = img(png), img(png_liso)
    h, w, _ = a.shape
    reg = (slice(int(h * 0.5), int(h * 0.8)), slice(int(w * 0.27), int(w * 0.46)))  # face da caixa da esquerda
    va, vl = a[reg].mean(2).std(), l[reg].mean(2).std()
    return va > 3 * vl and va > 8, f'variação na face da caixa: {va:.1f} com tijolos, {vl:.1f} lisa'


def teste_erro(b, pasta):
    ruim = {'nos': [{'id': 1, 'tipo': 'nao_existe'}], 'saida': {'cor': {'no': 1}}}
    _, _, log = rodar(b, pasta, {'a': ruim, 'b': METAL}, 'erro', tamanho=(32, 18), quadros=1)
    msg = [l.split('] ', 1)[-1] for l in log.splitlines() if 'Material a:' in l]
    ok = any('desconhecido' in m for m in msg)
    return ok, f'mensagem: {msg[0] if msg else "nenhuma"}'


def teste_arquivo(b, pasta):
    """O grafo num arquivo ("grafo": "x.mat.json", o que o editor de grafos grava) assa igual ao grafo na cena."""
    import numpy as np
    arq = os.path.join(pasta, 'tijolo.mat.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(TIJOLOS, f)
    dump, _, _ = rodar(b, pasta, {'a': {'grafo': arq}, 'b': METAL}, 'arquivo', tamanho=(32, 18), quadros=1)
    ref, _, _ = assado(b, pasta)
    d = np.abs(img(os.path.join(dump, 'a_cor.png')) - img(os.path.join(ref, 'a_cor.png'))).max()
    return d == 0, f'maior diferença entre o assado do arquivo e o da cena: {d:.0f}/255'


TESTES = {'tijolos': teste_tijolos, 'emenda': teste_emenda, 'metal': teste_metal, 'imagem': teste_imagem,
          'erro': teste_erro, 'arquivo': teste_arquivo}


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
