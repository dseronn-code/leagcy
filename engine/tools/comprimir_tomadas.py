#!/usr/bin/env python3
"""Recupera as tomadas do trailer da última rodada e recomprime para caberem no envio (limite de 100 MB por arquivo do
GitHub). O agente apaga resultados_pc/ no começo de cada rodada, mas a cópia do último envio fica em
%TEMP%/legacy_resultados_rtx/trailer. Uso: python tools/comprimir_tomadas.py"""
import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trailer import achar_ffmpeg  # noqa: E402

origens = [os.path.join('resultados_pc', 'trailer'),
           os.path.join(os.environ.get('TEMP', '/tmp'), 'legacy_resultados_rtx', 'trailer')]
saida = os.path.join('resultados_pc', 'trailer')
os.makedirs(saida, exist_ok=True)
ffmpeg = achar_ffmpeg()
achados = {}
for o in origens:
    for f in glob.glob(os.path.join(o, '*.mp4')):
        achados.setdefault(os.path.basename(f), f)
if not achados:
    sys.exit('Nenhuma tomada encontrada em: ' + ', '.join(origens))
for nome, f in sorted(achados.items()):
    destino = os.path.join(saida, nome)
    mb = os.path.getsize(f) / 1048576
    if mb < 60 and os.path.abspath(f) == os.path.abspath(destino):
        print(f'{nome}: {mb:.1f} MB (fica como está)')
        continue
    for crf in (20, 24, 28, 32):
        tmp = destino + '.tmp.mp4'
        subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-i', f, '-c:v', 'libx264', '-crf', str(crf), '-preset',
                        'slow', '-pix_fmt', 'yuv420p', tmp], check=True)
        if os.path.getsize(tmp) < 60 * 1048576:
            break
    os.replace(tmp, destino)
    print(f'{nome}: {mb:.1f} MB -> {os.path.getsize(destino) / 1048576:.1f} MB (crf {crf})', flush=True)
