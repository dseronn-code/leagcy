#!/usr/bin/env python3
"""Testes dos idiomas (t(), idioma(), idiomas() e idiomas/*.json). Uso (de engine/):

    python3 tools/idiomas.py [--bin build/legacy]

Roda sem janela numa pasta vazia (as traduções vêm de idiomas/ ao lado do motor, achadas pelo executável):
  - traduz:   t("Continuar") em inglês, espanhol e português; {q} trocado; texto sem tradução sai como está.
  - lista:    idiomas() lista os três com os nomes ("English", "Español", "Português").
  - escolha:  idioma("es") num jogo fica guardado e vale no próximo; --idioma e "idioma" da cena na ordem certa.
  - menu:     o menu de pausa padrão mostra "Continue" em inglês (imagem com janela, xvfb no Linux): a primeira linha
              de botão tem a largura do texto inglês, diferente da do português.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

MOTOR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

LUA = '''
function atualizar(e, dt)
  if feito then return end
  feito = true
  log("IDIOMA", idioma())
  log("T1", t("Continuar"))
  log("T2", t("Qualidade: {q}", {q = t("Alta")}))
  log("T3", t("Texto sem tradução nenhuma"))
  log("T4", t("Missão concluída! +{n} moedas", {n = 20}))
  local nomes = {}
  for _, id in ipairs(idiomas()) do nomes[#nomes + 1] = id[1] .. "=" .. id[2] end
  log("LISTA", table.concat(nomes, ","))
  %s
end
'''


def rodar(b, pasta, nome, extra_lua='', args=(), idioma_cena=None):
    lua = os.path.join(pasta, f'{nome}.lua')
    with open(lua, 'w', encoding='utf-8') as f:
        f.write(LUA % extra_lua)
    cena = {'entidades': [{'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [5, 0.5, 5], 'posicao': [0, -0.5, 0],
                           'fisica': {'tipo': 'estatico'}},
                          {'nome': 'teste', 'forma': 'esfera', 'raio': 0.01, 'posicao': [0, -50, 0], 'script': lua}],
            'menu': False}
    if idioma_cena:
        cena['idioma'] = idioma_cena
    arq = os.path.join(pasta, f'{nome}.json')
    with open(arq, 'w', encoding='utf-8') as f:
        json.dump(cena, f, ensure_ascii=False)
    p = subprocess.run([b, '--headless', '--scene', arq, '--width', '32', '--height', '18', '--frames', '3',
                        '--out', os.path.join(pasta, f'{nome}.png'), *args], capture_output=True, text=True,
                       encoding='utf-8', errors='replace', cwd=pasta)
    if p.returncode:
        raise RuntimeError(f'motor falhou ({p.returncode}): {p.stderr[-400:]}')
    out = {}
    for l in (p.stdout + p.stderr).splitlines():
        if '[script] ' in l:
            k, _, v = l.split('[script] ', 1)[1].strip().partition(' ')
            out[k] = v.strip()
    return out


def teste_traduz(b, pasta):
    en = rodar(b, pasta, 'en', args=('--idioma', 'en'))
    es = rodar(b, pasta, 'es', args=('--idioma', 'es'))
    pt = rodar(b, pasta, 'pt', args=('--idioma', 'pt'))
    ok = (en['T1'] == 'Continue' and es['T1'] == 'Continuar' and pt['T1'] == 'Continuar' and
          en['T2'] == 'Quality: High' and es['T2'] == 'Calidad: Alta' and pt['T2'] == 'Qualidade: Alta' and
          en['T3'] == 'Texto sem tradução nenhuma' and es['T4'] == '¡Misión cumplida! +20 monedas')
    return ok, f'en: "{en["T1"]}", "{en["T2"]}"; es: "{es["T2"]}", "{es["T4"]}"; sem tradução: "{en["T3"]}"'


def teste_lista(b, pasta):
    r = rodar(b, pasta, 'lista')
    return r['LISTA'] == 'en=English,es=Español,pt=Português', f'idiomas(): {r["LISTA"]}'


def teste_escolha(b, pasta):
    sub = os.path.join(pasta, 'escolha')
    os.makedirs(sub)
    a = rodar(b, sub, 'a', idioma_cena='en')                                    # só a cena: en
    c = rodar(b, sub, 'c', extra_lua='idioma("es")', idioma_cena='en')           # o jogador escolhe es
    d = rodar(b, sub, 'd', idioma_cena='en')                                    # a escolha guardada vence a cena
    e = rodar(b, sub, 'e', args=('--idioma', 'pt'), idioma_cena='en')           # --idioma vence tudo
    guardado = open(os.path.join(sub, 'saves', 'idioma.txt'), encoding='utf-8').read().strip()
    ok = a['IDIOMA'] == 'en' and c['IDIOMA'] == 'en' and d['IDIOMA'] == 'es' and e['IDIOMA'] == 'pt' and guardado == 'es'
    return ok, (f'cena en: {a["IDIOMA"]}; depois de idioma("es") (guardado "{guardado}"): {d["IDIOMA"]}; '
                f'com --idioma pt: {e["IDIOMA"]}')


def teste_menu(b, pasta):
    from PIL import Image
    larguras = {}
    for lang in ('pt', 'en'):
        cena = {'camera': {'posicao': [0, 2, 8], 'alvo': [0, 0, 0]},
                'entidades': [{'nome': 'chao', 'forma': 'caixa', 'meia_extensao': [5, 0.5, 5], 'posicao': [0, -0.5, 0],
                               'fisica': {'tipo': 'estatico'}}]}
        arq = os.path.join(pasta, f'menu_{lang}.json')
        with open(arq, 'w', encoding='utf-8') as f:
            json.dump(cena, f)
        tela = os.path.join(pasta, f'menu_{lang}.png')
        cmd = [b, '--scene', arq, '--width', '640', '--height', '360', '--quit-after', '2', '--screenshot', tela,
               '--idioma', lang, '--simular', '0.2:tecla:esc']
        if sys.platform.startswith('linux') and not os.environ.get('DISPLAY'):
            cmd = ['xvfb-run', '-a'] + cmd
        # Da pasta do motor: a fonte do HUD e o menu padrão (scripts/menu_padrao.lua) são achados por ela.
        subprocess.run(cmd, check=True, capture_output=True, cwd=MOTOR)
        img = Image.open(tela).convert('RGB')
        w, h = img.size
        # Texto claro dentro do 1º botão (y 0.32..0.385): largura ocupada pelas letras.
        xs = [x for x in range(int(0.34 * w), int(0.66 * w)) for y in range(int(0.335 * h), int(0.37 * h))
              if min(img.getpixel((x, y))) > 200]
        larguras[lang] = (max(xs) - min(xs)) / w if xs else 0.0
    # "Continuar" (9 letras) é mais largo que "Continue" (8).
    ok = larguras['pt'] > 0 and larguras['en'] > 0 and larguras['en'] < 0.95 * larguras['pt']
    return ok, f'largura do texto do 1º botão: pt {larguras["pt"]:.3f}, en {larguras["en"]:.3f} (fração da tela)'


TESTES = {'traduz': teste_traduz, 'lista': teste_lista, 'escolha': teste_escolha, 'menu': teste_menu}


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
    with tempfile.TemporaryDirectory() as pasta:
        for n in nomes:
            try:
                ok, msg = TESTES[n](a.bin, pasta)
            except Exception as ex:  # noqa: BLE001
                ok, msg = False, f'erro: {ex}'
            falhas += not ok
            print(f'{n:<8} {"PASSOU" if ok else "FALHOU"}: {msg}', flush=True)
    print(f'PASSOU {len(nomes) - falhas} FALHOU {falhas}')
    sys.exit(1 if falhas else 0)


if __name__ == '__main__':
    main()
