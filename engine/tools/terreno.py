#!/usr/bin/env python3
"""Testes do pincel de terreno (editor e Lua: terreno_pincel / salvar_terreno). Uso (de engine/):

    python3 tools/terreno.py [--bin build/legacy] [--so subir,...]

Terreno plano de 128 m (ladrilhos de 32 m). Um script pinta em t = 0,5 s e registra alturas; uma bola cai sobre o
ponto pintado.
  - subir:     "subir" 3 m com raio 5 em (10, 10): a altura lá vira 3, a 5 m fica 0, e a bola para em cima do morro
               (y = 3,25: a colisão foi refeita).
  - modos:     "aplanar" leva o centro à altura pedida (1 m); "suavizar" baixa o pico de um morro estreito.
  - salvar:    salvar_terreno grava o arquivo; uma cena nova com ele de "mapa" tem o morro no mesmo lugar.
  - imagem:    a imagem muda onde o morro subiu (e é igual fora dele).
  - editor:    no editor (janela), o painel Terreno esculpe com o mouse (aqui, pinceladas automáticas no centro).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

FPS = 30


def rodar(b, pasta, nome, lua, terreno=None, segundos=3.0, camera=None, tamanho=(32, 18)):
    arq_lua = os.path.join(pasta, f'{nome}.lua')
    with open(arq_lua, 'w', encoding='utf-8') as f:
        f.write(lua)
    t = terreno or {'tamanho': 128, 'ladrilho': 32, 'altura': 0, 'resolucao': 32}
    c = {'menu': False, 'terreno': t,
         'entidades': [{'nome': 'bola', 'forma': 'esfera', 'raio': 0.25, 'posicao': [10, 8, 10], 'fisica': {'tipo': 'dinamico'}},
                       {'nome': 'diretor', 'script': arq_lua}]}
    if camera:
        c['camera'] = camera
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(c, f)
    out = os.path.join(pasta, f'{nome}.png')
    p = subprocess.run([b, '--headless', '--scene', arq, '--width', str(tamanho[0]), '--height', str(tamanho[1]),
                        '--frames', str(int(segundos * FPS)), '--fps', str(FPS), '--no-motion-blur', '--out', out],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    vals = {}
    for l in (p.stdout + p.stderr).splitlines():
        if '[script] T ' in l:
            k, v = l.split('[script] T ', 1)[1].split()
            vals[k] = float(v)
    return vals, out


def teste_subir(b, pasta):
    lua = '''
function atualizar(e, dt)
  if tempo() >= 0.5 and not feito then
    feito = true
    terreno_pincel(10, 10, 5, 3, "subir")
    log("T", "centro " .. altura_terreno(10, 10)); log("T", "borda " .. altura_terreno(15.5, 10))
  end
  if tempo() >= 2.9 and not fim then fim = true; local _, y = achar("bola"):posicao(); log("T", "bola " .. y) end
end
'''
    v, _ = rodar(b, pasta, 'subir', lua)
    ok = abs(v.get('centro', 0) - 3) < 0.05 and abs(v.get('borda', 9)) < 1e-3 and abs(v.get('bola', 0) - 3.25) < 0.08
    return ok, f'altura no centro {v.get("centro")} (3), a 5,5 m {v.get("borda")} (0); bola parou em y = {v.get("bola")} (3,25)'


def teste_modos(b, pasta):
    lua = '''
function atualizar(e, dt)
  if tempo() >= 0.5 and not feito then
    feito = true
    terreno_pincel(-20, -20, 6, 1, "aplanar")
    log("T", "plano " .. altura_terreno(-20, -20))
    terreno_pincel(20, -20, 2, 4, "subir")
    log("T", "pico " .. altura_terreno(20, -20))
    for i = 1, 5 do terreno_pincel(20, -20, 4, 1, "suavizar") end
    log("T", "suave " .. altura_terreno(20, -20))
  end
end
'''
    v, _ = rodar(b, pasta, 'modos', lua, segundos=1.0)
    ok = abs(v.get('plano', 0) - 1) < 0.01 and v.get('pico', 0) > 3.9 and v.get('suave', 9) < 0.7 * v.get('pico', 0)
    return ok, f'aplanar a 1 m: {v.get("plano")}; pico {v.get("pico")} -> suavizado {v.get("suave")}'


def teste_salvar(b, pasta):
    arq = os.path.join(pasta, 'morro.altura').replace('\\', '/')
    lua = f'''
function atualizar(e, dt)
  if tempo() >= 0.5 and not feito then
    feito = true
    terreno_pincel(10, 10, 5, 3, "subir")
    log("T", "salvo " .. (salvar_terreno("{arq}") and 1 or 0))
  end
end
'''
    v1, _ = rodar(b, pasta, 'salvar1', lua, segundos=1.0)
    lua2 = 'function atualizar(e, dt) if not feito then feito = true; log("T", "lido " .. altura_terreno(10, 10)); ' \
           'log("T", "longe " .. altura_terreno(-30, 30)) end end\n'
    v2, _ = rodar(b, pasta, 'salvar2', lua2, terreno={'mapa': arq, 'ladrilho': 32, 'resolucao': 32}, segundos=0.5)
    ok = v1.get('salvo') == 1 and abs(v2.get('lido', 0) - 3) < 0.05 and abs(v2.get('longe', 9)) < 1e-3
    return ok, f'salvo: {v1.get("salvo") == 1}; cena nova com o mapa: altura {v2.get("lido")} no morro (3), {v2.get("longe")} longe'


def teste_imagem(b, pasta):
    import numpy as np
    from PIL import Image
    cam = {'posicao': [10, 6, 25], 'alvo': [10, 1, 10]}
    sem = 'function atualizar(e, dt) end\n'
    com = 'function atualizar(e, dt) if not feito then feito = true; terreno_pincel(10, 10, 6, 4, "subir") end end\n'
    _, a = rodar(b, pasta, 'img_sem', sem, segundos=1.0, camera=cam, tamanho=(160, 90))
    _, c = rodar(b, pasta, 'img_com', com, segundos=1.0, camera=cam, tamanho=(160, 90))
    x = np.asarray(Image.open(a).convert('RGB')).astype(float)
    y = np.asarray(Image.open(c).convert('RGB')).astype(float)
    d = np.abs(x - y).max(2)
    meio = d[30:70, 50:110].mean()
    canto = d[80:, :20].mean()
    return meio > 10 and canto < 3, f'diferença onde o morro subiu {meio:.1f}/255, num canto longe {canto:.1f}/255'


def teste_editor(b, pasta):
    """Editor (janela; no Linux sem tela precisa do xvfb-run): pinceladas no centro da tela pelo painel Terreno."""
    import shutil
    import numpy as np
    from PIL import Image
    if sys.platform.startswith('linux') and not os.environ.get('DISPLAY') and not shutil.which('xvfb-run'):
        return True, 'sem tela nem xvfb-run: pulado'
    cena_ = {'menu': False, 'camera': {'posicao': [0, 25, 40], 'alvo': [0, 0, 0]},
             'terreno': {'tamanho': 128, 'ladrilho': 32, 'altura': 0, 'resolucao': 32}, 'entidades': []}
    arq = os.path.join(pasta, 'editor.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(cena_, f)
    telas = {}
    for nome, env in (('sem', {}), ('com', {'LEGACY_EDITOR_TESTE': 'terreno'})):
        tela = os.path.join(pasta, f'editor_{nome}.png')
        cmd = [b, '--editor', '--scene', arq, '--width', '480', '--height', '270', '--quit-after', '6', '--screenshot', tela]
        if sys.platform.startswith('linux') and not os.environ.get('DISPLAY'):
            cmd = ['xvfb-run', '-a', '-s', '-screen 0 1280x720x24'] + cmd
        subprocess.run(cmd, capture_output=True, timeout=600, env=dict(os.environ, **env), cwd=pasta)
        telas[nome] = np.asarray(Image.open(tela).convert('RGB')).astype(float)
    d = np.abs(telas['sem'] - telas['com']).max(2)
    h, w = d.shape
    centro = d[int(h * 0.45):int(h * 0.75), int(w * 0.35):int(w * 0.65)].mean()
    chao = d[int(h * 0.85):, int(w * 0.45):int(w * 0.55)].mean()  # perto da câmera, fora do pincel
    return centro > 5 and chao < 2, (f'diferença no centro da tela depois das pinceladas: {centro:.1f}/255 (o morro '
                                     f'subiu); fora do pincel {chao:.1f}/255')


TESTES = {'subir': teste_subir, 'modos': teste_modos, 'salvar': teste_salvar, 'imagem': teste_imagem, 'editor': teste_editor}


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
            print(f'{n:<7} {"PASSOU" if ok else "FALHOU"}: {msg}', flush=True)
    print(f'PASSOU {len(nomes) - falhas} FALHOU {falhas}')
    sys.exit(1 if falhas else 0)


if __name__ == '__main__':
    main()
