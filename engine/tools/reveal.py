#!/usr/bin/env python3
"""Monta o vídeo de reveal do Legacy Engine a partir das tomadas do tools/trailer.py.

    python tools/reveal.py --tomadas resultados_pc/trailer --saida reveal.mp4 [--teste]

Estrutura (a dos reveals de motor: história de luz do escuro ao aceso, escala revelada pela câmera, uma frase curta com
número por recurso, uma interrupção com a vista de depuração, rótulo de captura no canto, logo no fim):
cada tomada é cortada, ganha o seu texto (entra e sai em fade) e o rótulo; as tomadas se ligam por fusões curtas; a
trilha é composta aqui mesmo (drone, chuva, pulso, impactos nos cortes, subida antes das revelações), sem música de
terceiros. --teste usa quadros coloridos no lugar das tomadas (para conferir a montagem sem o motor).
"""
import argparse
import os
import shutil
import subprocess
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H, FPS, SR = 1920, 1080, 30, 48000
FUSAO = 0.45  # segundos de fusão entre tomadas
AQUI = os.path.dirname(os.path.abspath(__file__))
MARCA = os.path.join(AQUI, '..', '..', 'plataforma', 'marca', 'legacy_engine-512.png')
FONTES = '/usr/share/fonts/opentype/inter'

# tomada, início dentro dela (s), duração (s), texto pequeno (laranja), texto grande, nota pequena
ROTEIRO = [
    ('rv_chuva', 0.0, 7.0, '', '', ''),
    ('rv_rua', 0.5, 7.0, 'TIMES SQUARE', 'Cada telão é uma luz.', ''),
    ('rv_luzes', 0.5, 3.0, 'VISTA DE DEPURAÇÃO', 'Mil luzes. Todas com sombra.', 'cor = luz escolhida em cada pixel'),
    ('sponza_luz', 0.5, 7.0, 'LUZ GLOBAL', 'Sem bake. Tudo muda no mesmo quadro.', ''),
    ('bistro_noite', 0.5, 7.0, 'PROVA FÍSICA', '6% do path tracing. A 60 q/s.', 'Bistro, 1080p, preset Médio, RTX 3070 Ti'),
    ('rv_rochas', 0.0, 10.0, 'GEOMETRIA DENSA', '151 milhões de triângulos. 4,1 ms.', 'G-buffer, RTX 3070 Ti'),
    ('pele', 0.0, 6.0, 'PELE', 'A luz atravessa. Como na pele de verdade.', ''),
    ('pintura', 0.0, 7.0, 'MATERIAIS', 'Verniz e flocos metálicos, raio por raio.', ''),
    ('oceano', 0.5, 7.0, 'MUNDO', 'Oceano, nuvens e céu físicos.', ''),
    ('nuvens', 0.5, 6.0, '', '', ''),
    ('rv_aereo', 0.0, 9.0, '', '', ''),
]
FINAL = 8.0  # cartão do logo
TEXTO_EM = 0.9  # o texto entra este tanto depois do começo da tomada


def fonte(tam, peso='Bold'):
    for f in (f'{FONTES}/InterDisplay-{peso}.otf', f'{FONTES}/Inter-{peso}.otf',
              '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'):
        if os.path.exists(f):
            return ImageFont.truetype(f, tam)
    return ImageFont.load_default()


def camada_texto(pequeno, grande, nota, arq):
    """Texto do recurso, embaixo à esquerda, com uma sombra suave para ler sobre qualquer imagem."""
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    sombra = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    # faixa escura em degradê na base (legibilidade)
    faixa = Image.new('L', (1, 360))
    for y in range(360):
        faixa.putpixel((0, y), int(170 * (y / 359) ** 1.6))
    sombra.paste((0, 0, 0, 255), (0, H - 360, W, H), faixa.resize((W, 360)))
    d = ImageDraw.Draw(im)
    x, y = 120, H - 250
    if pequeno:
        d.text((x, y), pequeno, font=fonte(30, 'SemiBold'), fill=(255, 181, 71, 255))
        # tracinho em degradê antes do título
        y += 48
    if grande:
        d.text((x, y), grande, font=fonte(76), fill=(244, 245, 249, 255))
        y += 100
    if nota:
        d.text((x, y), nota, font=fonte(24, 'Medium'), fill=(170, 176, 190, 255))
    Image.alpha_composite(sombra, im).save(arq)


def camada_rotulo(arq):
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    t = 'Capturado no Legacy Engine  ·  NVIDIA RTX 3070 Ti'
    f = fonte(22, 'Medium')
    w = d.textlength(t, font=f)
    d.text((W - 70 - w, 56), t, font=f, fill=(230, 232, 238, 150))
    im.save(arq)


def cartao_final(arq):
    im = Image.new('RGB', (W, H), (8, 9, 12))
    d = ImageDraw.Draw(im)
    logo = Image.open(MARCA).convert('RGBA').resize((220, 220), Image.LANCZOS)
    im.paste(logo, ((W - 220) // 2, 250), logo)
    for texto, tam, peso, cor, y in (('LEGACY ENGINE', 104, 'Bold', (244, 245, 249), 520),
                                     ('Luz de verdade. Medida.', 44, 'Medium', (255, 181, 71), 660),
                                     ('Grátis no Legacy Launcher  ·  legacyengine.com.br', 30, 'Regular',
                                      (170, 176, 190), 760)):
        f = fonte(tam, peso)
        w = d.textlength(texto, font=f)
        d.text(((W - w) / 2, y), texto, font=f, fill=cor)
    nota = ('Números medidos numa RTX 3070 Ti em 1080p: Bistro (Amazon Lumberyard, CC BY 4.0) no preset Médio, erro = '
            'diferença média contra path tracing de 512 quadros; campo de rochas: G-buffer.')
    f = fonte(17, 'Regular')
    w = d.textlength(nota, font=f)
    d.text(((W - w) / 2, H - 70), nota, font=f, fill=(110, 115, 128))
    im.save(arq)


def rodar(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        sys.exit(f'ffmpeg falhou:\n{" ".join(cmd)}\n{p.stderr[-3000:]}')


# ---------------------------------------------------------------- trilha (composta aqui, sem música de terceiros)

def filtro(x, lo=None, hi=None):
    """Passa-faixa por FFT (o sinal inteiro de uma vez)."""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    g = np.ones_like(f)
    if lo:
        g *= 1 / (1 + (lo / np.maximum(f, 1e-3)) ** 4)
    if hi:
        g *= 1 / (1 + (f / hi) ** 4)
    return np.fft.irfft(X * g, len(x))


def reverb(x, segundos=2.8, mistura=0.35, semente=1):
    rng = np.random.default_rng(semente)
    n = int(segundos * SR)
    ir = rng.standard_normal(n) * np.exp(-np.linspace(0, 7, n))
    ir = filtro(ir, hi=6000)
    ir /= np.sqrt(np.sum(ir ** 2))
    m = len(x) + n
    y = np.fft.irfft(np.fft.rfft(x, m) * np.fft.rfft(ir, m), m)[:len(x)]
    return x * (1 - mistura) + y * mistura


def envelope(t, pontos):
    """Interpola volume entre (tempo, nível)."""
    ts, vs = zip(*pontos)
    return np.interp(t, ts, vs)


def trilha(total, cortes, arq):
    n = int(total * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(7)
    mix = np.zeros(n)
    c = {nome: inicio for nome, inicio in cortes}
    t_rochas, t_aereo, t_final = c['rv_rochas'], c['rv_aereo'], c['final']
    # chuva: ruído rosado filtrado, forte na Times Square, some no meio, volta no voo final
    chuva = filtro(rng.standard_normal(n), lo=400, hi=9000)
    chuva *= envelope(t, [(0, 0), (1.5, 0.22), (c['rv_luzes'], 0.16), (c['sponza_luz'], 0.03), (t_aereo - 1, 0.0),
                          (t_aereo + 1, 0.14), (t_final, 0.05), (total, 0)])
    mix += chuva
    # drone em ré: fundamentais e quinta, com batimento lento; cresce até o fim
    drone = np.zeros(n)
    for f0, a in ((36.71, 1.0), (55.0, 0.6), (73.42, 0.5), (110.0, 0.25), (146.83, 0.12)):
        drone += a * np.sin(2 * np.pi * f0 * t + 0.6 * np.sin(2 * np.pi * 0.07 * t)) * (0.8 + 0.2 * np.sin(2 * np.pi * 0.13 * t + f0))
    mix += 0.18 * drone * envelope(t, [(0, 0), (3, 0.5), (t_rochas, 0.8), (t_aereo, 1.0), (t_final + 2, 1.0), (total, 0)])
    # acordes (Dm - Bb - F - C) em ondas suaves com quinta, entram na Sponza e crescem no voo final
    acordes = [(146.83, 174.61, 220.0), (116.54, 146.83, 174.61), (174.61, 220.0, 261.63), (130.81, 164.81, 196.0)]
    pad = np.zeros(n)
    dur = 4.0
    for k in range(int(total / dur) + 1):
        a, b = int(k * dur * SR), min(n, int((k + 1) * dur * SR + 0.5 * SR))
        if a >= n:
            break
        tt = t[a:b] - t[a]
        env = np.minimum(1, tt / 0.8) * np.minimum(1, np.maximum(0, (b - a) / SR - tt) / 0.8)
        for f0 in acordes[k % 4]:
            for det in (-0.12, 0.0, 0.12):
                pad[a:b] += env * (np.sin(2 * np.pi * (f0 + det) * tt) + 0.3 * np.sin(4 * np.pi * (f0 + det) * tt))
    pad = filtro(pad, hi=1800)
    mix += 0.035 * pad * envelope(t, [(0, 0), (c['sponza_luz'], 0.0), (c['sponza_luz'] + 2, 0.6), (t_rochas, 0.8),
                                      (c['pele'], 0.4), (t_aereo, 1.2), (t_final + 3, 1.0), (total, 0)])
    # pulso: bumbo a 100 bpm do meio em diante, dobra no voo final; caixa com reverb nos tempos 2 e 4
    bpm = 100.0
    bumbo = np.zeros(n)
    kick_n = int(0.45 * SR)
    kt = np.arange(kick_n) / SR
    kick = np.sin(2 * np.pi * (45 + 90 * np.exp(-kt * 30)) * kt) * np.exp(-kt * 7)
    caixa_n = int(0.3 * SR)
    caixa = filtro(rng.standard_normal(caixa_n), lo=900, hi=7000) * np.exp(-np.arange(caixa_n) / SR * 18)
    batida = 60 / bpm
    k = 0
    while True:
        tb = c['bistro_noite'] + k * batida
        if tb >= t_final:
            break
        dentro_pele = c['pele'] <= tb < c['oceano']
        if not dentro_pele:
            i = int(tb * SR)
            bumbo[i:i + kick_n] += kick[:max(0, min(kick_n, n - i))]
            if k % 2 == 1 and tb >= t_rochas:
                bumbo[i:i + caixa_n] += 0.5 * caixa[:max(0, min(caixa_n, n - i))]
            if tb >= t_aereo:  # contratempo no voo final
                i2 = int((tb + batida / 2) * SR)
                bumbo[i2:i2 + kick_n] += 0.6 * kick[:max(0, min(kick_n, n - i2))]
        k += 1
    mix += 0.30 * reverb(bumbo, 1.2, 0.2)
    # impactos (boom grave + estalo) nos momentos-chave e subidas antes das revelações
    imp_n = int(4.0 * SR)
    it = np.arange(imp_n) / SR
    boom = np.sin(2 * np.pi * (28 + 70 * np.exp(-it * 5)) * it) * np.exp(-it * 1.3)
    boom += 0.35 * filtro(rng.standard_normal(imp_n), hi=3000) * np.exp(-it * 9)
    for tb, g in ((c['rv_rua'], 0.6), (t_rochas, 0.9), (c['pele'], 0.5), (t_aereo, 1.0), (t_final, 1.3)):
        i = int(tb * SR)
        mix[i:i + imp_n] += 0.55 * g * boom[:max(0, min(imp_n, n - i))]
    for alvo, comp in ((t_rochas, 4.0), (t_aereo, 5.0), (t_final, 3.0)):
        a = int((alvo - comp) * SR)
        m = int(comp * SR)
        rt = np.arange(m) / SR
        sobe = filtro(rng.standard_normal(m), lo=300, hi=12000) * (rt / comp) ** 3
        sobe += 0.4 * np.sin(2 * np.pi * (200 + 600 * (rt / comp) ** 2) * rt) * (rt / comp) ** 2
        mix[a:a + m] += 0.12 * sobe
    mix = reverb(mix, 3.0, 0.25)
    mix *= envelope(t, [(0, 1), (total - 2.5, 1), (total, 0)])
    mix /= np.max(np.abs(mix)) + 1e-9
    mix = np.tanh(mix * 1.6) / np.tanh(1.6) * 0.92  # leve saturação: corpo e volume
    est = np.stack([mix, np.roll(mix, int(0.011 * SR))], axis=1)  # estéreo largo
    with wave.open(arq, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((est * 32767).astype('<i2').tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tomadas', default=os.path.join('resultados_pc', 'trailer'))
    ap.add_argument('--saida', default='reveal.mp4')
    ap.add_argument('--teste', action='store_true', help='quadros coloridos no lugar das tomadas')
    a = ap.parse_args()
    tmp = a.saida + '.partes'
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    rotulo = os.path.join(tmp, 'rotulo.png')
    camada_rotulo(rotulo)
    partes, cortes, agora = [], [], 0.0
    for k, (nome, ini, dur, peq, gra, nota) in enumerate(ROTEIRO):
        cortes.append((nome, agora))
        txt = os.path.join(tmp, f'texto{k}.png')
        camada_texto(peq, gra, nota, txt)
        src = os.path.join(a.tomadas, nome + '.mp4')
        if a.teste:
            entrada = ['-f', 'lavfi', '-i', f'testsrc2=size={W}x{H}:rate={FPS}:duration={dur}']
        else:
            if not os.path.exists(src):
                sys.exit(f'Falta a tomada {src} (rode tools/trailer.py --so {nome})')
            entrada = ['-ss', str(ini), '-t', str(dur), '-i', src]
        out = os.path.join(tmp, f'p{k:02d}.mp4')
        tem_texto = bool(peq or gra)
        a0, a1 = TEXTO_EM, max(TEXTO_EM + 0.5, dur - 0.6)
        filtros = (f'[0:v]scale={W}:{H},fps={FPS},setsar=1,'
                   # gradação: um pouco mais de contraste e vinheta (o motor entrega a imagem neutra)
                   f'eq=contrast=1.06:saturation=1.05,vignette=PI/5[b];')
        if tem_texto:
            filtros += (f'[1:v]format=rgba,fade=t=in:st={a0}:d=0.5:alpha=1,fade=t=out:st={a1}:d=0.5:alpha=1[t];'
                        f'[b][t]overlay=0:0:shortest=1[c];')
        else:
            filtros += '[b]null[c];'
        filtros += '[c][2:v]overlay=0:0:shortest=1,format=yuv420p[v]'
        rodar(['ffmpeg', '-y', '-loglevel', 'error'] + entrada +
              ['-loop', '1', '-t', str(dur), '-i', txt, '-loop', '1', '-t', str(dur), '-i', rotulo,
               '-filter_complex', filtros, '-map', '[v]', '-t', str(dur), '-c:v', 'libx264', '-crf', '15',
               '-preset', 'medium', '-r', str(FPS), out])
        partes.append((out, dur))
        agora += dur - FUSAO
    # cartão final
    cart = os.path.join(tmp, 'final.png')
    cartao_final(cart)
    out = os.path.join(tmp, 'final.mp4')
    rodar(['ffmpeg', '-y', '-loglevel', 'error', '-loop', '1', '-t', str(FINAL), '-i', cart, '-vf',
           f'fps={FPS},fade=t=in:st=0:d=1.2,fade=t=out:st={FINAL - 1.5}:d=1.5,format=yuv420p', '-c:v', 'libx264',
           '-crf', '15', '-r', str(FPS), out])
    cortes.append(('final', agora))
    partes.append((out, FINAL))
    total = agora + FINAL
    # fusões entre todas as partes (a primeira entra do preto)
    ins, filt, ult, desloc = [], '', '0:v', 0.0
    for k, (p, d) in enumerate(partes):
        ins += ['-i', p]
    for k in range(1, len(partes)):
        desloc += partes[k - 1][1] - FUSAO
        lab = f'x{k}'
        filt += f'[{ult}][{k}:v]xfade=transition=fade:duration={FUSAO}:offset={desloc:.3f}[{lab}];'
        ult = lab
    filt += f'[{ult}]fade=t=in:st=0:d=1.5[v]'
    musica = os.path.join(tmp, 'trilha.wav')
    trilha(total, cortes, musica)
    rodar(['ffmpeg', '-y', '-loglevel', 'error'] + ins + ['-i', musica, '-filter_complex', filt, '-map', '[v]', '-map',
          f'{len(partes)}:a', '-c:v', 'libx264', '-crf', '17', '-preset', 'slow', '-pix_fmt', 'yuv420p', '-c:a',
          'aac', '-b:a', '256k', '-movflags', '+faststart', '-t', f'{total:.3f}', a.saida])
    shutil.rmtree(tmp, ignore_errors=True)
    print(f'{a.saida}: {total:.1f} s, {os.path.getsize(a.saida) / 1048576:.1f} MB')


if __name__ == '__main__':
    main()
