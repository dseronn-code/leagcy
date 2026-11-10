#!/usr/bin/env python3
"""Teste do jogo de demonstração "Vale dos Cristais" (cenas/vale). Uso (de engine/):

    python3 tools/demo.py [--bin build/legacy] [--so abertura,...]

Joga sozinho: a cena original com um script de teste a mais, que depois da abertura leva o jogador para perto do
guarda (e aperta F), depois para a frente dele e por fim até cada cristal.
  - abertura:  a cena de abertura (linha do tempo) toca ao começar e termina em ~7 s.
  - cristais:  os 5 cristais (scripts visuais) são coletados quando o jogador chega perto; com os 5, a cena de vitória
               toca e termina.
  - guarda:    F perto do guarda o derruba (ragdoll ativo) e ele levanta 3 s depois.
  - persegue:  com o jogador à vista, o guarda (IA) passa a persegui-lo.
  - imagem:    a imagem do jogo tem terreno, céu e os cristais azuis.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

FPS = 30
TESTE = '''
local CRISTAIS = {"cristal_1", "cristal_2", "cristal_3", "cristal_4", "cristal_5"}
function atualizar(e, dt)
  local t = tempo()
  local jog, g = achar("jogador"), achar("guarda")
  -- 8 s: a 2,2 m do guarda (o F é simulado em 8,1 s, antes de ele alcançar); 12,5 s: 6 m na frente dele (para onde ele anda).
  if t >= 8.0 and not perto then
    perto = true
    local gx, gy, gz = g:posicao()
    jog:mover_para(gx + 2.2, altura_terreno(gx + 2.2, gz) + 0.95, gz)
  end
  if t >= 12.5 and not frente then
    frente = true
    local gx, gy, gz = g:posicao()
    local vx, vy, vz = g:velocidade()
    local n = math.sqrt(vx * vx + vz * vz)
    if n < 0.1 then vx, vz, n = 0, 1, 1 end
    local px, pz = gx + vx / n * 6, gz + vz / n * 6
    jog:mover_para(px, altura_terreno(px, pz) + 0.95, pz)
  end
  if t >= 12.6 and t < 14.5 then log("IA", t, g:estado_ia()) end
  for i, nome in ipairs(CRISTAIS) do
    if t >= 15.0 + (i - 1) * 1.2 and not feito[i] then
      feito[i] = true
      local c = achar(nome)
      local x, y, z = c:posicao()
      if y > -100 then jog:mover_para(x + 0.5, y, z) end  -- já coletado: fica onde está
    end
  end
end
feito = {}
'''


def preparar(pasta):
    base = os.path.abspath(os.path.join('cenas', 'vale'))
    with open(os.path.join(base, 'vale.json'), encoding='utf-8') as f:
        texto = f.read()
    cena = json.loads(re.sub(r'^\s*//.*$', '', texto, flags=re.M))
    for e in cena['entidades']:
        v = e.get('personagem', {}).get('visual')
        if v:
            v['modelo'] = os.path.normpath(os.path.join(base, v['modelo']))
        if 'script_visual' in e:
            e['script_visual'] = os.path.join(base, e['script_visual'])
        if 'script' in e:
            e['script'] = os.path.join(base, e['script'])
    lua = os.path.join(pasta, 'teste.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write(TESTE)
    cena['entidades'].append({'nome': 'teste', 'script': lua, 'sempre_carregado': True})
    arq = os.path.join(pasta, 'vale_teste.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(cena, f)
    return arq


_cache = {}


def sessao(b, pasta):
    if 'log' not in _cache:
        arq = preparar(pasta)
        out = os.path.join(pasta, 'vale.png')
        p = subprocess.run([b, '--headless', '--scene', arq, '--width', '320', '--height', '180', '--fps', str(FPS),
                            '--frames', str(28 * FPS), '--no-motion-blur', '--out', out, '--simular', '8.1:tecla:f'],
                           capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=3000)
        if p.returncode:
            raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
        _cache['log'] = (p.stdout + p.stderr, out)
    return _cache['log']


def quando(log, texto):
    """Tempo de jogo (pelo último registro "IA"/"quadro") em que `texto` apareceu: aproximado pela ordem das linhas."""
    t = 0.0
    for l in log.splitlines():
        m = re.search(r'frame (\d+)/', l)
        if m:
            t = int(m.group(1)) / FPS
        if texto in l:
            return t
    return None


def teste_abertura(b, pasta):
    log, _ = sessao(b, pasta)
    fim = [l for l in log.splitlines() if 'fim abertura' in l]
    return bool(fim), f'cena de abertura terminou: {bool(fim)}'


def teste_cristais(b, pasta):
    log, _ = sessao(b, pasta)
    coletados = [l for l in log.splitlines() if 'cristal coletado' in l]
    vitoria = 'VALE vitoria' in log
    fim = 'fim vitoria' in log
    ok = len(coletados) == 5 and vitoria and fim
    return ok, f'{len(coletados)} cristais coletados; vitória: {vitoria}; cena de vitória terminou: {fim}'


def teste_guarda(b, pasta):
    log, _ = sessao(b, pasta)
    caiu, levantou = 'guarda derrubado' in log, 'guarda levantou' in log
    return caiu and levantou, f'derrubado com F: {caiu}; levantou depois de 3 s: {levantou}'


def teste_persegue(b, pasta):
    log, _ = sessao(b, pasta)
    estados = [l.split('[script] IA ', 1)[1].split()[-1] for l in log.splitlines() if '[script] IA ' in l]
    ok = 'perseguindo' in estados or 'pego pelo guarda' in log
    return ok, f'estados do guarda com o jogador na frente: {sorted(set(estados))}'


def teste_imagem(b, pasta):
    import numpy as np
    from PIL import Image
    _, png = sessao(b, pasta)
    img = np.asarray(Image.open(png).convert('RGB')).astype(float)
    h = img.shape[0]
    ceu = img[: h // 5].mean(0).mean(0)
    chao = img[h * 3 // 4:].mean(0).mean(0)
    ok = ceu[2] > ceu[0] and img.std() > 10
    return ok, f'céu médio {tuple(int(v) for v in ceu)}, chão médio {tuple(int(v) for v in chao)}, contraste {img.std():.1f}'


TESTES = {'abertura': teste_abertura, 'cristais': teste_cristais, 'guarda': teste_guarda, 'persegue': teste_persegue,
          'imagem': teste_imagem}


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
            print(f'{n:<9} {"PASSOU" if ok else "FALHOU"}: {msg}', flush=True)
    print(f'PASSOU {len(nomes) - falhas} FALHOU {falhas}')
    sys.exit(1 if falhas else 0)


if __name__ == '__main__':
    main()
