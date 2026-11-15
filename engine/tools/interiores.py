#!/usr/bin/env python3
"""Interiores de loja em 3D de verdade (não imagem atrás do vidro): cada produto, prateleira, balcão, geladeira,
caixa registradora, revista, cartaz e luminária é geometria. Uso (de engine/):

    python3 tools/interiores.py [--saida cenas/interiores] [--tipo conveniencia|joalheria|todos] [--semente 7]

Sai um glTF por loja (origem no meio da porta, no chão; a loja se estende para +z, x de -L/2 a L/2), com as texturas
geradas aqui (rótulos, revistas, cartazes: marcas e nomes FICTÍCIOS) e as texturas reais CC0 do Poly Haven (piso,
madeira, mármore, gesso, veludo). O gerador da cidade encaixa essas lojas atrás das vitrines do térreo.
"""
import argparse
import math
import os
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from times_square import FONT, FONT2, Gltf, Mesh, Pbr, baixar_texturas  # noqa: E402

PH_EXTRA = ['floor_tiles_08', 'marble_01', 'dark_wood', 'wood_table_001', 'velour_velvet', 'plastered_wall_02',
            'laminate_floor_02']

MARCAS = ['CRUNCHOS', 'ZESTY', 'MOONPOP', 'FIZZ UP', 'GALAXY', 'NUTRO', 'SUNNY OATS', 'HOT BITE', 'CHOCO LOOP',
          'KRISP', 'AQUA PURA', 'VOLT', 'MANGO RUSH', 'BOLT', 'PEAKS', 'TWISTY', 'ROCKET', 'LUNA', 'OKTO',
          'MAPLE BEAR', 'CLOUD 9', 'TANGO', 'PIXEL', 'FROSTY', 'BIG CHEW', 'SALTY JOE', 'NOVA', 'KIWI KID',
          'DOUBLE DIP', 'PURE LEAF', 'BRISKO', 'HONEY HILL', 'RED FOX', 'ZAP', 'MELLOW', 'CRISPY', 'GOLDEN',
          'URBAN', 'ORBIT', 'POPSY']
REVISTAS = ['GLAMOUR CITY', 'SPEED', 'GOSSIP!', 'TECH NOW', 'FIT LIFE', 'MOVIE STAR', 'THE WEEK', 'HOME & CO',
            'GAMER', 'STYLE', 'SPORTS DAY', 'TRAVEL']


# ---------------------------------------------------------------------------------------------------------- texturas
def _fit(d, text, font_path, size, maxw):
    f = ImageFont.truetype(font_path, size)
    tw = d.textlength(text, font=f)
    if tw > maxw:
        f = ImageFont.truetype(font_path, max(8, int(size * maxw / tw)))
    return f


def atlas_produtos(path, seed, n=16, cell=128):
    """Atlas n×n de rótulos de produto: caixas de cereal, salgadinhos, latas, garrafas, chocolates, maços, sabão...
    Cada célula: fundo com gradiente e faixa, marca grande, um "selo" ou forma, texto miúdo e código de barras."""
    rng = random.Random(seed)
    W = n * cell
    img = Image.new('RGB', (W, W))
    for k in range(n * n):
        cx, cy = (k % n) * cell, (k // n) * cell
        c1 = np.array([rng.randrange(256) for _ in range(3)])
        c2 = np.clip(c1 * rng.uniform(0.3, 0.8) + rng.randrange(-40, 40), 0, 255)
        t = np.linspace(0, 1, cell)[:, None, None]
        tile = (c1 * (1 - t) + c2 * t) * np.ones((1, cell, 1))
        im = Image.fromarray(tile.astype(np.uint8))
        d = ImageDraw.Draw(im)
        fg = (255, 255, 255) if c1.sum() < 380 else (20, 20, 25)
        acc = tuple(rng.randrange(256) for _ in range(3))
        kind = rng.randrange(4)
        if kind == 0:  # faixa diagonal
            d.polygon([(0, cell * 0.55), (cell, cell * 0.35), (cell, cell * 0.5), (0, cell * 0.7)], fill=acc)
        elif kind == 1:  # sol / explosão
            r = cell * rng.uniform(0.18, 0.3)
            ox, oy = cell * rng.uniform(0.3, 0.7), cell * rng.uniform(0.55, 0.75)
            pts = [(ox + (r if i % 2 == 0 else r * 0.6) * math.cos(i * math.pi / 8),
                    oy + (r if i % 2 == 0 else r * 0.6) * math.sin(i * math.pi / 8)) for i in range(16)]
            d.polygon(pts, fill=acc)
        elif kind == 2:  # "foto" do produto: círculos (fruta, cereal, biscoito)
            for _ in range(rng.randrange(3, 8)):
                r = rng.uniform(6, 16)
                ox, oy = rng.uniform(15, cell - 15), rng.uniform(cell * 0.5, cell - 15)
                d.ellipse((ox - r, oy - r, ox + r, oy + r), fill=acc, outline=(40, 30, 20))
        else:  # faixa horizontal com moldura
            d.rectangle((0, cell * 0.62, cell, cell * 0.8), fill=acc)
        name = rng.choice(MARCAS)
        f = _fit(d, name, rng.choice([FONT, FONT2]), 30, cell - 10)
        tw = d.textlength(name, font=f)
        d.text(((cell - tw) / 2, cell * 0.12), name, font=f, fill=fg, stroke_width=2,
               stroke_fill=tuple(int(v * 0.4) for v in c1))
        sub = rng.choice(['ORIGINAL', 'EXTRA', 'LIGHT', 'FAMILY SIZE', 'NEW!', 'ZERO', 'CLASSIC', 'SPICY', 'BBQ'])
        f2 = _fit(d, sub, FONT, 14, cell - 20)
        d.text((10, cell * 0.42), sub, font=f2, fill=fg)
        for i in range(8):  # código de barras
            x = cell - 34 + i * 3
            d.line((x, cell - 16, x, cell - 6), fill=(10, 10, 10), width=1 + (i % 2))
        img.paste(im, (cx, cy))
    img.save(path, quality=90)
    return path


def atlas_revistas(path, seed, n=8, cw=128, ch=168):
    """Capas de revista (título, rosto em silhueta colorida, manchetes) em atlas n×n."""
    rng = random.Random(seed)
    img = Image.new('RGB', (n * cw, n * ch), (240, 240, 240))
    for k in range(n * n):
        x0, y0 = (k % n) * cw, (k // n) * ch
        bg = tuple(rng.randrange(256) for _ in range(3))
        im = Image.new('RGB', (cw, ch), bg)
        d = ImageDraw.Draw(im)
        skin = rng.choice([(230, 190, 160), (190, 140, 100), (120, 80, 55), (250, 215, 190)])
        d.ellipse((cw * 0.3, ch * 0.3, cw * 0.7, ch * 0.62), fill=skin)  # rosto
        d.rectangle((cw * 0.18, ch * 0.62, cw * 0.82, ch), fill=tuple(rng.randrange(256) for _ in range(3)))
        d.ellipse((cw * 0.26, ch * 0.22, cw * 0.74, ch * 0.42), fill=rng.choice([(30, 20, 15), (200, 160, 60),
                                                                                  (120, 50, 20), (20, 20, 20)]))
        title = rng.choice(REVISTAS)
        f = _fit(d, title, FONT2, 30, cw - 8)
        d.text((4, 4), title, font=f, fill=(255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0))
        for i in range(3):
            line = rng.choice(['10 SECRETS', 'EXCLUSIVE!', 'BEST OF 2026', 'SUMMER', 'TOP 50', 'HOW TO WIN', 'NEW LOOK'])
            d.text((4, ch * (0.55 + 0.12 * i)), line, font=ImageFont.truetype(FONT, 11), fill=(255, 255, 0))
        img.paste(im, (x0, y0))
    img.save(path, quality=90)
    return path


def atlas_cartazes(path, seed, n=4, cw=256, ch=352):
    """Cartazes de parede (promoções, loteria, bebidas) com marcas fictícias, em atlas n×n."""
    rng = random.Random(seed)
    img = Image.new('RGB', (n * cw, n * ch))
    frases = [('LOTTO', 'WIN BIG TODAY'), ('ICE COLD', 'GALAXY COLA'), ('2 FOR $5', 'CRUNCHOS'), ('NEW!', 'MANGO RUSH'),
              ('HOT COFFEE', '24 HOURS'), ('SALE', 'ALL SNACKS'), ('FRESH', 'DONUTS'), ('ATM INSIDE', 'NO FEE'),
              ('SCRATCH &', 'WIN $1000'), ('ENERGY', 'VOLT'), ('BEER', 'COLD HERE'), ('THANK YOU', 'COME AGAIN')]
    for k in range(n * n):
        x0, y0 = (k % n) * cw, (k // n) * ch
        bg = rng.choice([(200, 30, 40), (20, 60, 150), (250, 200, 30), (20, 20, 25), (30, 140, 70), (240, 240, 235)])
        fg = (255, 255, 255) if sum(bg) < 400 else (25, 25, 30)
        im = Image.new('RGB', (cw, ch), bg)
        d = ImageDraw.Draw(im)
        a, b = frases[k % len(frases)]
        r = rng.randrange(60, 110)
        d.ellipse((cw / 2 - r, ch * 0.62 - r, cw / 2 + r, ch * 0.62 + r),
                  fill=rng.choice([(255, 220, 0), (255, 255, 255), (230, 60, 60), (60, 200, 255)]))
        for txt, yy, sz in ((a, 0.08, 56), (b, 0.28, 34)):
            f = _fit(d, txt, FONT, sz, cw - 16)
            tw = d.textlength(txt, font=f)
            d.text(((cw - tw) / 2, ch * yy), txt, font=f, fill=fg)
        d.rectangle((3, 3, cw - 4, ch - 4), outline=fg, width=3)
        img.paste(im, (x0, y0))
    img.save(path, quality=90)
    return path


def textura_teto(path, w=512):
    """Forro de placas 60×60 cm (fibra mineral com furinhos) com a grade de alumínio."""
    rng = np.random.default_rng(3)
    v = 225 + 12 * rng.random((w, w))
    pin = rng.random((w, w)) > 0.985
    v[pin] = 150
    img = np.stack([v, v, v * 0.98], -1)
    g = w // 2  # 2 placas por textura (1,2 m)
    img[::g, :] = 175
    img[:, ::g] = 175
    img[1::g, :] = 175
    img[:, 1::g] = 175
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
    return path


def textura_marmore(path, w=1024):
    """Mármore branco polido com veios cinza (placas de 60 cm, juntas finas), para o piso da joalheria."""
    rng = np.random.default_rng(11)
    def noise(n):
        return np.asarray(Image.fromarray((rng.random((n, n)) * 255).astype(np.uint8)).resize((w, w), Image.BICUBIC)) / 255.0
    turb = noise(8) * 0.5 + noise(32) * 0.3 + noise(128) * 0.2
    yy, xx = np.mgrid[0:w, 0:w] / w
    vein = np.abs(np.sin((xx * 3 + yy * 2 + turb * 4) * math.pi))
    v = 232 - 70 * (vein < 0.06) * (1 - vein / 0.06) - 12 * noise(64)
    img = np.stack([v, v, v * 1.01], -1)
    tile = w // 2  # 2 placas por textura (1,2 m)
    img[::tile, :] *= 0.7
    img[:, ::tile] *= 0.7
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
    return path


def textura_damasco(path, w=256):
    """Papel de parede escuro de damasco (padrão ornamental repetido), para a joalheria."""
    img = Image.new('RGB', (w, w), (22, 22, 26))
    d = ImageDraw.Draw(img)
    for cx, cy in ((w / 2, w / 2), (0, 0), (w, 0), (0, w), (w, w)):
        for r in (60, 44, 28):
            d.ellipse((cx - r * 0.6, cy - r, cx + r * 0.6, cy + r), outline=(40, 40, 46), width=3)
        d.polygon([(cx, cy - 70), (cx + 18, cy), (cx, cy + 70), (cx - 18, cy)], outline=(46, 46, 52), width=2)
    img.save(path)
    return path


def textura_quadro(path, seed, w=256, h=352):
    """Retrato de época / anúncio de joia para as molduras douradas da joalheria."""
    rng = random.Random(seed)
    img = Image.new('RGB', (w, h), (15, 15, 18))
    d = ImageDraw.Draw(img)
    if seed % 2 == 0:  # retrato de dama: vestido claro, rosto, cabelo escuro, fundo esfumado
        bg = Image.new('RGB', (w, h), (40, 34, 30)).filter(ImageFilter.GaussianBlur(4))
        img.paste(bg)
        d.ellipse((w * 0.38, h * 0.18, w * 0.62, h * 0.38), fill=(235, 210, 190))
        d.ellipse((w * 0.34, h * 0.1, w * 0.66, h * 0.3), fill=(60, 40, 25))
        d.polygon([(w * 0.2, h), (w * 0.3, h * 0.45), (w * 0.7, h * 0.45), (w * 0.8, h)], fill=(225, 225, 230))
        img = img.filter(ImageFilter.GaussianBlur(1.5))
    else:  # anúncio: pedra preciosa no centro + nome fictício
        gem = rng.choice([(220, 30, 50), (40, 160, 230), (60, 200, 120), (250, 250, 255)])
        cx, cy = w / 2, h * 0.5
        d.polygon([(cx, cy - 50), (cx + 45, cy), (cx, cy + 55), (cx - 45, cy)], fill=gem, outline=(255, 255, 255))
        d.polygon([(cx, cy - 50), (cx + 20, cy - 5), (cx, cy + 10), (cx - 20, cy - 5)],
                  fill=tuple(min(255, c + 60) for c in gem))
        f = _fit(d, 'VALENTI', FONT2, 40, w - 30)
        tw = d.textlength('VALENTI', font=f)
        d.text(((w - tw) / 2, h * 0.08), 'VALENTI', font=f, fill=(212, 175, 90))
        d.text((w * 0.22, h * 0.85), 'FINE JEWELRY SINCE 1921', font=ImageFont.truetype(FONT, 11), fill=(212, 175, 90))
    img.save(path)
    return path


# ---------------------------------------------------------------------------------------------------------- geometria
def abox(M, mn, mx, uvs=1.0, skip=()):
    """Caixa alinhada aos eixos de mn a mx, UV em metros × uvs (madeira, gesso, metal)."""
    (x0, y0, z0), (x1, y1, z1) = mn, mx
    s = uvs
    F = {
        '+z': ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), ((x0 * s, y0 * s), (x1 * s, y0 * s), (x1 * s, y1 * s), (x0 * s, y1 * s)), (0, 0, 1)),
        '-z': ((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), ((x1 * s, y0 * s), (x0 * s, y0 * s), (x0 * s, y1 * s), (x1 * s, y1 * s)), (0, 0, -1)),
        '+x': ((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), ((z1 * s, y0 * s), (z0 * s, y0 * s), (z0 * s, y1 * s), (z1 * s, y1 * s)), (1, 0, 0)),
        '-x': ((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), ((z0 * s, y0 * s), (z1 * s, y0 * s), (z1 * s, y1 * s), (z0 * s, y1 * s)), (-1, 0, 0)),
        '+y': ((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), ((x0 * s, z1 * s), (x1 * s, z1 * s), (x1 * s, z0 * s), (x0 * s, z0 * s)), (0, 1, 0)),
        '-y': ((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), ((x0 * s, z0 * s), (x1 * s, z0 * s), (x1 * s, z1 * s), (x0 * s, z1 * s)), (0, -1, 0)),
    }
    for k, (a, b, c, d, uv, f) in F.items():
        if k not in skip:
            M.quad(a, b, c, d, uv=uv, face=f)


def cell_uv(k, n=16, pad=0.004):
    u0, v0 = (k % n) / n + pad, (k // n) / n + pad
    u1, v1 = u0 + 1 / n - 2 * pad, v0 + 1 / n - 2 * pad
    return u0, v0, u1, v1


def produto_caixa(M, x, y, z, w, h, d, k, face=(0, 0, -1), n=16):
    """Caixa de produto com o rótulo k do atlas na frente (face) e nas laterais (o mesmo rótulo apertado)."""
    u0, v0, u1, v1 = cell_uv(k, n)
    x0, x1, y0, y1, z0, z1 = x - w / 2, x + w / 2, y, y + h, z - d / 2, z + d / 2
    lab = ((u0, v1), (u1, v1), (u1, v0), (u0, v0))  # v do glTF: 0 em cima
    fz = face[2]
    if fz < 0:  # frente para -z (para o corredor do lado -z)
        M.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), uv=lab, face=(0, 0, -1))
        M.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), uv=lab, face=(0, 0, 1))
    else:
        M.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), uv=lab, face=(0, 0, 1))
        M.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), uv=lab, face=(0, 0, -1))
    side = ((u0, v1), (u0 + (u1 - u0) * 0.2, v1), (u0 + (u1 - u0) * 0.2, v0), (u0, v0))
    M.quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), uv=side, face=(1, 0, 0))
    M.quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), uv=side, face=(-1, 0, 0))
    top = ((u0, v0), (u0 + (u1 - u0) * 0.2, v0), (u0 + (u1 - u0) * 0.2, v0 + 0.002), (u0, v0 + 0.002))
    M.quad((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), uv=top, face=(0, 1, 0))


def produto_cilindro(M, x, y, z, r, h, k, garrafa=False, seg=10, n=16):
    """Lata ou garrafa: o rótulo k dá a volta; garrafa tem ombro e gargalo (tampa por cima)."""
    u0, v0, u1, v1 = cell_uv(k, n)
    body = h * (0.62 if garrafa else 1.0)
    for i in range(seg):
        a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
        P = lambda a, rr, yy: (x + rr * math.cos(a), yy, z + rr * math.sin(a))
        ua, ub = u0 + (u1 - u0) * i / seg, u0 + (u1 - u0) * (i + 1) / seg
        Na = (math.cos(a0), 0, math.sin(a0))
        Nb = (math.cos(a1), 0, math.sin(a1))
        M.quad(P(a0, r, y), P(a1, r, y), P(a1, r, y + body), P(a0, r, y + body), uv=((ua, v1), (ub, v1), (ub, v0), (ua, v0)),
               face=((Na[0] + Nb[0]) / 2, 0, (Na[2] + Nb[2]) / 2), nrms=(Na, Nb, Nb, Na))
        top_uv = ((u0, v0), (u0 + 0.001, v0), (u0 + 0.001, v0 + 0.001), (u0, v0 + 0.001))
        if garrafa:  # ombro e gargalo
            rn = r * 0.32
            M.quad(P(a0, r, y + body), P(a1, r, y + body), P(a1, rn, y + h * 0.82), P(a0, rn, y + h * 0.82), uv=top_uv,
                   face=((Na[0] + Nb[0]) / 2, 0.5, (Na[2] + Nb[2]) / 2))
            M.quad(P(a0, rn, y + h * 0.82), P(a1, rn, y + h * 0.82), P(a1, rn, y + h), P(a0, rn, y + h), uv=top_uv,
                   face=((Na[0] + Nb[0]) / 2, 0, (Na[2] + Nb[2]) / 2))
    M.frustum((x, z), r * (0.32 if garrafa else 1.0), r * (0.32 if garrafa else 1.0), y + h - 0.001, y + h, seg=seg)


def luminaria_tubo(F, E, x, y, z, comp=1.2, larg=0.3):
    """Calha de luminária fluorescente pendurada: corpo de chapa, difusor emissivo embaixo, dois tirantes."""
    abox(F, (x - larg / 2, y, z - comp / 2), (x + larg / 2, y + 0.08, z + comp / 2))
    abox(E, (x - larg / 2 + 0.03, y - 0.005, z - comp / 2 + 0.03), (x + larg / 2 - 0.03, y, z + comp / 2 - 0.03))
    for zz in (z - comp / 2 + 0.1, z + comp / 2 - 0.1):
        F.beam((x, y + 0.08, zz), (x, y + 0.6, zz), 0.01, 0.01)


def vidro(g, name, tint=(0.95, 0.97, 0.98)):
    m = g.material(name, color=tint, rough=0.02, double=True)
    g.doc['materials'][m]['extensions'] = {'KHR_materials_transmission': {'transmissionFactor': 0.95},
                                           'KHR_materials_ior': {'ior': 1.5}}
    for ext in ('KHR_materials_transmission', 'KHR_materials_ior'):
        if ext not in g.doc['extensionsUsed']:
            g.doc['extensionsUsed'].append(ext)
    return m


# ---------------------------------------------------------------------------------------------------------- lojas
def conveniencia(out, texdir, seed, g=None, pbr=None):
    """Loja de conveniência 24 h (10 × 12 × 3,4 m): geladeiras de porta de vidro no fundo, prateleiras de parede,
    duas gôndolas duplas, balcão com caixa registradora, vitrine de doces, expositor de loteria, parede de cigarros e
    bebidas atrás do caixa, revisteiro, caixotes de fruta, cartazes, forro de placas com calhas fluorescentes."""
    rng = random.Random(seed)
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    tex = lambda n: os.path.join(out, 'tex', 'int_' + n)
    embed = g is not None  # dentro da cidade: as malhas viram instâncias; sozinha: grava o glTF da loja
    if not embed:
        g = Gltf()
        pbr = Pbr(g, texdir, out)
    W, D, H = 10.0, 12.0, 3.4
    M = {k: Mesh() for k in ('piso', 'parede', 'teto', 'metal', 'prateleira', 'produtos', 'luz', 'balcao', 'vidro',
                             'luz_fria', 'revistas', 'cartazes', 'escuro', 'tapete', 'fruta', 'plastico')}
    # ---- casca: piso, paredes, forro (a parede da frente é a vitrine da cidade)
    M['piso'].quad((-W / 2, 0, 0), (W / 2, 0, 0), (W / 2, 0, D), (-W / 2, 0, D), uv=((-W / 4, 0), (W / 4, 0), (W / 4, D / 2), (-W / 4, D / 2)),
                   face=(0, 1, 0))
    for (a, b, f) in (((-W / 2, 0, D), (W / 2, 0, D), (0, 0, -1)), ((-W / 2, 0, 0), (-W / 2, 0, D), (1, 0, 0)),
                      ((W / 2, 0, D), (W / 2, 0, 0), (-1, 0, 0))):
        L = math.dist(a, b)
        M['parede'].quad(a, b, (b[0], H, b[2]), (a[0], H, a[2]), uv=((0, 0), (L / 2, 0), (L / 2, H / 2), (0, H / 2)), face=f)
        M['escuro'].quad(a, b, (b[0], 0.15, b[2]), (a[0], 0.15, a[2]), face=f)  # rodapé
    M['teto'].quad((-W / 2, H, 0), (W / 2, H, 0), (W / 2, H, D), (-W / 2, H, D), uv=((0, 0), (W / 1.2, 0), (W / 1.2, D / 1.2), (0, D / 1.2)),
                   face=(0, -1, 0))
    lights = []
    for x in (-2.6, 0.0, 2.6):  # fileiras de calhas
        for z in np.arange(2.0, D - 1.0, 2.2):
            luminaria_tubo(M['metal'], M['luz'], x, H - 0.6, z)
            lights.append((x, H - 0.75, z))
    # ---- geladeiras no fundo (portas de vidro, prateleiras com bebidas, luz fria nos montantes)
    nd = 9
    dw = (W - 1.0) / nd
    zf = D - 0.9
    abox(M['escuro'], (-W / 2 + 0.5, 0, zf), (W / 2 - 0.5, 0.25, D))
    abox(M['escuro'], (-W / 2 + 0.5, 2.35, zf), (W / 2 - 0.5, H, D))
    abox(M['cartazes'], (-W / 2 + 0.5, 2.4, zf - 0.02), (W / 2 - 0.5, 2.7, zf), skip=('+z', '-x', '+x', '+y', '-y'))
    for i in range(nd + 1):
        x = -W / 2 + 0.5 + i * dw
        abox(M['metal'], (x - 0.03, 0.25, zf - 0.05), (x + 0.03, 2.35, zf))
        abox(M['luz_fria'], (x - 0.012, 0.3, zf + 0.02), (x + 0.012, 2.3, zf + 0.06))
    M['vidro'].quad((-W / 2 + 0.5, 0.25, zf - 0.03), (W / 2 - 0.5, 0.25, zf - 0.03), (W / 2 - 0.5, 2.35, zf - 0.03),
                    (-W / 2 + 0.5, 2.35, zf - 0.03), face=(0, 0, -1))
    for i in range(nd):
        x0 = -W / 2 + 0.5 + i * dw
        abox(M['metal'], (x0 + 0.4, 1.0, zf - 0.07), (x0 + 0.43, 1.6, zf - 0.04))  # puxador
        for sy in (0.3, 0.75, 1.2, 1.65, 2.05):
            abox(M['prateleira'], (x0 + 0.04, sy - 0.02, zf + 0.05), (x0 + dw - 0.04, sy, D - 0.1))
            k = rng.randrange(256)
            garrafa = rng.random() < 0.6
            r = 0.035 if garrafa else 0.033
            h = rng.uniform(0.24, 0.32) if garrafa else 0.125
            for xx in np.arange(x0 + 0.08, x0 + dw - 0.05, 2 * r + 0.008):
                for zz in (zf + 0.12, zf + 0.22):
                    produto_cilindro(M['produtos'], xx, sy, zz, r, h, k, garrafa)
    # ---- prateleira de parede (lado esquerdo) e duas gôndolas duplas no meio
    def gondola(x0, x1, zc, depth, height, faces, levels):
        abox(M['prateleira'], (x0, 0, zc - depth / 2), (x1, 0.12, zc + depth / 2))
        if faces == 2:
            abox(M['prateleira'], (x0, 0.12, zc - 0.02), (x1, height, zc + 0.02))  # costas perfuradas
        for xx in np.arange(x0, x1 + 0.01, 1.0):
            abox(M['metal'], (xx - 0.02, 0, zc - 0.03), (xx + 0.02, height, zc + 0.03))
        sides = (-1, 1) if faces == 2 else (-1,)
        for sd in sides:
            zs0 = zc + sd * 0.03
            zs1 = zc + sd * depth / 2
            for lv in range(levels):
                sy = 0.15 + lv * (height - 0.25) / levels
                abox(M['prateleira'], (x0, sy - 0.025, min(zs0, zs1)), (x1, sy, max(zs0, zs1)))
                abox(M['cartazes'], (x0, sy - 0.06, zs1 - 0.005 * sd), (x1, sy - 0.025, zs1),
                     skip=('+x', '-x', '+y', '-y', '+z' if sd < 0 else '-z'))  # etiqueta de preço
                x = x0 + 0.04
                while x < x1 - 0.1:  # frentes de produto: 3 a 7 iguais lado a lado
                    k = rng.randrange(256)
                    typ = rng.random()
                    if typ < 0.55:
                        w, h, d = rng.uniform(0.12, 0.26), rng.uniform(0.18, min(0.36, (height - 0.25) / levels - 0.05)), rng.uniform(0.05, 0.09)
                    elif typ < 0.8:
                        w, h, d = rng.uniform(0.16, 0.24), rng.uniform(0.2, 0.3), 0.07  # saquinho
                    else:
                        w, h, d = 0.07, rng.uniform(0.18, 0.26), 0.07
                    cnt = rng.randrange(3, 8)
                    for _ in range(cnt):
                        if x + w > x1 - 0.02:
                            break
                        zf2 = zs1 - sd * (d / 2 + 0.01)
                        if typ >= 0.8:
                            produto_cilindro(M['produtos'], x + w / 2, sy, zf2, 0.033, h, k, garrafa=True, seg=8)
                        else:
                            produto_caixa(M['produtos'], x + w / 2, sy, zf2, w * 0.95, h, d, k, face=(0, 0, sd))
                            produto_caixa(M['produtos'], x + w / 2, sy, zf2 - sd * d, w * 0.95, h, d, k, face=(0, 0, sd))
                        x += w
                    x += 0.02
    # parede esquerda: prateleira encostada (frente para +x): monta como gôndola girada (aqui em z, frente -z...)
    for zc in (5.0, 8.0):
        gondola(-2.6, 1.2, zc, 0.8, 1.6, 2, 4)
    # prateleira de parede do lado esquerdo, ao longo de z (gera ao longo de x e gira trocando eixos)
    tmp = {k: Mesh() for k in ('prateleira', 'produtos', 'metal', 'cartazes')}
    saved = {k: M[k] for k in tmp}
    M.update(tmp)
    gondola(1.0, 9.0, 0.0, 0.5, 2.1, 1, 5)
    M.update(saved)
    for k, src in tmp.items():  # gira 90°: (x, z) -> (-W/2 + 0.25 - z, x)  → encostada na parede esquerda
        for i in range(src.nv):
            x, y, z = src.p[3 * i:3 * i + 3]
            nx, ny, nz = src.n[3 * i:3 * i + 3]
            M[k].vert((-W / 2 + 0.3 - z, y, x), (-nz, ny, nx), src.uv[2 * i:2 * i + 2])
        base = M[k].nv - src.nv
        M[k].idx.extend(i + base for i in src.idx)
    # ---- balcão do caixa (direita, perto da porta), em L, com vitrine de doces na frente
    bx0, bx1, bz0, bz1 = 1.6, 4.6, 1.6, 2.4
    abox(M['balcao'], (bx0, 0, bz0), (bx1, 0.95, bz1), uvs=0.8)
    abox(M['balcao'], (bx1 - 0.8, 0, bz1), (bx1, 0.95, 4.2), uvs=0.8)
    abox(M['escuro'], (bx0 - 0.02, 0.95, bz0 - 0.02), (bx1 + 0.02, 0.99, bz1 + 0.02))
    abox(M['escuro'], (bx1 - 0.82, 0.95, bz1), (bx1 + 0.02, 0.99, 4.22))
    for lv in (0.2, 0.5):  # vitrine de doces na frente do balcão (prateleirinhas inclinadas com chocolates)
        x = bx0 + 0.1
        while x < bx1 - 0.15:
            k = rng.randrange(256)
            for _ in range(rng.randrange(3, 6)):
                produto_caixa(M['produtos'], x + 0.045, lv, bz0 - 0.06, 0.09, 0.03, 0.12, k, face=(0, 0, -1))
                x += 0.095
            x += 0.02
        abox(M['prateleira'], (bx0 + 0.05, lv - 0.02, bz0 - 0.14), (bx1 - 0.05, lv, bz0))
    # caixa registradora, monitor, maquininha, expositor de loteria
    cx = bx0 + 0.9
    abox(M['plastico'], (cx - 0.22, 0.99, 1.85), (cx + 0.22, 1.12, 2.25))
    abox(M['plastico'], (cx - 0.2, 1.12, 2.05), (cx + 0.2, 1.2, 2.25))
    abox(M['escuro'], (cx - 0.18, 1.2, 2.15), (cx + 0.18, 1.45, 2.19))  # tela
    abox(M['plastico'], (cx - 0.04, 1.12, 2.17), (cx + 0.04, 1.2, 2.2))
    abox(M['escuro'], (cx + 0.4, 0.99, 1.75), (cx + 0.5, 1.03, 1.95))  # maquininha de cartão
    abox(M['cartazes'], (bx0 + 0.15, 0.99, 1.7), (bx0 + 0.45, 1.35, 1.72))  # expositor de loteria
    abox(M['cartazes'], (bx1 - 0.6, 0.99, 1.75), (bx1 - 0.2, 1.25, 1.95))  # display de chiclete
    # parede atrás do caixa: painel de cigarros (maços) e bebidas, com letreiros iluminados por cima
    zw = 4.6
    abox(M['escuro'], (bx0, 0.0, zw), (bx1 + 0.3, 2.6, zw + 0.35))
    for row in range(8):
        y = 1.0 + row * 0.16
        x = bx0 + 0.1
        while x < bx1 + 0.2:
            k = rng.randrange(256)
            for _ in range(rng.randrange(4, 9)):
                produto_caixa(M['produtos'], x + 0.03, y, zw - 0.03, 0.055, 0.085, 0.022, k, face=(0, 0, -1))
                x += 0.06
            x += 0.015
    for i in range(3):  # letreiros iluminados de marcas sobre o painel
        x = bx0 + 0.2 + i * 1.05
        abox(M['cartazes'], (x, 2.35, zw - 0.12), (x + 0.95, 2.6, zw - 0.04))
        abox(M['luz'], (x + 0.02, 2.33, zw - 0.1), (x + 0.93, 2.35, zw - 0.06))
    # revisteiro perto da vitrine e caixotes de fruta
    for i in range(4):
        y = 0.4 + i * 0.3
        abox(M['prateleira'], (-4.6, y - 0.02, 0.6), (-2.6, y, 0.9))
        x = -4.55
        while x < -2.7:
            u0, v0, u1, v1 = ((rng.randrange(64) % 8) / 8, (rng.randrange(64) // 8) / 8, 0, 0)
            u1, v1 = u0 + 1 / 8 - 0.003, v0 + 1 / 8 - 0.003
            M['revistas'].quad((x, y, 0.62 + 0.0), (x + 0.21, y, 0.62), (x + 0.21, y + 0.27, 0.72), (x, y + 0.27, 0.72),
                               uv=((u0, v1), (u1, v1), (u1, v0), (u0, v0)), face=(0, 0.4, -1))
            x += 0.23
    for i, col in enumerate([(0.9, 0.45, 0.05), (0.85, 0.8, 0.1), (0.6, 0.05, 0.05), (0.3, 0.6, 0.1)]):
        x = -4.4 + (i % 2) * 0.7
        z = 2.0 + (i // 2) * 0.6
        abox(M['balcao'], (x - 0.3, 0.55 + 0.25 * (i // 2), z - 0.25), (x + 0.3, 0.65 + 0.25 * (i // 2), z + 0.25))
        for _ in range(14):
            M['fruta'].sphere((x + rng.uniform(-0.24, 0.24), 0.7 + 0.25 * (i // 2), z + rng.uniform(-0.2, 0.2)),
                              rng.uniform(0.04, 0.06), 8)
    # tapetes da entrada, cartazes nas paredes, relógio, câmera de segurança, ventilador
    abox(M['tapete'], (-1.4, 0.0, 0.3), (1.0, 0.012, 1.6))
    for i in range(5):
        z = 2.0 + i * 1.8
        u0, v0 = (i % 4) / 4, (i // 4) / 4
        M['cartazes'].quad((W / 2 - 0.01, 1.4, z + 0.35), (W / 2 - 0.01, 1.4, z - 0.35), (W / 2 - 0.01, 2.35, z - 0.35),
                           (W / 2 - 0.01, 2.35, z + 0.35), uv=((u0, v0 + 0.25), (u0 + 0.25, v0 + 0.25), (u0 + 0.25, v0), (u0, v0)),
                           face=(-1, 0, 0))
    M['plastico'].cylinder((3.0, D - 0.95), 0.18, 2.85, 2.86, seg=20)  # relógio (fica na parede do fundo, no alto)
    M['escuro'].sphere((W / 2 - 0.3, H - 0.3, 0.4), 0.09, 10)  # câmera
    # ---- materiais
    t_prod = os.path.relpath(atlas_produtos(tex('produtos.jpg'), seed), out)
    t_rev = os.path.relpath(atlas_revistas(tex('revistas.jpg'), seed + 1), out)
    t_car = os.path.relpath(atlas_cartazes(tex('cartazes.jpg'), seed + 2), out)
    t_teto = os.path.relpath(textura_teto(tex('forro.png')), out)
    mats = {
        'piso': pbr('piso', 'floor_tiles_08', tint=(0.85, 0.85, 0.85), rough=0.6),
        'parede': pbr('parede', 'plastered_wall_02', tint=(0.85, 0.86, 0.82)),
        'teto': g.material('forro', tex=t_teto, rough=0.9),
        'metal': g.material('metal', color=(0.6, 0.62, 0.64), rough=0.35, metal=1.0),
        'prateleira': g.material('prateleira', color=(0.82, 0.82, 0.8), rough=0.4, metal=0.3),
        'produtos': g.material('produtos', tex=t_prod, rough=0.35),
        'luz': g.material('luz_tubo', color=(1, 1, 1), emissive=(1.0, 0.98, 0.94), strength=18.0),
        'luz_fria': g.material('luz_geladeira', color=(1, 1, 1), emissive=(0.85, 0.93, 1.0), strength=25.0),
        'balcao': pbr('balcao', 'wood_table_001', tint=(0.75, 0.7, 0.65), rough=0.8),
        'vidro': vidro(g, 'vidro_geladeira'),
        'revistas': g.material('revistas', tex=t_rev, rough=0.25),
        'cartazes': g.material('cartazes', tex=t_car, rough=0.4, emissive=(0.25, 0.25, 0.25), emissive_tex=t_car,
                               strength=1.0),
        'escuro': g.material('escuro', color=(0.05, 0.05, 0.055), rough=0.5),
        'tapete': g.material('tapete', color=(0.08, 0.09, 0.1), rough=0.95),
        'fruta': g.material('fruta', color=(0.85, 0.4, 0.05), rough=0.45),
        'plastico': g.material('plastico', color=(0.85, 0.85, 0.82), rough=0.35),
    }
    ids = [g.add_mesh('conveniencia_' + k, mesh, mats[k], scene_node=not embed) for k, mesh in M.items()]
    luzes = [((x, y, z), (1.0, 0.97, 0.92), 140.0, 10.0) for (x, y, z) in lights]
    luzes += [((x, 1.3, zf - 0.3), (0.85, 0.93, 1.0), 40.0, 4.0) for x in np.linspace(-W / 2 + 1.0, W / 2 - 1.0, 4)]
    if embed:
        return [i for i in ids if i is not None], luzes, W
    for pos, col, inten, rr in luzes:
        g.light(pos, col, inten, rr)
    path = os.path.join(out, 'conveniencia.gltf')
    g.write(path)
    print(path, sum(m.nv for m in M.values()), 'vértices')
    return path


def joalheria(out, texdir, seed, g=None, pbr=None):
    """Joalheria de luxo (12 × 14 × 4,5 m): piso de mármore claro com losangos pretos, paredes de damasco escuro,
    vitrines de madeira escura com tampo de vidro e bandejas brancas com anéis, colares e relógios, lustre de pingentes
    de cristal, pendentes, arandelas, quadros em molduras douradas, balcão, cadeiras e vasos."""
    rng = random.Random(seed)
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    tex = lambda n: os.path.join(out, 'tex', 'int_' + n)
    embed = g is not None
    if not embed:
        g = Gltf()
        pbr = Pbr(g, texdir, out)
    W, D, H = 12.0, 14.0, 4.5
    M = {k: Mesh() for k in ('piso', 'losango', 'parede', 'teto', 'madeira', 'vidro', 'bandeja', 'ouro', 'prata',
                             'gema_r', 'gema_b', 'gema_w', 'cristal', 'luz', 'quadro', 'escuro', 'veludo', 'planta',
                             'moldura_branca', 'luz_vitrine')}
    M['piso'].quad((-W / 2, 0, 0), (W / 2, 0, 0), (W / 2, 0, D), (-W / 2, 0, D),
                   uv=((-W / 2.4, 0), (W / 2.4, 0), (W / 2.4, D / 2.4), (-W / 2.4, D / 2.4)), face=(0, 1, 0))
    for x in np.arange(-W / 2 + 0.6, W / 2, 0.6):  # losangos pretos nos cruzamentos das placas
        for z in np.arange(0.6, D, 0.6):
            s = 0.06
            M['losango'].quad((x, 0.002, z - s), (x + s, 0.002, z), (x, 0.002, z + s), (x - s, 0.002, z), face=(0, 1, 0))
    for (a, b, f) in (((-W / 2, 0, D), (W / 2, 0, D), (0, 0, -1)), ((-W / 2, 0, 0), (-W / 2, 0, D), (1, 0, 0)),
                      ((W / 2, 0, D), (W / 2, 0, 0), (-1, 0, 0))):
        L = math.dist(a, b)
        M['parede'].quad(a, b, (b[0], H, b[2]), (a[0], H, a[2]), uv=((0, 0), (L / 1.2, 0), (L / 1.2, H / 1.2), (0, H / 1.2)), face=f)
        M['moldura_branca'].quad(a, b, (b[0], 0.25, b[2]), (a[0], 0.25, a[2]), face=f)
        M['moldura_branca'].quad((a[0], H - 0.3, a[2]), (b[0], H - 0.3, b[2]), (b[0], H, b[2]), (a[0], H, a[2]), face=f)
    M['teto'].quad((-W / 2, H, 0), (W / 2, H, 0), (W / 2, H, D), (-W / 2, H, D), face=(0, -1, 0))
    lights = []

    def vitrine(x0, x1, z0, z1):
        """Balcão-vitrine: caixa de madeira escura, faixa de luz, tampo e laterais de vidro, bandejas com joias."""
        abox(M['madeira'], (x0, 0, z0), (x1, 0.85, z1), uvs=0.7)
        abox(M['escuro'], (x0 - 0.02, 0.85, z0 - 0.02), (x1 + 0.02, 0.9, z1 + 0.02))
        abox(M['bandeja'], (x0 + 0.05, 0.9, z0 + 0.05), (x1 - 0.05, 0.93, z1 - 0.05))
        # vidro: tampo e 4 lados
        y0, y1 = 0.9, 1.22
        V = M['vidro']
        V.quad((x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1), face=(0, 1, 0))
        V.quad((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), face=(0, 0, -1))
        V.quad((x1, y0, z1), (x0, y0, z1), (x0, y1, z1), (x1, y1, z1), face=(0, 0, 1))
        V.quad((x0, y0, z1), (x0, y0, z0), (x0, y1, z0), (x0, y1, z1), face=(-1, 0, 0))
        V.quad((x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0), face=(1, 0, 0))
        abox(M['luz_vitrine'], (x0 + 0.05, 1.2, (z0 + z1) / 2 - 0.015), (x1 - 0.05, 1.215, (z0 + z1) / 2 + 0.015))
        lights.append(((x0 + x1) / 2, 1.15, (z0 + z1) / 2))
        # joias: degraus de expositor com anéis (toros), bustos com colar, suportes de relógio
        x = x0 + 0.2
        while x < x1 - 0.2:
            kind = rng.randrange(3)
            zc = (z0 + z1) / 2
            if kind == 0:  # escadinha de anéis
                for st in range(3):
                    abox(M['bandeja'], (x - 0.12, 0.93, zc - 0.15 + st * 0.1), (x + 0.12, 0.96 + st * 0.04, zc - 0.05 + st * 0.1))
                    for rx in np.linspace(x - 0.09, x + 0.09, 4):
                        ry = 0.975 + st * 0.04
                        ring = [(rx + 0.012 * math.cos(a), ry + 0.012 * math.sin(a), zc - 0.1 + st * 0.1)
                                for a in np.linspace(0, 2 * math.pi, 13)]
                        M[rng.choice(['ouro', 'prata'])].tube(ring, 0.0025, seg=5)
                        M[rng.choice(['gema_r', 'gema_b', 'gema_w'])].sphere((rx, ry + 0.015, zc - 0.1 + st * 0.1), 0.0045, 6)
                x += 0.35
            elif kind == 1:  # busto de veludo com colar
                M['veludo'].frustum((x, zc), 0.06, 0.03, 0.93, 1.12, seg=10)
                M['veludo'].sphere((x, 1.12, zc), 0.035, 8, sy=0.6)
                pts = [(x + 0.055 * math.sin(a), 1.09 - 0.05 * math.cos(a) ** 2, zc - 0.035 * math.cos(a))
                       for a in np.linspace(-1.4, 1.4, 15)]
                metal = rng.choice(['ouro', 'prata'])
                M[metal].tube(pts, 0.003, seg=4)
                for p in pts[::2]:
                    M[rng.choice(['gema_r', 'gema_b', 'gema_w'])].sphere(p, 0.006, 6)
                x += 0.3
            else:  # relógios em suporte
                for rx in (x - 0.06, x + 0.06):
                    M['bandeja'].frustum((rx, zc), 0.025, 0.025, 0.93, 1.02, seg=10)
                    M[rng.choice(['ouro', 'prata'])].tube([(rx + 0.03 * math.cos(a), 1.03 + 0.03 * math.sin(a), zc)
                                                         for a in np.linspace(0, 2 * math.pi, 15)], 0.006, seg=5)
                    M['escuro'].cylinder((rx, zc + 0.0), 0.022, 1.0, 1.0, seg=12)
                x += 0.3

    for (x0, x1, z0, z1) in ((-4.5, -1.5, 4.0, 4.8), (1.5, 4.5, 4.0, 4.8), (-1.2, 1.2, 6.5, 8.5),
                             (-5.3, -4.5, 6.0, 11.0), (4.5, 5.3, 6.0, 11.0), (-3.0, 3.0, 11.6, 12.4)):
        if x1 - x0 >= z1 - z0:
            vitrine(x0, x1, z0, z1)
        else:  # vitrine de lado: gera em x e gira
            tmp = {k: Mesh() for k in M}
            saved = M.copy()
            M.update(tmp)
            vitrine(z0 - (z0 + z1) / 2, z1 - (z0 + z1) / 2, x0 - (x0 + x1) / 2, x1 - (x0 + x1) / 2)
            M.update(saved)
            cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
            for k, src in tmp.items():
                for i in range(src.nv):
                    x, y, z = src.p[3 * i:3 * i + 3]
                    nx, ny, nz = src.n[3 * i:3 * i + 3]
                    M[k].vert((cx + z, y, cz + x), (nz, ny, nx), src.uv[2 * i:2 * i + 2])
                base = M[k].nv - src.nv
                # a troca de eixos espelha: inverte a ordem dos triângulos
                ids = list(src.idx)
                for t in range(0, len(ids), 3):
                    M[k].idx.extend((ids[t] + base, ids[t + 2] + base, ids[t + 1] + base))
            lights[-1] = (cx, 1.15, cz)
    # lustre de cristais (pingentes em espiral) e pendentes
    lx, lz = 0.0, 3.0
    for i in range(70):
        a = i * 0.55
        r = 0.15 + 0.6 * (i / 70)
        y1 = H - 0.5 - (i / 70) * 0.3
        L = rng.uniform(0.5, 1.4)
        M['cristal'].cylinder((lx + r * math.cos(a), lz + r * math.sin(a)), 0.018, y1 - L, y1, seg=6)
    M['escuro'].beam((lx, H, lz), (lx, H - 0.5, lz), 0.03, 0.03)
    lights.append((lx, H - 1.2, lz))
    for x in np.linspace(-4.5, 4.5, 5):
        for z in (6.0, 9.5, 12.0):
            M['escuro'].beam((x, H, z), (x, H - 1.2, z), 0.006, 0.006)
            M['escuro'].frustum((x, z), 0.07, 0.015, H - 1.45, H - 1.2, seg=10)
            M['luz'].cylinder((x, z), 0.05, H - 1.46, H - 1.45, seg=10)
    # arandelas nas paredes, quadros em molduras douradas, porta do fundo, cadeiras e vasos
    for i, z in enumerate((3.0, 6.5, 10.0)):
        for sx in (-1, 1):
            x = sx * (W / 2 - 0.02)
            M['cristal'].sphere((x - sx * 0.12, 3.0, z), 0.16, 10, sy=0.5)
            lights.append((x - sx * 0.3, 3.0, z))
            # quadro
            q = rng.randrange(4)
            fx0, fx1, fy0, fy1 = z + 0.9, z + 2.1, 1.3, 2.9
            M['quadro'].quad((x, fy0, fx0 if sx < 0 else fx1), (x, fy0, fx1 if sx < 0 else fx0),
                             (x, fy1, fx1 if sx < 0 else fx0), (x, fy1, fx0 if sx < 0 else fx1),
                             uv=((q / 4, 1), ((q + 1) / 4, 1), ((q + 1) / 4, 0), (q / 4, 0)), face=(-sx, 0, 0))
            for (a0, a1, b0, b1) in ((fy0 - 0.1, fy0, fx0 - 0.1, fx1 + 0.1), (fy1, fy1 + 0.1, fx0 - 0.1, fx1 + 0.1),
                                     (fy0, fy1, fx0 - 0.1, fx0), (fy0, fy1, fx1, fx1 + 0.1)):
                abox(M['ouro'], (min(x, x - sx * 0.06), a0, b0), (max(x, x - sx * 0.06), a1, b1))
    abox(M['moldura_branca'], (-0.85, 0, D - 0.03), (0.85, 2.75, D - 0.005))  # batente
    abox(M['madeira'], (-0.7, 0, D - 0.07), (0.7, 2.6, D - 0.03))  # porta do fundo, na frente do batente
    abox(M['ouro'], (0.5, 1.0, D - 0.1), (0.55, 1.15, D - 0.07))  # maçaneta
    for (x, z) in ((-3.5, 9.0), (-2.6, 9.0), (3.5, 9.0)):  # cadeiras estofadas
        abox(M['veludo'], (x - 0.25, 0.45, z - 0.25), (x + 0.25, 0.55, z + 0.25))
        abox(M['veludo'], (x - 0.25, 0.55, z + 0.18), (x + 0.25, 1.1, z + 0.25))
        for dx in (-0.22, 0.22):
            for dz in (-0.22, 0.22):
                M['madeira'].beam((x + dx, 0, z + dz), (x + dx, 0.45, z + dz), 0.035, 0.035)
    for (x, z) in ((-5.4, 1.0), (5.4, 1.0), (-1.5, D - 0.5), (1.5, D - 0.5)):  # vasos com folhagem
        M['escuro'].frustum((x, z), 0.25, 0.35, 0.0, 1.0, seg=14)
        for _ in range(18):
            a = rng.uniform(0, 2 * math.pi)
            M['planta'].beam((x, 1.0, z), (x + 0.5 * math.cos(a), 1.6 + rng.uniform(0, 0.6), z + 0.5 * math.sin(a)), 0.08, 0.01)
    # ---- materiais
    t_dam = os.path.relpath(textura_damasco(tex('damasco.png')), out)
    qt = Image.new('RGB', (1024, 352))
    for q in range(4):
        qt.paste(Image.open(textura_quadro(tex(f'_quadro{q}.png'), seed + q)), (q * 256, 0))
    qt.save(tex('quadros.jpg'), quality=90)
    for q in range(4):
        os.remove(tex(f'_quadro{q}.png'))
    mats = {
        'piso': g.material('marmore', tex=os.path.relpath(textura_marmore(tex('marmore.png')), out), rough=0.08),
        'losango': g.material('losango', color=(0.02, 0.02, 0.025), rough=0.1),
        'parede': g.material('damasco', tex=t_dam, rough=0.7),
        'teto': g.material('teto', color=(0.05, 0.05, 0.06), rough=0.8),
        'madeira': pbr('madeira_escura', 'dark_wood', tint=(0.5, 0.42, 0.38), rough=0.4),
        'vidro': vidro(g, 'vidro_vitrine'),
        'bandeja': g.material('bandeja', color=(0.85, 0.85, 0.83), rough=0.8),
        'ouro': g.material('ouro', color=(1.0, 0.78, 0.34), rough=0.18, metal=1.0),
        'prata': g.material('prata', color=(0.95, 0.95, 0.96), rough=0.12, metal=1.0),
        'gema_r': g.material('rubi', color=(0.9, 0.05, 0.1), rough=0.02, emissive=(0.3, 0.0, 0.02), strength=1.0),
        'gema_b': g.material('safira', color=(0.1, 0.4, 0.95), rough=0.02, emissive=(0.0, 0.05, 0.2), strength=1.0),
        'gema_w': g.material('diamante', color=(0.95, 0.97, 1.0), rough=0.01, emissive=(0.15, 0.15, 0.18), strength=1.0),
        'cristal': g.material('cristal', color=(1, 1, 1), rough=0.05, emissive=(1.0, 0.95, 0.85), strength=6.0),
        'luz': g.material('luz_pendente', color=(1, 1, 1), emissive=(1.0, 0.95, 0.88), strength=30.0),
        'quadro': g.material('quadros', tex=os.path.relpath(tex('quadros.jpg'), out), rough=0.5),
        'escuro': g.material('escuro', color=(0.03, 0.03, 0.035), rough=0.4),
        'veludo': pbr('veludo', 'velour_velvet', tint=(0.9, 0.88, 0.82)),
        'planta': g.material('planta', color=(0.06, 0.2, 0.06), rough=0.6, double=True),
        'moldura_branca': g.material('moldura_branca', color=(0.85, 0.85, 0.83), rough=0.4),
        'luz_vitrine': g.material('luz_vitrine', color=(1, 1, 1), emissive=(0.95, 0.97, 1.0), strength=40.0),
    }
    ids = [g.add_mesh('joalheria_' + k, mesh, mats[k], scene_node=not embed) for k, mesh in M.items()]
    luzes = [((x, y, z), (1.0, 0.96, 0.9), 90.0, 8.0) for (x, y, z) in lights]
    if embed:
        return [i for i in ids if i is not None], luzes, W
    for pos, col, inten, rr in luzes:
        g.light(pos, col, inten, rr)
    path = os.path.join(out, 'joalheria.gltf')
    g.write(path)
    print(path, sum(m.nv for m in M.values()), 'vértices')
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--saida', default=os.path.join('cenas', 'interiores'))
    ap.add_argument('--tipo', default='todos')
    ap.add_argument('--semente', type=int, default=7)
    ap.add_argument('--texturas', default='/home/user/p3/downloads/ph')
    a = ap.parse_args()
    baixar_texturas(a.texturas)
    import times_square
    for pid in PH_EXTRA:  # as texturas extras dos interiores, do mesmo jeito
        if pid not in times_square.PH_IDS:
            times_square.PH_IDS.append(pid)
    baixar_texturas(a.texturas)
    os.makedirs(a.saida, exist_ok=True)
    if a.tipo in ('conveniencia', 'todos'):
        conveniencia(a.saida, a.texturas, a.semente)
    if a.tipo in ('joalheria', 'todos'):
        joalheria(a.saida, a.texturas, a.semente)


if __name__ == '__main__':
    main()
