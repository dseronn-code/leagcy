#!/usr/bin/env python3
"""Testes da geometria virtualizada (no estilo do Nanite). Uso (de engine/):

    python3 tools/nanite.py [--bin build/legacy] [--so nivel0,grafo,...]

Cena "nanite": rochas procedurais (esfera de 768 segmentos deformada por ruído 3D: ~590 mil triângulos cada).
  - nivel0:     com erro máximo de 0,01 pixel, todo pedaço desenhado é da malha inteira: a imagem é a mesma de
                --no-nanite (os triângulos só mudaram de ordem).
  - grafo:      a hierarquia da rocha tem vários níveis, a raiz é pequena (≤ 1% dos triângulos) e o total de pedaços
                fica abaixo de 2,5 vezes o do nível 0 (memória).
  - qualidade:  com 1 pixel (o padrão), a imagem fica perto da malha inteira (2 x 2 rochas de 65 mil triângulos).
  - desempenho: 4 x 4 rochas de 590 mil triângulos (9,4 milhões): o G-buffer com a geometria virtualizada leva menos da
                metade do tempo de --no-nanite.
  - software:   rasterização por software dos pedaços pequenos (--sw-raster, limite de 64 pixels para mandar quase tudo
                por ela): a imagem bate com a do rasterizador fixo (média < 0,5/255; < 0,5% dos pixels acima de 8).
(Sempre com o TAAU do motor: numa RTX o padrão seria o DLSS-RR, cuja reconstrução varia de uma imagem para
outra e mascara a comparação da geometria.)
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile


def rodar(b, pasta, nome, extra, rochas=2, segmentos=256, tamanho=(480, 270), quadros=8):
    out = os.path.join(pasta, f'{nome}.png')
    p = subprocess.run([b, '--headless', '--scene', 'nanite', '--rochas', str(rochas), '--rocha-segmentos', str(segmentos),
                        '--width', str(tamanho[0]), '--height', str(tamanho[1]), '--frames', str(quadros),
                        '--no-motion-blur', '--upscaler', 'taau', '--out', out] + extra, capture_output=True, text=True, encoding='utf-8',
                       errors='replace')
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    return out, p.stdout + p.stderr


def diferenca(a, b):
    import numpy as np
    from PIL import Image
    x = np.asarray(Image.open(a).convert('RGB')).astype(float)
    y = np.asarray(Image.open(b).convert('RGB')).astype(float)
    d = np.abs(x - y)
    return d.mean(), 100.0 * (d.max(2) > 8).mean()


def gbuffer_ms(log):
    linhas = [l for l in log.splitlines() if re.search(r'GPU [0-9.]+ ms', l)]
    m = re.search(r'gbuffer ([0-9.]+)', linhas[-1])
    return float(m.group(1))


def teste_nivel0(b, pasta):
    ref, _ = rodar(b, pasta, 'n0_ref', ['--no-nanite'])
    img, _ = rodar(b, pasta, 'n0', ['--lod-pixels', '0.01'])
    media, fora = diferenca(ref, img)
    return media < 0.05, f'diferença média {media:.3f}/255, {fora:.3f}% dos pixels acima de 8'


def teste_grafo(b, pasta):
    _, log = rodar(b, pasta, 'grafo', [], rochas=1, segmentos=768, tamanho=(64, 36), quadros=1)
    m = re.search(r'Geometria virtualizada: (\d+) triângulos em (\d+) níveis \((\d+) -> (\d+) pedaços; a raiz tem (\d+)', log)
    if not m:
        return False, 'o motor não montou a hierarquia'
    tri, niveis, base, total, raiz = (int(x) for x in m.groups())
    ok = niveis >= 8 and raiz <= 0.01 * tri and total <= 2.5 * base
    return ok, (f'{tri} triângulos: {niveis} níveis, {base} -> {total} pedaços ({total / base:.2f}x), raiz com {raiz} '
                f'triângulos ({100 * raiz / tri:.2f}%)')


def teste_qualidade(b, pasta):
    ref, _ = rodar(b, pasta, 'q_ref', ['--no-nanite'])
    img, _ = rodar(b, pasta, 'q', [])
    media, fora = diferenca(ref, img)
    return media < 1.0, f'1 pixel: diferença média {media:.3f}/255, {fora:.2f}% dos pixels acima de 8'


def teste_desempenho(b, pasta):
    _, log_n = rodar(b, pasta, 'd_n', [], rochas=4, segmentos=768, quadros=6)
    _, log_c = rodar(b, pasta, 'd_c', ['--no-nanite'], rochas=4, segmentos=768, quadros=6)
    n, c = gbuffer_ms(log_n), gbuffer_ms(log_c)
    return n < 0.5 * c, f'G-buffer de 9,4 milhões de triângulos: {n:.0f} ms com, {c:.0f} ms sem ({c / n:.1f}x)'


def teste_software(b, pasta):
    hw, log_h = rodar(b, pasta, 's_hw', [], rochas=4, segmentos=768, quadros=6)
    sw, log_s = rodar(b, pasta, 's_sw', ['--sw-raster', '--sw-raster-px', '64'], rochas=4, segmentos=768, quadros=6)
    media, fora = diferenca(hw, sw)
    return media < 0.5 and fora < 0.5, (f'diferença média {media:.3f}/255, {fora:.2f}% dos pixels acima de 8; G-buffer '
                                        f'{gbuffer_ms(log_h):.0f} ms (fixo) e {gbuffer_ms(log_s):.0f} ms (software, '
                                        f'sem GPU: só a correção vale aqui)')


TESTES = {'nivel0': teste_nivel0, 'grafo': teste_grafo, 'qualidade': teste_qualidade, 'desempenho': teste_desempenho,
          'software': teste_software}


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--so', default='')
    a = ap.parse_args()
    nomes = [n for n in a.so.split(',') if n] or list(TESTES)
    falhas = 0
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as pasta:
        for n in nomes:
            try:
                ok, msg = TESTES[n](a.bin, pasta)
            except Exception as ex:  # noqa: BLE001
                ok, msg = False, f'erro: {ex}'
            falhas += not ok
            print(f'{n:<10} {"PASSOU" if ok else "FALHOU"}: {msg}', flush=True)
    print(f'PASSOU {len(nomes) - falhas} FALHOU {falhas}')
    sys.exit(1 if falhas else 0)


if __name__ == '__main__':
    main()
