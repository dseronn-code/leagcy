#!/usr/bin/env python3
"""Renderiza as tomadas do trailer do Legacy Engine e transforma cada uma num vídeo .mp4. Uso (de dentro de engine/):

    python tools/trailer.py [--previa] [--so oceano,pele] [--quality cinema] [--fps 30]

Cada tomada é uma cena do motor com uma trajetória de câmera (trailer/*.txt, ver --camera-path) ou um movimento reto
(--move). O motor grava os quadros em PNG (resultados_pc/trailer/<tomada>/), o ffmpeg junta num .mp4
(resultados_pc/trailer/<tomada>.mp4) e os PNG são apagados para o envio ficar pequeno.

--previa: 960x540 e qualidade Alto (rápido, para conferir enquadramento); sem ela, 1920x1080 no modo cinema.
O ffmpeg vem do PATH ou do pacote imageio-ffmpeg (instalado sozinho com pip se faltar).
Quadros renderizados offline: no trailer, a legenda é "capturado no Legacy Engine", não "tempo real".
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

BISTRO = os.path.join('assets', 'Bistro', 'bistro.gltf')

# nome: (segundos, argumentos da cena, trajetória em trailer/ ou None, velocidade --move em m/s)
TOMADAS = {
    'oceano': (8, ['--scene', 'ocean', '--sun-elevation', '4', '--sun-azimuth', '15'], 'oceano.txt', 0.0),
    'nuvens': (7, ['--scene', 'outdoor', '--sun-elevation', '50', '--clouds', '0.6'], 'nuvens.txt', 0.0),
    'sponza_luz': (8, ['--fog', '0.02'], 'sponza_luz.txt', 0.0),
    'cidade_chuva': (7, ['--scene', 'city', '--night', '--rain', '0.6'], None, 2.0),
    'pintura': (7, ['--scene', 'paint'], 'pintura.txt', 0.0),
    'pele': (6, ['--scene', 'scan-backlit'], 'pele.txt', 0.0),
    'multidao': (7, ['--scene', 'crowd'], 'multidao.txt', 0.0),
    'bistro_noite': (8, ['--scene', BISTRO, '--night'], None, 1.0),
    'vfx': (6, ['--scene', 'vfx'], 'vfx.txt', 0.0),
    # Parte 3: caminhada pela Times Square (cenas/times_square, tools/times_square.py), à noite, com chuva.
    'times_square': (30, ['--scene', os.path.join('cenas', 'times_square', 'times_square.gltf'), '--night', '--rain',
                          '0.8', '--fog', '0.004'], 'times_square.txt', 0.0),
    # A cena jogável com a multidão (cenas/times_square.json: clima, 2.000 pessoas, vapor nos bueiros).
    'times_square_multidao': (15, ['--scene', os.path.join('cenas', 'times_square.json')], 'times_square_multidao.txt', 0.0),
}


def achar_binario():
    for b in (os.path.join('build', 'Release', 'legacy.exe'), os.path.join('build', 'legacy.exe'),
              os.path.join('build', 'legacy')):
        if os.path.exists(b):
            return b
    sys.exit('Não achei o executável: compile antes (cmake --build build --config Release).')


def achar_ffmpeg():
    f = shutil.which('ffmpeg')
    if f:
        return f
    try:
        import imageio_ffmpeg  # noqa: PLC0415
    except ImportError:
        print('ffmpeg não encontrado: instalando imageio-ffmpeg (traz o ffmpeg)...', flush=True)
        subprocess.run([sys.executable, '-m', 'pip', 'install', '--quiet', 'imageio-ffmpeg'], check=True)
        import imageio_ffmpeg  # noqa: PLC0415
    return imageio_ffmpeg.get_ffmpeg_exe()


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--previa', action='store_true', help='960x540, qualidade Alto (conferir enquadramento)')
    ap.add_argument('--so', help='só estas tomadas (separadas por vírgula)')
    ap.add_argument('--quality', help='preset do motor (padrão: cinema; na prévia, alto)')
    ap.add_argument('--fps', type=int, default=30)
    ap.add_argument('--tamanho', help='LxA (teste: substitui a resolução)')
    ap.add_argument('--aquecimento', type=int, default=30, help='quadros parados antes de gravar')
    ap.add_argument('--saida', default=os.path.join('resultados_pc', 'trailer'))
    a = ap.parse_args()
    binario = achar_binario()
    ffmpeg = achar_ffmpeg()
    w, h = (960, 540) if a.previa else (1920, 1080)
    if a.tamanho:
        w, h = (int(x) for x in a.tamanho.lower().split('x'))
    quality = a.quality or ('alto' if a.previa else 'cinema')
    nomes = a.so.split(',') if a.so else list(TOMADAS)
    os.makedirs(a.saida, exist_ok=True)
    for nome in nomes:
        segundos, cena, caminho, mover = TOMADAS[nome]
        pasta = os.path.join(a.saida, nome)
        shutil.rmtree(pasta, ignore_errors=True)
        os.makedirs(pasta)
        quadros = segundos * a.fps
        aquecimento = a.aquecimento
        args = [binario, '--headless', '--width', str(w), '--height', str(h), '--quality', quality,
                '--fps', str(a.fps), '--warmup', str(aquecimento), '--frames', str(aquecimento + quadros),
                '--save-every', '1', '--out', os.path.join(pasta, 'f.png')] + cena
        if caminho:
            args += ['--camera-path', os.path.join('trailer', caminho)]
        if mover:
            args += ['--move', str(mover)]
        t0 = time.time()
        p = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if p.returncode:
            print(f'{nome}: o motor falhou (código {p.returncode})\n{p.stdout[-2000:]}{p.stderr[-2000:]}', flush=True)
            continue
        mp4 = os.path.join(a.saida, f'{nome}.mp4')
        enc = subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-framerate', str(a.fps), '-i',
                              os.path.join(pasta, 'f_%04d.png'), '-c:v', 'libx264', '-crf', '17', '-preset', 'slow',
                              '-pix_fmt', 'yuv420p', mp4], capture_output=True, text=True)
        if enc.returncode:
            print(f'{nome}: ffmpeg falhou\n{enc.stderr[-2000:]}', flush=True)
            continue
        # Guarda um quadro do meio (conferência rápida) e apaga a sequência.
        meio = sorted(glob.glob(os.path.join(pasta, 'f_*.png')))
        if meio:
            shutil.copy(meio[len(meio) // 2], os.path.join(a.saida, f'{nome}_meio.png'))
        shutil.rmtree(pasta, ignore_errors=True)
        mb = os.path.getsize(mp4) / 1048576.0
        print(f'{nome}: {quadros} quadros em {time.time() - t0:.0f} s -> {mp4} ({mb:.1f} MB)', flush=True)


if __name__ == '__main__':
    main()
