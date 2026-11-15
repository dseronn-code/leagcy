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
import glob
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

MOTOR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAIZ = os.path.dirname(MOTOR)
DIST = os.path.join(RAIZ, 'dist')
SAIDA = os.path.join(DIST, 'instaladores')
LOCAL = os.environ.get('LOCALAPPDATA', os.path.expanduser('~'))
INNO_URL = 'https://jrsoftware.org/download.php/is.exe'
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


def iscc():
    for p in (os.path.join(LOCAL, 'InnoSetup6', 'ISCC.exe'), r'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
              r'C:\Program Files\Inno Setup 6\ISCC.exe'):
        if os.path.isfile(p):
            return p
    inst = os.path.join(os.environ.get('TEMP', '.'), 'innosetup.exe')
    baixar(INNO_URL, inst)
    rodar([inst, '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', '/DIR=' + os.path.join(LOCAL, 'InnoSetup6')])
    return os.path.join(LOCAL, 'InnoSetup6', 'ISCC.exe')


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
    exe = max(glob.glob(os.path.join(lan, 'dist', '*.exe')), key=os.path.getmtime)
    shutil.copyfile(exe, os.path.join(SAIDA, 'Legacy-Setup.exe'))


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


def publicar():
    arquivos = [os.path.join(SAIDA, n) for n in ('Legacy-Setup.exe', 'TimesSquare-Setup.exe', 'LegacyEngine-Setup.exe')]
    faltam = [a for a in arquivos if not os.path.isfile(a)]
    if faltam:
        sys.exit('Faltam instaladores: ' + ', '.join(faltam))
    token = token_git()
    v = versao()
    rel = api('POST', f'https://api.github.com/repos/{REPO}/releases', token, json.dumps({
        'tag_name': f'v{v}', 'name': f'Legacy {v}', 'make_latest': 'true',
        'target_commitish': subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=RAIZ, capture_output=True, text=True).stdout.strip(),
        'body': 'Legacy Launcher, Times Square e Legacy Engine para Windows (gerados no PC com a RTX).'}).encode())
    for a in arquivos:
        nome = os.path.basename(a)
        print('enviando', nome, f'{os.path.getsize(a) / 1e6:.0f} MB', flush=True)
        up = rel['upload_url'].split('{')[0] + '?name=' + nome
        with open(a, 'rb') as f:
            api('POST', up, token, f.read(), 'application/octet-stream')
    print('Release publicada:', rel['html_url'])


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
