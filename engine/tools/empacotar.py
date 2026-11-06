#!/usr/bin/env python3
"""Exporta um jogo feito no Legacy: uma pasta que roda sozinha, sem o código-fonte nem o build. Uso (de engine/):

    python tools/empacotar.py --cena cenas/carro.json --saida dist/Carro
    python tools/empacotar.py --cena cenas/parque.json --cena cenas/carro.json --saida dist/Demo --qualidade alto

O que vai para a pasta:
  - o executável e as bibliotecas ao lado dele (DLSS e outras .dll/.so do build);
  - os shaders compilados (o executável procura "shaders/" na própria pasta antes da pasta do build);
  - a fonte do HUD e os textos de licença (DLSS, Roboto e a tabela de licenças);
  - as cenas e tudo o que elas usam: modelos glTF (com .bin e texturas), scripts Lua, sons e os arquivos citados
    dentro dos scripts (strings com extensão conhecida que existem no disco);
  - jogar.bat (Windows) e jogar.sh (Linux): abrem a primeira cena em tela cheia.

Os caminhos ficam iguais aos da pasta do motor (cenas/..., assets/..., scripts/...), então a cena não muda. Na
primeira execução o motor comprime as texturas em texcache/ (as seguintes já começam rápido).
"""
import argparse
import glob
import json
import os
import re
import shutil
import stat
import sys

MOTOR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTENSOES = ('.gltf', '.glb', '.bin', '.png', '.jpg', '.jpeg', '.ktx2', '.dds', '.wav', '.mp3', '.flac', '.ogg',
             '.lua', '.json', '.txt')


def sem_comentarios(texto):
    """O motor aceita // e /* */ nas cenas; o json do Python não. Remove fora das strings."""
    saida, i, n, em_string = [], 0, len(texto), False
    while i < n:
        c = texto[i]
        if em_string:
            saida.append(c)
            if c == '\\' and i + 1 < n:
                saida.append(texto[i + 1])
                i += 1
            elif c == '"':
                em_string = False
        elif c == '"':
            em_string = True
            saida.append(c)
        elif texto.startswith('//', i):
            while i < n and texto[i] != '\n':
                i += 1
            continue
        elif texto.startswith('/*', i):
            fim = texto.find('*/', i + 2)
            i = n if fim < 0 else fim + 2
            continue
        else:
            saida.append(c)
        i += 1
    return ''.join(saida)


class Pacote:
    def __init__(self, saida):
        self.saida = saida
        self.arquivos = {}  # caminho relativo à pasta do motor -> origem absoluta
        self.avisos = []

    def relativo(self, caminho, *pastas):
        """Acha o arquivo como o motor acha (pasta do motor, depois a pasta de quem o cita); devolve o caminho
        relativo à pasta do motor ou None."""
        if os.path.isabs(caminho):
            return None
        for base in (MOTOR,) + pastas:
            p = os.path.normpath(os.path.join(base, caminho))
            if os.path.isfile(p):
                rel = os.path.relpath(p, MOTOR)
                return None if rel.startswith('..') else rel.replace('\\', '/')
        return None

    def adicionar(self, rel):
        novo = rel not in self.arquivos
        self.arquivos[rel] = os.path.join(MOTOR, rel)
        return novo

    def exigir(self, caminho, quem, *pastas):
        rel = self.relativo(caminho, *pastas)
        if rel is None:
            self.avisos.append(f'{quem}: "{caminho}" não encontrado dentro da pasta do motor')
            return None
        if self.adicionar(rel):
            self.seguir(rel)
        return rel

    def seguir(self, rel):
        """Inclui o que o arquivo cita: texturas e buffers de um .gltf, arquivos citados num .lua."""
        origem = os.path.join(MOTOR, rel)
        pasta = os.path.dirname(origem)
        if rel.endswith('.gltf'):
            with open(origem, encoding='utf-8') as f:
                g = json.load(f)
            for item in g.get('buffers', []) + g.get('images', []):
                uri = item.get('uri', '')
                if uri and not uri.startswith('data:'):
                    from urllib.parse import unquote
                    self.exigir(unquote(uri), rel, pasta)
        elif rel.endswith('.lua'):
            with open(origem, encoding='utf-8', errors='replace') as f:
                texto = f.read()
            for s in re.findall(r'"([^"\n]+)"|\'([^\'\n]+)\'', texto):
                s = s[0] or s[1]
                if s.lower().endswith(EXTENSOES) and self.relativo(s, pasta, os.path.dirname(pasta)):
                    self.exigir(s, rel, pasta, os.path.dirname(pasta))

    def cena(self, caminho):
        rel = self.relativo(caminho)
        if rel is None:
            sys.exit(f'Cena não encontrada dentro da pasta do motor: {caminho}')
        self.adicionar(rel)
        pasta = os.path.dirname(os.path.join(MOTOR, rel))
        with open(os.path.join(MOTOR, rel), encoding='utf-8') as f:
            doc = json.loads(sem_comentarios(f.read()))
        menu = doc.get('menu', 'scripts/menu_padrao.lua')  # menu de pausa: o padrão do motor, outro ou nenhum
        if isinstance(menu, str):
            self.exigir(menu, f'{rel} (menu)', pasta)
        for e in doc.get('entidades', []):
            quem = f'{rel} ({e.get("nome", "sem nome")})'
            if 'modelo' in e:
                self.exigir(e['modelo'], quem, pasta)
            if e.get('script'):
                self.exigir(e['script'], quem, pasta)
            if isinstance(e.get('som'), dict) and e['som'].get('arquivo'):
                self.exigir(e['som']['arquivo'], quem, pasta)
        return rel


def achar_binario(pedido):
    if pedido:
        return os.path.abspath(pedido)
    for c in ('build/Release/legacy.exe', 'build/legacy.exe', 'build/legacy', 'build2/legacy'):
        p = os.path.join(MOTOR, c)
        if os.path.isfile(p):
            return p
    sys.exit('Executável não encontrado: compile antes (cmake --build build --config Release) ou use --bin')


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description='Exporta um jogo do Legacy para uma pasta independente.')
    ap.add_argument('--cena', action='append', required=True, help='cena .json (repita para várias; a 1ª abre)')
    ap.add_argument('--saida', required=True, help='pasta do jogo exportado (apagada e refeita)')
    ap.add_argument('--bin', help='executável (padrão: o do build)')
    ap.add_argument('--qualidade', default='alto', help='baixo, medio, alto, ultra (padrão: alto)')
    ap.add_argument('--janela', action='store_true', help='abrir em janela em vez de tela cheia')
    ap.add_argument('--testar', action='store_true',
                    help='depois de exportar, roda o jogo exportado sem janela (de dentro da pasta dele) e confere')
    a = ap.parse_args()

    binario = achar_binario(a.bin)
    pasta_bin = os.path.dirname(binario)
    shaders = next((p for p in (os.path.join(pasta_bin, 'shaders'), os.path.join(os.path.dirname(pasta_bin), 'shaders'))
                    if os.path.isfile(os.path.join(p, 'lighting.comp.spv'))), None)
    if not shaders:
        sys.exit(f'Shaders compilados não encontrados perto de {binario}')

    saida = os.path.abspath(a.saida)
    if os.path.abspath(MOTOR).startswith(saida + os.sep) or saida == os.path.abspath(MOTOR):
        sys.exit('A pasta de saída não pode conter a pasta do motor')
    pac = Pacote(saida)
    cenas = [pac.cena(c) for c in a.cena]
    for extra in ('fontes/Roboto-Medium.ttf', 'fontes/LICENSE-Roboto.txt', 'docs/licencas.md'):
        if os.path.isfile(os.path.join(MOTOR, extra)):
            pac.adicionar(extra)

    if os.path.isdir(saida):
        shutil.rmtree(saida)
    os.makedirs(saida)
    exe = os.path.basename(binario)
    shutil.copy2(binario, os.path.join(saida, exe))
    bibliotecas = [p for padrao in ('*.dll', '*.so', '*.so.*') for p in glob.glob(os.path.join(pasta_bin, padrao))]
    for b in bibliotecas:
        shutil.copy2(b, saida)
    shutil.copytree(shaders, os.path.join(saida, 'shaders'), ignore=shutil.ignore_patterns('*.d', '*.tmp'))
    for rel, origem in sorted(pac.arquivos.items()):
        destino = os.path.join(saida, rel)
        os.makedirs(os.path.dirname(destino), exist_ok=True)
        shutil.copy2(origem, destino)
    # Licença do DLSS (vem no SDK baixado pelo build): obrigatória junto das bibliotecas do DLSS.
    if any('ngx' in os.path.basename(b).lower() for b in bibliotecas):
        candidatas = glob.glob(os.path.join(os.path.dirname(shaders), '_deps', 'ngx-src', 'LICENSE.txt')) + \
            sorted(glob.glob(os.path.join(MOTOR, 'build*', '_deps', 'ngx-src', 'LICENSE.txt')))
        if candidatas:
            shutil.copy2(candidatas[0], os.path.join(saida, 'LICENSE-DLSS.txt'))
        else:
            pac.avisos.append('bibliotecas do DLSS incluídas sem o LICENSE.txt do SDK (build*/_deps/ngx-src)')

    args = ['--scene', cenas[0], '--quality', a.qualidade] + ([] if a.janela else ['--fullscreen'])
    with open(os.path.join(saida, 'jogar.bat'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('@echo off\ncd /d "%~dp0"\n' + f'"{exe}" ' + ' '.join(x.replace('/', '\\') for x in args) + ' %*\n')
    sh = os.path.join(saida, 'jogar.sh')
    with open(sh, 'w', encoding='utf-8', newline='\n') as f:
        f.write('#!/bin/sh\ncd "$(dirname "$0")"\n' + f'exec ./{exe} ' + ' '.join(args) + ' "$@"\n')
    os.chmod(sh, os.stat(sh).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(saida) for f in fs)
    print(f'Jogo exportado em {saida}: {len(pac.arquivos)} arquivos do jogo, {len(bibliotecas)} bibliotecas, '
          f'{len(os.listdir(shaders))} shaders, {total / 1e6:.1f} MB')
    print('Para jogar: jogar.bat (Windows) ou ./jogar.sh (Linux)')
    for aviso in pac.avisos:
        print(f'AVISO: {aviso}')
    ok = not pac.avisos
    if a.testar:
        import subprocess
        p = subprocess.run([os.path.join(saida, exe), '--headless', '--scene', cenas[0], '--width', '160', '--height',
                            '90', '--frames', '30', '--out', 'teste.png'], cwd=saida, capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
        log = p.stdout + p.stderr
        do_pacote = 'Shaders do pacote' in log
        imagem = os.path.isfile(os.path.join(saida, 'teste.png'))
        passou = p.returncode == 0 and do_pacote and imagem
        print(f'teste do jogo exportado {"PASSOU" if passou else "FALHOU"}: código {p.returncode}, shaders do pacote: '
              f'{do_pacote}, imagem gravada: {imagem}')
        if not passou:
            print(log[-1500:])
        ok &= passou
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
