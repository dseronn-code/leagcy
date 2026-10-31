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
             ('DLSS-RR qualidade', ['--denoiser', 'dlss-rr', '--scale', '0.67']),
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


def denoisers(binario, rel):
    """SVGF, NRD ReLAX e DLSS Ray Reconstruction: erro andando contra referência e cintilação parado (tools/denoiser.py)."""
    rel.append('\n== Denoisers contra referência verdadeira (Sponza à noite; erro e brilhinhos: menor é melhor)')
    for cena, titulo in (('sponza-noite', 'cena congelada'), ('sponza-noite-animada', 'personagens e luzes animados')):
        p = subprocess.run([sys.executable, os.path.join(AQUI, 'denoiser.py'), '--bin', binario, '--out',
                            os.path.join(PASTA, 'denoiser'), '--cena', cena, '--ref-frames', '2048',
                            '--candidatos', 'svgf,relax,dlss-rr'],
                           capture_output=True, text=True, encoding='utf-8', errors='replace',
                           env=dict(os.environ, PYTHONIOENCODING='utf-8'))
        rel.append(f'-- {titulo}')
        linhas = p.stdout.strip().splitlines()
        rel += linhas if linhas else ['falhou: ' + ' | '.join(p.stderr.strip().splitlines()[-2:])]


def texturas(binario, rel):
    """Compressão BC7/BC5: memória de vídeo, qualidade das texturas e diferença na imagem final."""
    rel.append('\n== Texturas comprimidas (BC7/BC5) contra RGBA8')
    for cena in ([], ['--scene', 'assets/Bistro/bistro.gltf']):
        nome = 'Bistro' if cena else 'Sponza'
        imgs = []
        # O Bistro já vem em BC7 no disco (DDS): não há original sem compressão para comparar, e em RGBA8 as
        # texturas dele (~9 GB) nem cabem numa placa de 8 GB — só o log de memória.
        for extra in ([], ['--no-texture-compression']) if not cena else ([],):
            arq = os.path.join(PASTA, f'texturas_{nome}_{"rgba" if extra else "bc"}.png')
            code, out = rodar(binario, ['--headless', '--pause', '--exposure', '12', '--no-motion-blur', '--width', '1280',
                                        '--height', '720', '--frames', '32', '--out', arq] + cena + extra)
            if not extra:
                rel.append(f'{nome}: ' + (linha(out, r'Texturas BC7/BC5:[^\n]*') or f'sem log (código {code})'))
            imgs.append(arq)
        if all(os.path.exists(i) for i in imgs):
            a, b = (np.asarray(Image.open(i).convert('RGB')).astype(np.float32) / 255.0 for i in imgs)
            a, b = (np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4) @ np.array([0.2126, 0.7152, 0.0722]) for x in (a, b))
            rel.append(f'   imagem final BC x RGBA8: diferença {float((np.abs(a - b) / (b + 0.02)).mean()):.4f}, '
                       f'viés {100 * (a.mean() / b.mean() - 1):+.2f}%')


def reflexos(binario, rel):
    """Reflexos e luz indireta contra a verdade por path tracing (tools/reflexos.py) e reflexos em movimento."""
    rel.append('\n== Reflexos e luz indireta contra path tracing (erro: menor é melhor; viés: luz a mais/menos)')
    for nome, extra in (('com cache de radiância', []), ('sem cache (antes)', ['--extra=--no-radiance-cache'])):
        p = subprocess.run([sys.executable, os.path.join(AQUI, 'reflexos.py'), '--bin', binario, '--out',
                            os.path.join(PASTA, 'reflexos')] + extra, capture_output=True, text=True, encoding='utf-8',
                           errors='replace', env=dict(os.environ, PYTHONIOENCODING='utf-8'))
        rel.append(f'-- {nome}')
        rel += p.stdout.strip().splitlines() or ['falhou: ' + ' | '.join(p.stderr.strip().splitlines()[-2:])]
    rel.append('-- reflexos andando (sala de espelho)')
    for nome, args in (('', ['--candidatos', 'svgf,relax,dlss-rr']),
                       (' (SVGF sem movimento virtual)', ['--candidatos', 'svgf', '--extra=--no-virtual-motion'])):
        p = subprocess.run([sys.executable, os.path.join(AQUI, 'denoiser.py'), '--bin', binario, '--cena', 'reflections',
                            '--out', os.path.join(PASTA, 'reflexos_mov')] + args, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', env=dict(os.environ, PYTHONIOENCODING='utf-8'))
        rel += [l + nome for l in p.stdout.strip().splitlines()] or ['falhou: ' + ' | '.join(p.stderr.strip().splitlines()[-2:])]


def luzes(binario, rel):
    """Luzes de malha (emissivos como luz): prova de energia por path tracing e motor x referência (tools/luzes.py),
    mais o Bistro de dia e de noite para olhar."""
    rel.append('\n== Luzes de malha (prova: com x sem tem que dar ~0; motor x referência: menor é melhor)')
    p = subprocess.run([sys.executable, os.path.join(AQUI, 'luzes.py'), '--bin', binario, '--out', os.path.join(PASTA, 'luzes')],
                       capture_output=True, text=True, encoding='utf-8', errors='replace',
                       env=dict(os.environ, PYTHONIOENCODING='utf-8'))
    rel += p.stdout.strip().splitlines() or ['falhou: ' + ' | '.join(p.stderr.strip().splitlines()[-2:])]
    bistro = ['--scene', os.path.join('assets', 'Bistro', 'bistro.gltf')]
    arqs = []
    for nome, extra in (('dia', []), ('noite', ['--night'])):
        arq = os.path.join(PASTA, f'bistro_{nome}.png')
        code, out = rodar(binario, ['--headless', '--pause', '--width', '1920', '--height', '1080', '--frames', '64',
                                    '--out', arq] + bistro + extra)
        rel.append(f'Bistro {nome}: ' + (linha(out, r'\d+ frames em [^\n]*') or f'falhou (código {code})'))
        if code == 0:
            arqs.append(arq)
    lado_a_lado(arqs, os.path.join(PASTA, 'bistro_dia_noite.png'))


def galeria(binario, rel):
    """Imagens 1080p dos recursos novos, com o tempo de quadro de cada um (aparência e custo numa GPU de verdade)."""
    rel.append('\n== Galeria (imagens em resultados_pc/galeria_*.png; ms/frame no Ultra a 1080p)')
    bistro = os.path.join('assets', 'Bistro', 'bistro.gltf')
    casos = [
        ('oceano_por_do_sol', ['--scene', 'ocean', '--sun-elevation', '6', '--sun-azimuth', '15']),
        ('oceano_dia', ['--scene', 'ocean']),
        ('cidade_chuva', ['--scene', 'city', '--rain', '0.8']),
        ('cidade_noite_chuva', ['--scene', 'city', '--night', '--rain', '0.6']),
        ('sponza_raios_de_luz', ['--fog', '0.02']),
        ('externa_nuvens', ['--scene', 'outdoor', '--sun-elevation', '50', '--cam', '0,3,20', '--look', '0,12,-60']),
        ('fogueira_noite', ['--scene', 'outdoor', '--night', '--look', '-3,1.5,11']),
        ('bistro_dia', ['--scene', bistro]),
        ('bistro_noite', ['--scene', bistro, '--night']),
        ('vidro', ['--scene', 'glass']),
        ('pintura', ['--scene', 'paint']),
        ('ceu_lua_estrelas', ['--scene', 'outdoor', '--night', '--cam', '0,2,30', '--look', '3,8,25', '--exposure', '-6']),
    ]
    for nome, args in casos:
        arq = os.path.join(PASTA, f'galeria_{nome}.png')
        code, out = rodar(binario, ['--headless', '--width', '1920', '--height', '1080', '--frames', '96', '--out', arq] + args)
        rel.append(f'{nome}: ' + (linha(out, r'\d+ frames em [^\n]*') or f'falhou (código {code})'))


def movimento(binario, rel):
    """Câmera andando: imagens para comparar rastros e ruído (denoiser e upscaler)."""
    rel.append('\n== Movimento (imagens lado a lado em resultados_pc/)')
    # Sem motion blur: o desfoque cinematográfico borraria tudo igual e esconderia rastros do denoiser/upscaler.
    base = ['--headless', '--no-motion-blur', '--width', '1280', '--height', '720', '--frames', '48', '--move', '3']
    grupos = {
        'denoiser_noite_andando': [('svgf', ['--night', '--denoiser', 'svgf']), ('relax', ['--night', '--denoiser', 'relax']),
                                   ('dlss-rr', ['--night', '--denoiser', 'dlss-rr'])],
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


def gpudriven(binario, rel):
    """Culling GPU-driven: oclusão HiZ e meshlets ligados x desligados (tempo do G-buffer e imagem igual)."""
    rel.append('\n== GPU-driven (1920x1080, 120 frames andando; "gbuffer" = ms da passada na GPU; dif = maior diferença de pixel)')
    cenas = [('Sponza', []), ('Cidade', ['--scene', 'city']), ('Cidade 30 quadras', ['--scene', 'city', '--city-blocks', '30']),
             ('Mundo 8 km', ['--scene', 'world']), ('Bistro', ['--scene', 'assets/Bistro/bistro.gltf'])]
    casos = [('tudo ligado', []), ('sem oclusão', ['--no-occlusion']), ('sem meshlets', ['--no-meshlets']),
             ('sem culling', ['--no-gpu-culling', '--no-occlusion', '--no-meshlets'])]
    for cena, extra in cenas:
        ref = None
        for nome, args in casos:
            arq = os.path.join(PASTA, 'gpudriven.png')
            code, out = rodar(binario, ['--headless', '--width', '1920', '--height', '1080', '--frames', '120', '--move', '2',
                                        '--out', arq] + extra + args)
            if code:
                rel.append(f'{cena} / {nome}: ERRO código {code}')
                continue
            gb = linha(out, r'gbuffer [0-9.]+')
            ms = linha(out, r'[0-9.]+ ms/frame')
            img = np.asarray(Image.open(arq).convert('RGB')).astype(int)
            dif = '' if ref is None else f', dif {int(np.abs(img - ref).max())}'
            ref = img if ref is None else ref
            rel.append(f'{cena} / {nome}: {ms}, {gb}{dif}')


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

    # --so secao1,secao2: só algumas seções (suite, desempenho, nitidez, denoisers, reflexos, luzes, galeria, movimento,
    # gpudriven).
    so = None
    if '--so' in sys.argv:
        so = set(sys.argv[sys.argv.index('--so') + 1].split(','))
    quer = lambda s: so is None or s in so

    if quer('suite'):
        print('suíte de testes...', flush=True)
        suite(binario, rel)
    etapas = [('desempenho', desempenho), ('nitidez', nitidez), ('denoisers', denoisers), ('denoisers', texturas),
              ('reflexos', reflexos), ('luzes', luzes), ('galeria', galeria), ('movimento', movimento),
              ('gpudriven', gpudriven)]
    for nome, f in etapas:
        if quer(nome):
            print(f'{nome}...', flush=True)
            f(binario, rel)

    with open(os.path.join(PASTA, 'relatorio.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(rel) + '\n')
    print('\n'.join(rel))
    print(f'\nPronto. Copie o texto acima (ou {PASTA}/relatorio.txt) e cole na conversa.')


def suite(binario, rel):
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


if __name__ == '__main__':
    main()
