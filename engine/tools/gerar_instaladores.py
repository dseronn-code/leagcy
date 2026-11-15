#!/usr/bin/env python3
"""Gera os instaladores do Legacy no PC Windows (pelo agente da RTX, sem GitHub Actions) e publica para download.
Uso (de engine/, depois de compilar o Release), uma etapa por linha do pedido (cada uma cabe nos 20 min do agente):

    python tools\\gerar_instaladores.py --jogos      # exporta Times Square e Legacy Engine e monta os 2 instaladores
    python tools\\gerar_instaladores.py --launcher   # monta o instalador do Legacy Launcher (Electron)
    python tools\\gerar_instaladores.py --publicar   # cria uma Release no GitHub e sobe os 3 instaladores

Sai em ../dist/instaladores/: Legacy-Setup.exe, TimesSquare-Setup.exe, LegacyEngine-Setup.exe (nomes fixos: o launcher
baixa de https://github.com/<dono>/<repo>/releases/latest/download/<nome>). O Inno Setup e o Node são baixados e
instalados só para o usuário (sem administrador) na primeira vez. A publicação usa o login do git do próprio PC
(`git credential fill`), o mesmo que o agente usa para enviar os resultados.
"""
import argparse
import gzip
import hashlib
import glob
import json
import os
import shutil
import time
import subprocess
import sys
import urllib.parse
import urllib.request
import zipfile

MOTOR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAIZ = os.path.dirname(MOTOR)
DIST = os.path.join(RAIZ, 'dist')
SAIDA = os.path.join(DIST, 'instaladores')
LOCAL = os.environ.get('LOCALAPPDATA', os.path.expanduser('~'))
NODE_URL = 'https://nodejs.org/dist/v22.11.0/node-v22.11.0-win-x64.zip'
REPO = 'dseronn-code/leagcy'


def rodar(cmd, **kw):
    print('>', ' '.join(cmd) if isinstance(cmd, list) else cmd, flush=True)
    subprocess.run(cmd, check=True, **kw)


def baixar(url, destino):
    print('baixando', url, flush=True)
    with urllib.request.urlopen(url, timeout=600) as r, open(destino, 'wb') as f:
        shutil.copyfileobj(r, f)


def versao():
    n = subprocess.run(['git', 'rev-list', '--count', 'HEAD'], cwd=RAIZ, capture_output=True, text=True).stdout.strip()
    return f'0.1.{n or 0}'


INNO_URLS = ['https://jrsoftware.org/download.php/is.exe', 'https://files.jrsoftware.org/is/6/innosetup-6.4.3.exe',
             'https://github.com/jrsoftware/issrc/releases/download/is-6_4_3/innosetup-6.4.3.exe']
ISCC_LOCAIS = [os.path.join(LOCAL, 'InnoSetup6', 'ISCC.exe'), os.path.join(LOCAL, 'Programs', 'Inno Setup 6', 'ISCC.exe'),
               r'C:\Program Files (x86)\Inno Setup 6\ISCC.exe', r'C:\Program Files\Inno Setup 6\ISCC.exe']


def iscc():
    """Acha o compilador do Inno Setup; se não houver, instala (só para o usuário, sem admin). Tenta cada link e
    confere o arquivo baixado (um .exe de verdade, > 1 MB) antes de rodar; se nada der, tenta o winget."""
    achar = lambda: next((p for p in ISCC_LOCAIS if os.path.isfile(p)), None)
    if achar():
        return achar()
    pasta = os.path.join(LOCAL, 'legacy-inno')
    os.makedirs(pasta, exist_ok=True)
    for k, url in enumerate(INNO_URLS):
        inst = os.path.join(pasta, f'innosetup_{k}_{int(time.time())}.exe')  # nome novo: nunca reaproveita download estragado
        try:
            baixar(url, inst)
            with open(inst, 'rb') as f:
                ok = f.read(2) == b'MZ' and os.path.getsize(inst) > 1_000_000
            if not ok:
                print('download inválido (não é um .exe):', url, flush=True)
                continue
            rodar([inst, '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER',
                   '/DIR=' + os.path.join(LOCAL, 'InnoSetup6')])
            if achar():
                return achar()
        except Exception as e:  # noqa: BLE001 - tenta o próximo caminho
            print('falhou:', url, '->', e, flush=True)
    try:
        rodar(['winget', 'install', '--id', 'JRSoftware.InnoSetup', '-e', '--silent', '--scope', 'user',
               '--accept-package-agreements', '--accept-source-agreements'])
    except Exception as e:  # noqa: BLE001
        print('winget falhou:', e, flush=True)
    if achar():
        return achar()
    sys.exit('Não consegui instalar o Inno Setup (veja as mensagens acima).')


def node():
    pasta = os.path.join(LOCAL, 'legacy-node')
    exe = glob.glob(os.path.join(pasta, '*', 'node.exe'))
    if not exe:
        z = os.path.join(os.environ.get('TEMP', '.'), 'node.zip')
        baixar(NODE_URL, z)
        zipfile.ZipFile(z).extractall(pasta)
        exe = glob.glob(os.path.join(pasta, '*', 'node.exe'))
    return os.path.dirname(exe[0])


def jogos():
    os.makedirs(SAIDA, exist_ok=True)
    v = versao()
    py = sys.executable
    exp = os.path.join(MOTOR, 'tools', 'empacotar.py')
    subprocess.run([py, exp, '--cena', 'cenas/times_square.json', '--saida', os.path.join(DIST, 'TimesSquare'),
                    '--qualidade', 'medio'], cwd=MOTOR)  # avisos (licença do DLSS) não param a montagem
    subprocess.run([py, exp, '--cena', 'cenas/vale/vale.json', '--cena', 'cenas/parque.json', '--cena', 'cenas/fisica.json',
                    '--cena', 'cenas/carro.json', '--saida', os.path.join(DIST, 'LegacyEngine')], cwd=MOTOR)
    comp = iscc()
    for nome, origem in (('TimesSquare', 'TimesSquare'), ('LegacyEngine', 'LegacyEngine')):
        rodar([comp, f'/DOrigem={os.path.join(DIST, origem)}', f'/DVersao={v}', f'/O{SAIDA}', f'/F{nome}-Setup',
               os.path.join(RAIZ, 'plataforma', 'instaladores', f'{nome}.iss')])


def launcher():
    os.makedirs(SAIDA, exist_ok=True)
    pasta_node = node()
    env = dict(os.environ, PATH=pasta_node + os.pathsep + os.environ['PATH'])
    npm = os.path.join(pasta_node, 'npm.cmd')
    npx = os.path.join(pasta_node, 'npx.cmd')
    lan = os.path.join(RAIZ, 'plataforma', 'launcher')
    rodar([npm, 'install', '--no-audit', '--no-fund'], cwd=lan, env=env, shell=False)
    rodar([npm, 'pkg', 'set', 'version=' + versao()], cwd=lan, env=env)
    rodar([npx, 'electron-builder', '--win', 'nsis', '--publish', 'never'], cwd=lan, env=env)
    shutil.copyfile(os.path.join(lan, 'dist', 'Legacy-Setup.exe'), os.path.join(SAIDA, 'Legacy-Setup.exe'))
    for extra in ('latest.yml', 'Legacy-Setup.exe.blockmap'):  # autoatualização do launcher
        if os.path.isfile(os.path.join(lan, 'dist', extra)):
            shutil.copyfile(os.path.join(lan, 'dist', extra), os.path.join(SAIDA, extra))


def token_git():
    p = subprocess.run(['git', 'credential', 'fill'], input='protocol=https\nhost=github.com\n\n', capture_output=True,
                       text=True, cwd=RAIZ)
    campos = dict(l.split('=', 1) for l in p.stdout.splitlines() if '=' in l)
    if 'password' not in campos:
        sys.exit('Sem login do git para o github.com neste PC.')
    return campos['password']


def api(metodo, url, token, dados=None, tipo='application/json'):
    req = urllib.request.Request(url, method=metodo, data=dados, headers={
        'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.github+json', 'Content-Type': tipo,
        'User-Agent': 'legacy-instaladores'})
    with urllib.request.urlopen(req, timeout=1800) as r:
        return json.load(r)


# Atualização pelo launcher, baixando só o que mudou: cada produto ganha um manifesto (todos os arquivos com sha256,
# tamanho e endereço). Um arquivo igual ao da versão anterior reaproveita o endereço antigo (não sobe de novo); um
# novo ou mudado sobe comprimido como "<produto>-<sha256>.gz" nesta Release. Por isso as Releases antigas NÃO
# podem ser apagadas: os manifestos novos apontam para arquivos delas. (plataforma/launcher/atualizador.js)
PRODUTOS_DIST = (('times-square', 'TimesSquare'), ('legacy-engine', 'LegacyEngine'))
RELEASES = f'https://github.com/{REPO}/releases'


def sha256(arq):
    h = hashlib.sha256()
    with open(arq, 'rb') as f:
        for bloco in iter(lambda: f.read(1 << 20), b''):
            h.update(bloco)
    return h.hexdigest()


def manifesto_anterior(produto):
    try:
        with urllib.request.urlopen(f'{RELEASES}/latest/download/manifesto-{produto}.json', timeout=60) as r:
            return json.load(r)
    except Exception:  # noqa: BLE001 - primeira vez (ainda não há manifesto publicado)
        return {'arquivos': []}


def montar_manifesto(produto, pasta, tag, anterior, saida_gz):
    """Devolve (manifesto, [(arquivo .gz a subir, nome do asset)])."""
    antes = {a['sha256']: a for a in anterior.get('arquivos', [])}
    arquivos, subir = [], []
    os.makedirs(saida_gz, exist_ok=True)
    for raiz, _, nomes in os.walk(pasta):
        for n in sorted(nomes):
            arq = os.path.join(raiz, n)
            rel = os.path.relpath(arq, pasta).replace(os.sep, '/')
            h, tam = sha256(arq), os.path.getsize(arq)
            if h in antes:  # igual a um arquivo já publicado (nesta versão ou numa anterior): mesmo endereço
                url, comp = antes[h]['url'], antes[h].get('comprimido', tam)
            else:
                nome = f'{produto}-{h}.gz'
                gz = os.path.join(saida_gz, nome)
                with open(arq, 'rb') as f, gzip.open(gz, 'wb', compresslevel=6) as g:
                    shutil.copyfileobj(f, g)
                url, comp = f'{RELEASES}/download/{tag}/{nome}', os.path.getsize(gz)
                subir.append((gz, nome))
                antes[h] = {'url': url, 'comprimido': comp}  # arquivo repetido no produto: sobe uma vez só
            arquivos.append({'caminho': rel, 'sha256': h, 'tamanho': tam, 'comprimido': comp, 'url': url})
    arquivos.sort(key=lambda a: a['caminho'])
    return {'produto': produto, 'versao': tag, 'arquivos': arquivos}, subir


def publicar():
    arquivos = [os.path.join(SAIDA, n) for n in ('Legacy-Setup.exe', 'TimesSquare-Setup.exe', 'LegacyEngine-Setup.exe')]
    faltam = [a for a in arquivos if not os.path.isfile(a)]
    if faltam:
        sys.exit('Faltam instaladores: ' + ', '.join(faltam))
    # O autoatualizador do launcher (electron-updater) lê latest.yml; o .blockmap permite baixar só o que mudou.
    arquivos += [a for a in (os.path.join(SAIDA, 'latest.yml'), os.path.join(SAIDA, 'Legacy-Setup.exe.blockmap'))
                 if os.path.isfile(a)]
    token = token_git()
    v = versao()
    tag = f'v{v}'
    envios = [(a, os.path.basename(a)) for a in arquivos]
    for produto, pasta in PRODUTOS_DIST:
        man, subir = montar_manifesto(produto, os.path.join(DIST, pasta), tag, manifesto_anterior(produto),
                                      os.path.join(DIST, 'atualizacao', produto))
        novos = sum(os.path.getsize(g) for g, _ in subir)
        print(f'{produto}: {len(man["arquivos"])} arquivos, {len(subir)} novos ou mudados ({novos / 1e6:.1f} MB comprimidos)',
              flush=True)
        caminho = os.path.join(DIST, 'atualizacao', f'manifesto-{produto}.json')
        with open(caminho, 'w', encoding='utf-8') as f:
            json.dump(man, f)
        envios += subir + [(caminho, f'manifesto-{produto}.json')]
    # Rascunho primeiro: só vira a "mais nova" depois de todos os arquivos subirem (o launcher nunca vê um
    # manifesto apontando para um arquivo que ainda não existe).
    rel = api('POST', f'https://api.github.com/repos/{REPO}/releases', token, json.dumps({
        'tag_name': tag, 'name': f'Legacy {v}', 'draft': True,
        'target_commitish': subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=RAIZ, capture_output=True, text=True).stdout.strip(),
        'body': 'Legacy Launcher, Times Square e Legacy Engine para Windows (gerados no PC com a RTX). '
                'O launcher atualiza sozinho e baixa só os arquivos que mudaram.'}).encode())
    for k, (a, nome) in enumerate(envios):
        if os.path.getsize(a) > 5e6 or k % 50 == 0:
            print(f'enviando {nome} ({k + 1}/{len(envios)}, {os.path.getsize(a) / 1e6:.1f} MB)', flush=True)
        up = rel['upload_url'].split('{')[0] + '?name=' + urllib.parse.quote(nome)
        with open(a, 'rb') as f:
            api('POST', up, token, f.read(), 'application/octet-stream')
    api('PATCH', f'https://api.github.com/repos/{REPO}/releases/{rel["id"]}', token,
        json.dumps({'draft': False, 'make_latest': 'true'}).encode())
    print('Release publicada:', f'{RELEASES}/tag/{tag}', flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jogos', action='store_true')
    ap.add_argument('--launcher', action='store_true')
    ap.add_argument('--publicar', action='store_true')
    a = ap.parse_args()
    if a.jogos:
        jogos()
    if a.launcher:
        launcher()
    if a.publicar:
        publicar()


if __name__ == '__main__':
    main()
