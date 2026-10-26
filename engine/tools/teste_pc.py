#!/usr/bin/env python3
"""Bateria completa para rodar no PC com GPU de verdade. Uso (de dentro de engine/):

    python tools/teste_pc.py

Gera a pasta resultados_pc/ com:
  - relatorio.txt  (curto: copie e cole o conteúdo na conversa)
  - imagens .png   (comparações lado a lado; anexe na conversa se quiser)
Leva uns 10-20 minutos. Requer numpy e pillow (pip install numpy pillow).
"""
import os
import platform
import re
import subprocess
import sys
import time

import numpy as np
from PIL import Image

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)

PASTA = 'resultados_pc'


def achar_binario():
    for b in (os.path.join('build', 'Release', 'legacy.exe'), os.path.join('build', 'legacy.exe'),
              os.path.join('build', 'legacy')):
        if os.path.exists(b):
            return b
    sys.exit('Não achei o executável: compile antes (cmake --build build --config Release).')


def rodar(binario, args, timeout=1800):
    p = subprocess.run([binario] + args, capture_output=True, text=True, encoding='utf-8', errors='replace',
                       timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def linha(out, padrao):
    m = re.search(padrao, out)
    return m.group(0) if m else ''


def desempenho(binario, rel):
    """1920x1080, 200 frames, preset Ultra: tempo por frame e por passe na GPU."""
    rel.append('\n== Desempenho (1920x1080, Ultra, média dos frames; "gpu" = tempo de cada passe em ms)')
    casos = [('TAA nativo', ['--upscaler', 'taau', '--scale', '1']), ('TAAU 67%', ['--upscaler', 'taau', '--scale', '0.67']),
             ('DLAA', ['--upscaler', 'dlss', '--scale', '1']), ('DLSS qualidade', ['--upscaler', 'dlss', '--scale', '0.67']),
             ('DLSS desempenho', ['--upscaler', 'dlss', '--scale', '0.5']), ('FSR qualidade', ['--upscaler', 'fsr', '--scale', '0.67']),
             ('Noite 512 luzes', ['--night']), ('Noite 4000 luzes', ['--night', '--lights', '4000']),
             ('Bistro', ['--scene', 'assets/Bistro/bistro.gltf']), ('Cinema', ['--quality', 'cinema'])]
    for nome, args in casos:
        code, out = rodar(binario, ['--headless', '--width', '1920', '--height', '1080', '--frames', '200',
                                    '--out', os.path.join(PASTA, 'perf.png')] + args)
        aviso = linha(out, r'\[warn\][^\n]*indispon[^\n]*')
        ms = linha(out, r'[0-9.]+ ms/frame')
        gpu = linha(out, r'GPU [0-9.]+ ms \([^\n]*')
        rel.append(f'{nome}: {"ERRO código " + str(code) if code else ms}' + (f'  ({aviso})' if aviso else ''))
        if gpu:
            rel.append('   ' + gpu)


def lado_a_lado(caminhos, saida):
    imgs = [Image.open(c).convert('RGB') for c in caminhos if os.path.exists(c)]
    if not imgs:
        return
    w, h = imgs[0].size
    tela = Image.new('RGB', (w * len(imgs), h))
    for i, im in enumerate(imgs):
        tela.paste(im, (i * w, 0))
    tela.save(saida)


def nitidez(binario, rel):
    """Nitidez e erro contra uma referência com 4x os pixels (tools/taa.py). DLSS com jitter errado sai mole."""
    rel.append('\n== Nitidez contra referência 4x (1,00 = igual à referência; erro: menor é melhor)')
    for escala, extra in (('1.0', '--upscaler taau'), ('1.0', '--upscaler dlss'), ('1.0', '--upscaler dlss --sharpness 0.5'),
                          ('0.67', '--upscaler taau'), ('0.67', '--upscaler dlss'),
                          ('0.67', '--upscaler fsr'), ('0.5', '--upscaler dlss')):
        p = subprocess.run([sys.executable, os.path.join(AQUI, 'taa.py'), '--bin', binario, '--out', PASTA,
                            '--scale', escala, '--extra', extra], capture_output=True, text=True, encoding='utf-8',
                           errors='replace')
        erro = (p.stderr.strip().splitlines() or ['?'])[-1]
        rel.append((p.stdout.strip().splitlines() or [f'escala {escala} {extra}: falhou ({erro})'])[-1])


def movimento(binario, rel):
    """Câmera andando: imagens para comparar rastros e ruído (denoiser e upscaler)."""
    rel.append('\n== Movimento (imagens lado a lado em resultados_pc/)')
    base = ['--headless', '--width', '1280', '--height', '720', '--frames', '48', '--move', '3']
    grupos = {
        'denoiser_noite_andando': [('svgf', ['--night', '--denoiser', 'svgf']), ('relax', ['--night', '--denoiser', 'relax'])],
        'upscaler_andando': [('taau', ['--scale', '0.67', '--upscaler', 'taau']), ('dlss', ['--scale', '0.67', '--upscaler', 'dlss']),
                             ('fsr', ['--scale', '0.67', '--upscaler', 'fsr'])],
        'upscaler_parado': [('taau', ['--scale', '0.67', '--upscaler', 'taau', '--pause', '--move', '0']),
                            ('dlss', ['--scale', '0.67', '--upscaler', 'dlss', '--pause', '--move', '0'])],
    }
    for grupo, casos in grupos.items():
        arqs = []
        for nome, args in casos:
            arq = os.path.join(PASTA, f'{grupo}_{nome}.png')
            code, _ = rodar(binario, base + ['--out', arq] + args)
            if code == 0:
                arqs.append(arq)
        lado_a_lado(arqs, os.path.join(PASTA, f'{grupo}_comparacao.png'))
        rel.append(f'{grupo}: {" | ".join(n for n, _ in casos)} -> {grupo}_comparacao.png')


def main():
    for f in (sys.stdout, sys.stderr):
        f.reconfigure(encoding='utf-8', errors='replace')
    binario = achar_binario()
    os.makedirs(PASTA, exist_ok=True)
    rel = [f'Legacy Engine — teste no PC ({time.strftime("%Y-%m-%d %H:%M")}, {platform.system()} {platform.release()})']
    code, out = rodar(binario, ['--headless', '--width', '64', '--height', '64', '--frames', '1', '--upscaler', 'dlss',
                                '--out', os.path.join(PASTA, 'info.png')])
    rel.append(linha(out, r'GPU: [^\n]*') or 'GPU: ?')
    rel.append('DLSS: ' + (linha(out, r'DLSS (ativo|indispon)[^\n]*') or '?'))

    print('1/4 suíte de testes...', flush=True)
    p = subprocess.run([sys.executable, os.path.join(AQUI, 'testes.py'), '--bin', binario], capture_output=True, text=True,
                       encoding='utf-8', errors='replace', env=dict(os.environ, PYTHONIOENCODING='utf-8'))
    rel.append('\n== Suíte de testes')
    detalhe = False  # depois de um FALHOU, as linhas recuadas trazem o motivo
    for l in p.stdout.splitlines():
        if 'PASSOU' in l or 'FALHOU' in l or l.startswith('Resumo'):
            rel.append(l)
            detalhe = 1 if 'FALHOU' in l else 0
        elif detalhe and l.startswith('    ') and detalhe < 9:  # até 8 linhas de motivo por teste
            rel.append(l[:400])
            detalhe += 1
    if p.returncode not in (0, 1):
        rel.append('suíte interrompida: ' + ' | '.join(p.stderr.strip().splitlines()[-3:]))
    print('2/4 desempenho...', flush=True)
    desempenho(binario, rel)
    print('3/4 nitidez...', flush=True)
    nitidez(binario, rel)
    print('4/4 movimento...', flush=True)
    movimento(binario, rel)

    with open(os.path.join(PASTA, 'relatorio.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(rel) + '\n')
    print('\n'.join(rel))
    print(f'\nPronto. Copie o texto acima (ou {PASTA}/relatorio.txt) e cole na conversa.')


if __name__ == '__main__':
    main()
