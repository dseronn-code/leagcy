#!/usr/bin/env python3
"""Fotos de divulgação (site, launcher, repositório público): cada foto é uma cena do motor com a câmera parada num
ponto escolhido, renderizada sem janela no preset Cinema e salva em JPG. Uso (de engine/):

    python tools/fotos.py [--so ts_rua,eng_luzes] [--previa] [--saida resultados_pc/fotos]

--previa: 960x540 no preset Alto (conferir enquadramento rápido). As animações (multidão, telões) param no quadro 8
(--pause-at) para a imagem acumular sem rastro.
"""
import argparse
import math
import os
import subprocess
import sys
import time

from PIL import Image

TS = os.path.join('cenas', 'times_square.json')               # com multidão, trânsito e vapor
CIDADE = os.path.join('cenas', 'times_square', 'times_square.gltf')  # só a cidade (interiores)
BISTRO = os.path.join('assets', 'Bistro', 'bistro.gltf')
NOITE = ['--night', '--rain', '0.6', '--fog', '0.004']



def loja(x, z, yaw_graus, modo, lado_cam=1.0):
    """Câmera para uma loja da cidade (posição da porta e giro, do log de tools/times_square.py): 'porta' = da calçada
    olhando a entrada; 'dentro' = logo depois da porta, olhando o fundo na diagonal. lado_cam = -1: a câmera fica do
    outro lado da porta (quando o balcão está do lado padrão e taparia a foto)."""
    y = math.radians(yaw_graus)
    d = (math.sin(y), math.cos(y))   # para dentro da loja
    lado = (d[1] * lado_cam, -d[0] * lado_cam)
    if modo == 'porta':
        # entre a fileira de árvores da calçada e a fachada, um pouco de lado: a porta aberta e o interior aparecem
        cam = (x - d[0] * 2.4 + lado[0] * 2.0, 1.6, z - d[1] * 2.4 + lado[1] * 2.0)
        alvo = (x + d[0] * 2.5 - lado[0] * 0.5, 1.5, z + d[1] * 2.5 - lado[1] * 0.5)
    else:
        cam = (x + d[0] * 1.6 + lado[0] * 2.8, 1.6, z + d[1] * 1.6 + lado[1] * 2.8)
        alvo = (x + d[0] * 8.0 - lado[0] * 2.0, 1.0, z + d[1] * 8.0 - lado[1] * 2.0)
    return cam, alvo


# nome: (cena e argumentos, posição, alvo, fov, recorte final LxA ou None)
FOTOS = {
    'ts_hero': (['--scene', TS], (24.0, 9.0, 200.0), (18.0, 4.0, 150.0), 58, None),
    'ts_rua': (['--scene', TS], (19.2, 1.7, 183.0), (21.5, 6.0, 163.0), 62, None),
    'ts_praca': (['--scene', TS], (23.1, 1.7, 172.0), (18.1, 2.9, 152.6), 64, None),
    # lojas da cidade atual (tools/times_square.py imprime "tipo [x, y, z] giro")
    'ts_mobiliario': (['--scene', CIDADE] + NOITE, *loja(51.5, 150.4, 180, 'porta'), 72, None),
    'ts_lanchonete': (['--scene', CIDADE] + NOITE, *loja(51.5, 150.4, 180, 'dentro', -1), 70, None),
    'ts_loja': (['--scene', CIDADE] + NOITE, *loja(40.7, 208.3, 97, 'dentro'), 70, None),
    'ts_joalheria': (['--scene', CIDADE] + NOITE, *loja(106.6, -9.6, 180, 'dentro'), 70, None),
    'ts_conveniencia': (['--scene', CIDADE] + NOITE, *loja(70.1, -89.6, 180, 'dentro', -1), 70, None),
    'ts_cafe': (['--scene', CIDADE] + NOITE, *loja(-43.9, 169.6, 0, 'dentro'), 70, None),
    'ts_farmacia': (['--scene', CIDADE] + NOITE, *loja(-35.3, -89.6, 180, 'dentro'), 70, None),
    'eng_hero': (['--scene', BISTRO, '--night'], None, None, 0, None),
    'eng_luzes': (['--night'], None, None, 0, None),
    'eng_geometria': (['--scene', 'nanite', '--rochas', '16', '--rocha-segmentos', '768', '--sun-elevation', '14',
                       '--sun-azimuth', '200'], (-8.0, 38.0, 34.0), (3.0, 0.0, -80.0), 60, None),
    'eng_cabelo': (['--scene', 'hair'], None, None, 0, (1080, 1080)),
    'vale': (['--scene', os.path.join('cenas', 'vale', 'vale.json')], None, None, 0, None),
}


def achar_binario():
    for b in (os.path.join('build', 'Release', 'legacy.exe'), os.path.join('build', 'legacy.exe'),
              os.path.join('build', 'legacy')):
        if os.path.exists(b):
            return b
    sys.exit('Não achei o executável: compile antes.')


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--so')
    ap.add_argument('--previa', action='store_true')
    ap.add_argument('--saida', default=os.path.join('resultados_pc', 'fotos'))
    ap.add_argument('--quadros', type=int, default=64)
    a = ap.parse_args()
    b = achar_binario()
    os.makedirs(a.saida, exist_ok=True)
    w, h = (960, 540) if a.previa else (1920, 1080)
    for nome in (a.so.split(',') if a.so else FOTOS):
        cena, pos, alvo, fov, recorte = FOTOS[nome]
        # A Times Square inteira com multidão não cabe no preset Cinema numa placa de 8 GB: no Alto, com mais quadros.
        quality = 'alto' if a.previa or cena[1] == TS else 'cinema'
        png = os.path.join(a.saida, nome + '.png')
        args = [b, '--headless', '--width', str(w), '--height', str(h), '--quality', quality, '--frames',
                str(a.quadros), '--pause-at', '8', '--out', png] + cena
        if pos:
            cam = os.path.join(a.saida, nome + '.cam.txt')
            with open(cam, 'w') as f:  # câmera parada: o mesmo ponto no começo e no fim
                for t in (0, 100):
                    f.write(f'{t} {pos[0]} {pos[1]} {pos[2]} {alvo[0]} {alvo[1]} {alvo[2]} {fov}\n')
            args += ['--camera-path', cam, '--fps', '30']
        t0 = time.time()
        p = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if (p.returncode or not os.path.exists(png)) and quality == 'cinema':  # não coube na memória: tenta no Alto
            print(f'{nome}: o Cinema falhou (código {p.returncode}); de novo no Alto', flush=True)
            args[args.index('cinema')] = 'alto'
            p = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if p.returncode or not os.path.exists(png):
            print(f'{nome}: falhou (código {p.returncode})\n{p.stdout[-1500:]}{p.stderr[-1500:]}', flush=True)
            continue
        im = Image.open(png).convert('RGB')
        if recorte:
            rw, rh = recorte
            x0, y0 = (im.width - rw) // 2, (im.height - rh) // 2
            im = im.crop((x0, y0, x0 + rw, y0 + rh))
        jpg = os.path.join(a.saida, nome + '.jpg')
        im.save(jpg, quality=90, optimize=True, progressive=True)
        os.remove(png)
        print(f'{nome}: {time.time() - t0:.0f} s -> {jpg} ({os.path.getsize(jpg) / 1024:.0f} KB)', flush=True)


if __name__ == '__main__':
    main()
