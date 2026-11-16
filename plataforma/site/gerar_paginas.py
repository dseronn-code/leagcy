"""Gera termos.html e privacidade.html do site a partir de plataforma/docs/*.md (os mesmos textos do launcher).
Rode depois de editar os .md:  python3 plataforma/site/gerar_paginas.py"""
import os
import sys

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(AQUI, '..', 'docs'))
from gerar_textos_launcher import md_html  # noqa: E402

MOLDE = '''<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{titulo} — Legacy</title>
<link rel="icon" href="img/favicon.svg" type="image/svg+xml">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&family=Space+Grotesk:wght@500;700&display=swap" rel="stylesheet">
<link rel="stylesheet" href="estilo.css">
</head>
<body>
<header class="topo">
  <a class="marca" href="index.html" aria-label="Legacy, início"><img class="logo-l" src="img/logo.svg" alt=""><span class="marca-nome">LEGACY</span></a>
  <div class="topo-acoes">
    <a class="botao baixar" href="https://github.com/dseronn-code/legacy/releases/latest/download/Legacy-Setup.exe">Baixar</a>
  </div>
</header>
<main class="texto">
{corpo}
<p><a href="index.html">← Voltar ao início</a></p>
</main>
</body>
</html>
'''

for arq, md, titulo in (('termos.html', 'termos_de_uso.md', 'Termos de Uso'),
                        ('privacidade.html', 'politica_privacidade.md', 'Política de Privacidade')):
    with open(os.path.join(AQUI, '..', 'docs', md), encoding='utf-8') as f:
        corpo = md_html(f.read())
    with open(os.path.join(AQUI, arq), 'w', encoding='utf-8') as f:
        f.write(MOLDE.format(titulo=titulo, corpo=corpo))
    print(arq)
