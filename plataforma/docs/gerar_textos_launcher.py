"""Gera launcher/ui/textos.js (Termos e Política de Privacidade dentro do launcher) a partir dos .md desta pasta.
Rode depois de editar termos_de_uso.md ou politica_privacidade.md:  python3 plataforma/docs/gerar_textos_launcher.py"""
import html
import json
import os
import re

AQUI = os.path.dirname(os.path.abspath(__file__))


def md_html(md: str) -> str:
    out, lista = [], False
    for linha in md.splitlines():
        l = linha.strip()
        esc = html.escape(l)
        esc = re.sub(r'\*\*(.+?)\*\*', r'<b>\1</b>', esc)
        if l.startswith('- '):
            if not lista:
                out.append('<ul>')
                lista = True
            out.append('<li>' + esc[2:] + '</li>')
            continue
        if lista:
            out.append('</ul>')
            lista = False
        if not l:
            continue
        m = re.match(r'(#{1,3}) (.*)', esc)
        out.append(f'<h{len(m.group(1))}>{m.group(2)}</h{len(m.group(1))}>' if m else '<p>' + esc + '</p>')
    if lista:
        out.append('</ul>')
    return '\n'.join(out)


textos = {k: md_html(open(os.path.join(AQUI, f), encoding='utf-8').read())
          for k, f in (('termos', 'termos_de_uso.md'), ('privacidade', 'politica_privacidade.md'))}
destino = os.path.join(AQUI, '..', 'launcher', 'ui', 'textos.js')
with open(destino, 'w', encoding='utf-8') as f:
    f.write('// Gerado por plataforma/docs/gerar_textos_launcher.py a partir dos .md (não edite à mão).\n')
    f.write('window.TEXTOS_LEGACY = ' + json.dumps(textos, ensure_ascii=False, indent=1) + ';\n')
print(destino)
