#!/usr/bin/env python3
"""Suíte de regressão do Legacy Engine. Uso (de dentro de engine/):

    python3 tools/testes.py [--rapido] [--bin build/legacy]

Cada teste tem um critério de aprovação objetivo; o resumo final lista o que passou e o que falhou.
Requer numpy e pillow (pip install numpy pillow).
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)


def rodar(binario, args, env_extra=None, timeout=3600):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    # O motor escreve em UTF-8; no Windows o padrão do Python seria cp1252 (acentos viram lixo e as buscas falham).
    p = subprocess.run([binario] + args, capture_output=True, text=True, encoding='utf-8', errors='replace', env=env,
                       timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def linear(img):
    a = np.asarray(Image.open(img).convert('RGB')).astype(np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def teste_validacao(binario, tmp, rapido):
    """Camadas de validação Vulkan (inclusive sincronização) em todos os presets e cenas."""
    presets = ['ultra'] if rapido else ['baixo', 'medio', 'alto', 'ultra', 'cinema']
    cenas = [[], ['--night'], ['--scene', 'outdoor', '--dof'], ['--no-rt'], ['--scene', 'furnace'],
             ['--scene', 'assets/Bistro/bistro.gltf']]
    falhas = []
    for q in presets:
        for c in cenas:
            code, out = rodar(binario, ['--headless', '--validation', '--quality', q, '--width', '320', '--height', '180',
                                        '--frames', '3', '--out', os.path.join(tmp, 'v.png')] + c,
                              {'VK_KHRONOS_VALIDATION_VALIDATE_SYNC': 'true'})
            msgs = [l for l in out.splitlines() if ('[error]' in l or '[warn]' in l) and 'Textura não carregada' not in l]
            if code != 0 or msgs:
                falhas.append(f"{q} {' '.join(c)}: código {code}; {msgs[:2]}")
    return not falhas, f"{len(presets) * len(cenas)} combinações" + ('' if not falhas else '\n    ' + '\n    '.join(falhas))


def teste_fornalha(binario, tmp, rapido):
    """Fornalha branca: esferas brancas sob céu uniforme devem ter o brilho do fundo (0,93–1,03)."""
    # Teste de energia, não de velocidade: 96 frames deixam convergir o metal de rugosidade 1 (o sinal mais
    # ruidoso), que o NRD acumula devagar por causa do clamp estatístico do histórico (0,925 em 48 → 0,937 em 96).
    img = os.path.join(tmp, 'fornalha.png')
    rodar(binario, ['--headless', '--scene', 'furnace', '--quality', 'cinema', '--exposure', '2.2', '--no-motion-blur',
                    '--width', '960', '--height', '540', '--frames', '48' if rapido else '96', '--out', img])
    lum = linear(img) @ np.array([0.2126, 0.7152, 0.0722])
    H, W = lum.shape
    hh = 13 * np.tan(np.radians(30)); hw = hh * W / H
    rpx = 1.0 / hh * 0.5 * H
    yy, xx = np.mgrid[0:H, 0:W]
    valores = []
    for y in (1.3, -1.3):
        for i in range(6):
            px = int(((-6.25 + 2.5 * i) / hw * 0.5 + 0.5) * W); py = int((0.5 - y / hh * 0.5) * H)
            d = np.hypot(xx - px, yy - py)
            anel = lum[(d > 1.15 * rpx) & (d < 1.3 * rpx) & (np.abs(yy - py) < 0.5 * rpx)]
            valores.append(lum[py - 8:py + 9, px - 8:px + 9].mean() / np.median(anel))
    ok = all(0.93 <= v <= 1.03 for v in valores)
    return ok, 'razões ' + ' '.join(f'{v:.3f}' for v in valores)


def teste_atmosfera(binario, tmp, rapido):
    """Céu do motor vs path tracing de referência (todas as ordens): erro médio < 5%."""
    code, out = rodar(binario, ['--atmosphere-test', '512' if rapido else '2048'])
    m = re.search(r'Erro relativo médio de luminância.*: ([0-9.]+)%', out)
    if not m:
        return False, 'sem resultado: ' + out[-300:]
    erro = float(m.group(1))
    return erro < 5.0, f'erro médio {erro:.2f}%'


def teste_cintilacao(binario, tmp, rapido):
    """Sponza à noite, cena congelada, preset Ultra: pixels com brilhinho < 0,35%."""
    pasta = os.path.join(tmp, 'cint')
    os.makedirs(pasta, exist_ok=True)
    rodar(binario, ['--headless', '--pause', '--night', '--width', '640', '--height', '360', '--frames', '40',
                    '--save-every', '1', '--out', os.path.join(pasta, 'f.png')])
    os.replace(os.path.join(pasta, 'f.png'), os.path.join(pasta, 'f_0040.png'))
    arquivos = sorted(glob.glob(os.path.join(pasta, 'f_*.png')))[-16:]
    L = np.stack([linear(f) @ np.array([0.2126, 0.7152, 0.0722]) for f in arquivos])
    cv = (L.std(0) / (L.mean(0) + 0.01)).mean()
    brilho = 100 * (L > 2.0 * np.median(L, 0) + 0.02).any(0).mean()
    return brilho < 0.35, f'cintilação {cv:.4f}, brilhinhos {brilho:.2f}%'


def teste_pixels(binario, tmp, rapido):
    """Sem pixels inválidos: nenhuma região de preto puro isolado (NaN zerado) nas cenas principais."""
    falhas = []
    for nome, args in (('dia', []), ('noite', ['--night']), ('externa', ['--scene', 'outdoor'])):
        img = os.path.join(tmp, f'px_{nome}.png')
        rodar(binario, ['--headless', '--pause', '--width', '640', '--height', '360', '--frames', '16', '--out', img] + args)
        a = np.asarray(Image.open(img).convert('RGB')).astype(np.int32)
        preto = (a.sum(2) == 0)
        # Preto puro cercado de pixels claros = valor inválido descartado pelo TAA.
        # Borda replicada (np.roll daria a volta e compararia o céu do topo com o chão da base).
        s = np.pad(a.sum(2), 1, mode='edge')
        H, W = a.shape[:2]
        viz = sum(s[1 + dy:1 + dy + H, 1 + dx:1 + dx + W] for dy in (-1, 0, 1) for dx in (-1, 0, 1))
        isolados = int((preto & (viz > 9 * 60)).sum())
        if isolados > 0:
            falhas.append(f'{nome}: {isolados} pixels pretos isolados')
    return not falhas, 'ok' if not falhas else '; '.join(falhas)


def teste_sombras(binario, tmp, rapido):
    """Sombra do sol: sem degraus no terminator (luz perdida ≤ 1%, na origem e a 4 km) e sem acne."""
    import sombras
    linhas, ok = [], True
    # A 4 km o limite é o mesmo da origem desde a renderização relativa à câmera (antes: 2,4% / 9,4% na RTX).
    for cena, limite in (('shadows', 1.0), ('shadows-far', 1.0)):
        for elev in ('20',) if rapido else ('20', '3'):
            falsas, acne = sombras.medir(binario, cena, elev, tmp)
            ok &= max(falsas) <= limite and acne < 0.01
            linhas.append(f'{cena} {elev}°: luz perdida máx. {max(falsas):.1f}%, acne {acne:.3f}%')
    return ok, '; '.join(linhas)


def teste_restir(binario, tmp, rapido):
    """ReSTIR sem viés: luz das luzes locais (média de 16 frames) contra força bruta, |viés| < 3%."""
    import restir
    linhas, ok = [], True
    for cena in ('sponza',) if rapido else ('sponza', 'externa'):
        # Só as luzes pontuais: a força bruta percorre todas as luzes, e os ~15 mil triângulos emissivos do capacete
        # (luzes de malha) a tornariam horas mais lenta sem GPU. As luzes de malha têm a própria prova (tools/luzes.py).
        relmse, vies = restir.medir(binario, cena, tmp, ['--quality', 'ultra', '--no-mesh-lights'])
        ok &= abs(vies) < 3.0
        linhas.append(f'{cena}: viés {vies:+.2f}%, relMSE {relmse:.2f}')
    return ok, '; '.join(linhas)


TESTES = [teste_validacao, teste_fornalha, teste_atmosfera, teste_cintilacao, teste_pixels, teste_sombras, teste_restir]


def main():
    for s in (sys.stdout, sys.stderr):  # "≤", "°", "✓" num pipe do Windows (cp1252) derrubariam o print
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=os.path.join('build', 'legacy'))
    ap.add_argument('--rapido', action='store_true')
    ap.add_argument('--so', help='rodar só testes cujo nome contém este texto')
    args = ap.parse_args()
    binario = args.bin
    if not os.path.exists(binario) and os.path.exists(os.path.join('build', 'Release', 'legacy.exe')):
        binario = os.path.join('build', 'Release', 'legacy.exe')
    resultados = []
    with tempfile.TemporaryDirectory() as tmp:
        for t in TESTES:
            if args.so and args.so not in t.__name__:
                continue
            print(f'== {t.__name__}: {t.__doc__.strip()}', flush=True)
            try:
                ok, info = t(binario, tmp, args.rapido)
            except Exception as e:  # noqa: BLE001 — falha do teste vira resultado, não aborta a suíte
                ok, info = False, f'exceção: {e}'
            print(f'   {"PASSOU" if ok else "FALHOU"}: {info}', flush=True)
            resultados.append((t.__name__, ok))
    print('\nResumo: ' + ', '.join(f'{n} {"✓" if ok else "✗"}' for n, ok in resultados))
    sys.exit(0 if all(ok for _, ok in resultados) else 1)


if __name__ == '__main__':
    main()
