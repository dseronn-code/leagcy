#!/usr/bin/env python3
"""Gera a Times Square (cenas/times_square/times_square.gltf): ruas, calçadas, praças, prédios, telões e letreiros.
Uso (de engine/):

    python3 tools/times_square.py [--saida cenas/times_square] [--semente 42]

Sem dados de mapa (o OpenStreetMap não é alcançável daqui), o traçado sai da geometria conhecida da região:
  - Grade de Manhattan sem a rotação de 29° (à noite ela não importa): x = leste, -z = norte (uptown). A 7th Ave tem
    o eixo em x = 0; as ruas numeradas ficam a 80 m uma da outra (a 45th em z = 0). Ruas com 18 m de largura (pista de
    10 m e calçadas de 4 m); a 42nd com 30 m; avenidas com 30 m (pista de 18 m).
  - A Broadway corta a grade na diagonal (x = 0,12·z: a leste da 7th Ave na 42nd, a oeste na 47th), cruzando a 7th Ave
    perto da 45th: a "gravata-borboleta". Da 42nd à 47th ela é praça de pedestres (piso de granito, sem carros).
  - Quarteirões em lotes de 8 a 40 m de frente; prédios com base (pódio) de 15 a 45 m e, atrás, torres de até 250 m.
  - Nos pódios que dão para a praça, telões de LED (zoneamento: um telão grande a cada ~15 m de fachada), com anúncios
    de marcas FICTÍCIAS gerados aqui; letreiros de neon; janelas acesas com cortina/sombra nas torres.
  - Arquibancada vermelha iluminada na Duffy Square (47th), torre de telões empilhados na ponta sul (One Times Square
    estilizada), postes de luz, faixas de pedestre, meio-fio de 15 cm.
Prédios são estilizados (massas e proporções parecidas, fachadas próprias): nada de cópia de arquitetura nem de marca.
"""
import argparse
import json
import math
import os
import random

import mapbox_earcut as earcut
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from shapely.geometry import LineString, MultiPolygon, Point, Polygon, box
from shapely.ops import unary_union

STREET = 80.0                     # m entre ruas numeradas
AVE_X = {'8th': -240.0, '7th': 0.0, '6th': 270.0}
BWAY_K = 0.12                     # Broadway: x = k·z
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
FONT2 = '/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf'


def polys(geom):
    """Lista plana de polígonos de qualquer geometria do shapely (coleções, multipolígonos, linhas descartadas)."""
    if geom is None or geom.is_empty:
        return []
    if isinstance(geom, Polygon):
        return [geom]
    if hasattr(geom, 'geoms'):
        return [p for g in geom.geoms for p in polys(g)]
    return []


def street_z(n):
    return -(n - 45) * STREET


def street_width(n):
    return 30.0 if n == 42 else 18.0


# ---------------------------------------------------------------------------------------------------------- malha
class Mesh:
    def __init__(self):
        self.p, self.n, self.uv, self.idx = [], [], [], []

    def quad(self, a, b, c, d, uv=((0, 0), (1, 0), (1, 1), (0, 1)), face=None):
        """Quadrilátero a-b-c-d (anti-horário visto de fora). face: para onde a frente deve apontar (vira a ordem se
        preciso)."""
        a, b, c, d = (np.asarray(x, np.float64) for x in (a, b, c, d))
        nrm = np.cross(b - a, d - a)
        nrm /= max(np.linalg.norm(nrm), 1e-9)
        if face is not None and np.dot(nrm, face) < 0:
            a, b, c, d = a, d, c, b
            uv = (uv[0], uv[3], uv[2], uv[1])
            nrm = -nrm
        base = len(self.p)
        for q, t in zip((a, b, c, d), uv):
            self.p.append(q)
            self.n.append(nrm)
            self.uv.append(t)
        self.idx += [base, base + 1, base + 2, base, base + 2, base + 3]

    def polygon(self, poly, y, up=True, uv_scale=1.0):
        """Polígono horizontal (shapely) na altura y, triangulado (com furos)."""
        for pg in polys(poly):
            if pg.is_empty or pg.area < 1e-3:
                continue
            rings = [np.array(pg.exterior.coords)[:-1]] + [np.array(r.coords)[:-1] for r in pg.interiors]
            verts = np.concatenate(rings)
            ends = np.cumsum([len(r) for r in rings]).astype(np.uint32)
            tri = earcut.triangulate_float64(verts, ends).reshape(-1, 3)
            base = len(self.p)
            for x, z in verts:
                self.p.append(np.array([x, y, z]))
                self.n.append(np.array([0.0, 1.0 if up else -1.0, 0.0]))
                self.uv.append((x * uv_scale, z * uv_scale))
            for t in tri:
                # earcut no plano xz: orientação para a normal +y (x, z -> anti-horário visto de cima = z para baixo)
                a, b, c = (base + int(i) for i in t)
                pa, pb, pc = self.p[a], self.p[b], self.p[c]
                ny = (pb[2] - pa[2]) * (pc[0] - pa[0]) - (pb[0] - pa[0]) * (pc[2] - pa[2])
                if (ny > 0) != up:
                    b, c = c, b
                self.idx += [a, b, c]

    def extrude_walls(self, poly, y0, y1, uv_w=1.0, uv_h=1.0):
        """Paredes verticais do contorno de um polígono, de y0 a y1 (normal para fora)."""
        for pg in polys(poly):
            ring = pg.exterior
            pts = np.array(ring.coords)
            ccw = ring.is_ccw
            for i in range(len(pts) - 1):
                (x0, z0), (x1, z1) = pts[i], pts[i + 1]
                if not ccw:  # contorno no sentido em que "fora" fica à direita do segmento visto de cima
                    pass
                L = math.hypot(x1 - x0, z1 - z0)
                if L < 1e-3:
                    continue
                a, b = (x0, z0), (x1, z1)
                # Em xz com y para cima, um anel anti-horário (shapely, x→z) tem o lado de fora à direita de a→b.
                if ccw:
                    a, b = b, a
                self.quad((a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1]),
                          uv=((0, y0 * uv_h), (L * uv_w, y0 * uv_h), (L * uv_w, y1 * uv_h), (0, y1 * uv_h)))

    def cylinder(self, c, r, y0, y1, seg=12, cap=True):
        """Cilindro vertical de raio r em c=(x, z), de y0 a y1 (com tampa em cima)."""
        x, z = c
        ang = [2 * math.pi * i / seg for i in range(seg + 1)]
        for i in range(seg):
            a0, a1 = ang[i], ang[i + 1]
            p0 = (x + r * math.cos(a0), z + r * math.sin(a0))
            p1 = (x + r * math.cos(a1), z + r * math.sin(a1))
            mid = ((a0 + a1) / 2)
            self.quad((p0[0], y0, p0[1]), (p1[0], y0, p1[1]), (p1[0], y1, p1[1]), (p0[0], y1, p0[1]),
                      uv=((i / seg, y0), ((i + 1) / seg, y0), ((i + 1) / seg, y1), (i / seg, y1)),
                      face=(math.cos(mid), 0, math.sin(mid)))
            if cap:
                base = len(self.p)
                for q in ((x, y1, z), (p0[0], y1, p0[1]), (p1[0], y1, p1[1])):
                    self.p.append(np.array(q, np.float64))
                    self.n.append(np.array([0.0, 1.0, 0.0]))
                    self.uv.append((q[0], q[2]))
                self.idx += [base, base + 2, base + 1]

    def sphere(self, c, r, seg=10):
        cx, cy, cz = c
        for j in range(seg // 2):
            t0, t1 = math.pi * j / (seg // 2) - math.pi / 2, math.pi * (j + 1) / (seg // 2) - math.pi / 2
            for i in range(seg):
                a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
                P = lambda t, a: (cx + r * math.cos(t) * math.cos(a), cy + r * math.sin(t), cz + r * math.cos(t) * math.sin(a))
                tm, am = (t0 + t1) / 2, (a0 + a1) / 2
                self.quad(P(t0, a0), P(t0, a1), P(t1, a1), P(t1, a0),
                          face=(math.cos(tm) * math.cos(am), math.sin(tm), math.cos(tm) * math.sin(am)))

    def box(self, c, h, rot=0.0):
        """Caixa de centro c e meias-medidas h, girada rot rad em torno de y."""
        cx, cy, cz = c
        hx, hy, hz = h
        cs, sn = math.cos(rot), math.sin(rot)
        P = lambda x, y, z: (cx + x * cs - z * sn, cy + y, cz + x * sn + z * cs)
        v = [P(sx * hx, sy * hy, sz * hz) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
        # índices: (sx, sy, sz) -> 4*ix + 2*iy + iz
        V = lambda ix, iy, iz: v[4 * ix + 2 * iy + iz]
        self.quad(V(0, 0, 1), V(1, 0, 1), V(1, 1, 1), V(0, 1, 1))  # +z
        self.quad(V(1, 0, 0), V(0, 0, 0), V(0, 1, 0), V(1, 1, 0))  # -z
        self.quad(V(1, 0, 1), V(1, 0, 0), V(1, 1, 0), V(1, 1, 1))  # +x
        self.quad(V(0, 0, 0), V(0, 0, 1), V(0, 1, 1), V(0, 1, 0))  # -x
        self.quad(V(0, 1, 1), V(1, 1, 1), V(1, 1, 0), V(0, 1, 0))  # +y
        self.quad(V(0, 0, 0), V(1, 0, 0), V(1, 0, 1), V(0, 0, 1))  # -y


# ---------------------------------------------------------------------------------------------------------- texturas
def ad_texture(path, rng, w=512, h=256, kind=None):
    """Anúncio fictício: fundo em degradê/forma, marca inventada e chamada. Nada de marca real."""
    brands = ['NOVA', 'KYRO', 'LUMEN', 'ORBITA', 'VELA', 'ZENTO', 'AURA', 'PIXEL&CO', 'MAREA', 'SOLARA', 'TEMPO',
              'BRISA', 'QUASAR', 'ONDA', 'VOLTA', 'NEXA', 'FAROL', 'ECLIPSE', 'RUMO', 'ATLAS']
    tags = ['NOW PLAYING', 'NEW SEASON', 'LIVE TONIGHT', 'OPEN 24H', 'SUMMER SALE', 'THE MUSICAL', 'ON SALE NOW',
            'JUST LANDED', 'STREAM IT', 'COMING SOON', 'TASTE THE NIGHT', 'FEEL THE CITY']
    pal = [((255, 40, 90), (40, 0, 120)), ((0, 200, 255), (0, 30, 80)), ((255, 200, 0), (200, 40, 0)),
           ((120, 255, 120), (0, 60, 40)), ((255, 255, 255), (220, 0, 0)), ((255, 120, 0), (30, 0, 60)),
           ((180, 0, 255), (0, 0, 30)), ((255, 230, 200), (90, 30, 10))]
    a, b = pal[rng.randrange(len(pal))]
    y = np.linspace(0, 1, h)[:, None, None]
    x = np.linspace(0, 1, w)[None, :, None]
    t = np.clip(0.6 * y + 0.4 * x + 0.15 * np.sin(6 * x + 3 * y + rng.random() * 6), 0, 1)
    img = (np.array(a)[None, None] * (1 - t) + np.array(b)[None, None] * t).astype(np.uint8)
    im = Image.fromarray(img, 'RGB')
    d = ImageDraw.Draw(im)
    for _ in range(rng.randrange(1, 4)):  # formas
        cx, cy, r = rng.randrange(w), rng.randrange(h), rng.randrange(30, 160)
        col = tuple(int(c) for c in (np.array(a) * rng.random() + 255 * (1 - rng.random()) * 0.3).clip(0, 255))
        if rng.random() < 0.5:
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
        else:
            d.polygon([(cx, cy - r), (cx + r, cy + r), (cx - r, cy + r)], fill=col)
    brand = rng.choice(brands)
    f1 = ImageFont.truetype(FONT if rng.random() < 0.7 else FONT2, int(h * rng.uniform(0.28, 0.42)))
    f2 = ImageFont.truetype(FONT, int(h * 0.11))
    tw = d.textlength(brand, font=f1)
    if tw > w * 0.9:
        f1 = ImageFont.truetype(FONT, int(h * 0.28 * w * 0.9 / tw))
        tw = d.textlength(brand, font=f1)
    tx, ty = (w - tw) / 2, h * rng.uniform(0.18, 0.35)
    d.text((tx + 4, ty + 4), brand, font=f1, fill=(0, 0, 0))
    d.text((tx, ty), brand, font=f1, fill=(255, 255, 255))
    tag = rng.choice(tags)
    d.text(((w - d.textlength(tag, font=f2)) / 2, h * 0.75), tag, font=f2, fill=(255, 255, 255))
    im.save(path)
    return path


def ad_flipbook(path, rng, cols=4, rows=2, w=384, h=192):
    """Anúncio animado: `cols` x `rows` quadros numa folha (o motor anda por eles, extras legacy_flipbook). Movimento
    por quadro: o degradê gira, as formas deslizam e crescem, a marca entra deslizando e a chamada pisca."""
    brands = ['NOVA', 'KYRO', 'LUMEN', 'ORBITA', 'VELA', 'ZENTO', 'AURA', 'PIXEL&CO', 'MAREA', 'SOLARA', 'TEMPO',
              'BRISA', 'QUASAR', 'ONDA', 'VOLTA', 'NEXA', 'FAROL', 'ECLIPSE', 'RUMO', 'ATLAS']
    tags = ['NOW PLAYING', 'NEW SEASON', 'LIVE TONIGHT', 'OPEN 24H', 'SUMMER SALE', 'THE MUSICAL', 'ON SALE NOW',
            'JUST LANDED', 'STREAM IT', 'COMING SOON', 'TASTE THE NIGHT', 'FEEL THE CITY']
    pal = [((255, 40, 90), (40, 0, 120)), ((0, 200, 255), (0, 30, 80)), ((255, 200, 0), (200, 40, 0)),
           ((120, 255, 120), (0, 60, 40)), ((255, 255, 255), (220, 0, 0)), ((255, 120, 0), (30, 0, 60)),
           ((180, 0, 255), (0, 0, 30)), ((255, 230, 200), (90, 30, 10))]
    a, b = (np.array(c, np.float64) for c in pal[rng.randrange(len(pal))])
    brand, tag = rng.choice(brands), rng.choice(tags)
    font_path = FONT if rng.random() < 0.7 else FONT2
    shapes = [(rng.random(), rng.random(), rng.uniform(0.15, 0.45), rng.random() < 0.5, rng.uniform(-0.3, 0.3),
               rng.uniform(-0.15, 0.15)) for _ in range(rng.randrange(1, 4))]
    phase0 = rng.random() * 6
    sheet = Image.new('RGB', (w * cols, h * rows))
    n = cols * rows
    yy = np.linspace(0, 1, h)[:, None, None]
    xx = np.linspace(0, 1, w)[None, :, None]
    for f in range(n):
        t = f / n
        ph = phase0 + 2 * math.pi * t
        mix = np.clip(0.6 * yy + 0.4 * xx + 0.25 * np.sin(6 * xx + 3 * yy + ph), 0, 1)
        img = Image.fromarray((a[None, None] * (1 - mix) + b[None, None] * mix).astype(np.uint8), 'RGB')
        d = ImageDraw.Draw(img)
        for (sx, sy, sr, circ, vx, vy) in shapes:
            cx, cy = (sx + vx * math.sin(2 * math.pi * t)) * w, (sy + vy * math.cos(2 * math.pi * t)) * h
            r = sr * h * (1 + 0.15 * math.sin(2 * math.pi * t * 2))
            col = tuple(int(c) for c in np.clip(a * 0.6 + 255 * 0.35, 0, 255))
            if circ:
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
            else:
                d.polygon([(cx, cy - r), (cx + r, cy + r), (cx - r, cy + r)], fill=col)
        size = int(h * 0.36)
        f1 = ImageFont.truetype(font_path, size)
        tw = d.textlength(brand, font=f1)
        if tw > w * 0.9:
            f1 = ImageFont.truetype(font_path, int(size * w * 0.9 / tw))
            tw = d.textlength(brand, font=f1)
        slide = max(0.0, 1.0 - t * 3.0)  # a marca entra deslizando no começo do ciclo
        tx, ty = (w - tw) / 2 + slide * w * 0.6, h * 0.2
        d.text((tx + 3, ty + 3), brand, font=f1, fill=(0, 0, 0))
        d.text((tx, ty), brand, font=f1, fill=(255, 255, 255))
        if f % 2 == 0 or n < 4:  # a chamada pisca
            f2 = ImageFont.truetype(FONT, int(h * 0.12))
            d.text(((w - d.textlength(tag, font=f2)) / 2, h * 0.74), tag, font=f2, fill=(255, 255, 255))
        sheet.paste(img, ((f % cols) * w, (f // cols) * h))
    sheet.save(path)
    return path


def neon_texture(path, rng, w=512, h=128):
    words = ['BAR', 'DELI', 'PIZZA', 'HOTEL', 'THEATRE', 'GIFTS', 'OPEN', 'DINER', 'TICKETS', 'CAFE', 'NOODLES',
             'JAZZ CLUB', 'KARAOKE', 'BURGERS', 'SHOW TONIGHT', 'SOUVENIRS']
    col = rng.choice([(255, 40, 120), (0, 220, 255), (255, 170, 0), (120, 255, 80), (255, 60, 30), (200, 80, 255)])
    im = Image.new('RGB', (w, h), (6, 4, 8))
    d = ImageDraw.Draw(im)
    word = rng.choice(words)
    f = ImageFont.truetype(FONT2 if rng.random() < 0.5 else FONT, int(h * 0.62))
    tw = d.textlength(word, font=f)
    if tw > w * 0.92:
        f = ImageFont.truetype(FONT, int(h * 0.62 * w * 0.92 / tw))
        tw = d.textlength(word, font=f)
    glow = Image.new('RGB', (w, h), (0, 0, 0))
    ImageDraw.Draw(glow).text(((w - tw) / 2, h * 0.12), word, font=f, fill=col)
    glow = glow.filter(ImageFilter.GaussianBlur(6))
    im = Image.fromarray(np.maximum(np.asarray(im), np.asarray(glow)))
    d = ImageDraw.Draw(im)
    d.text(((w - tw) / 2, h * 0.12), word, font=f, fill=tuple(min(255, c + 120) for c in col))
    im.save(path)
    return path


def window_texture(path, rng, lit_frac, style, w=256, h=256):
    """Fachada noturna (4x4 janelas por ladrilho): cor base com janelas; emissiva = só as janelas acesas."""
    base = {'pedra': (88, 78, 70), 'tijolo': (92, 52, 40), 'vidro': (18, 26, 34), 'concreto': (110, 108, 104)}[style]
    img = np.zeros((h, w, 3), np.uint8)
    img[:] = base
    emi = np.zeros((h, w, 3), np.uint8)
    cw, ch = w // 4, h // 4
    for j in range(4):
        for i in range(4):
            x0, y0 = i * cw + cw // 6, j * ch + ch // 5
            x1, y1 = (i + 1) * cw - cw // 6, (j + 1) * ch - ch // 6
            if style == 'vidro':
                x0, x1, y0, y1 = i * cw + 1, (i + 1) * cw - 1, j * ch + 2, (j + 1) * ch - 2
            img[y0:y1, x0:x1] = (12, 14, 18)
            if rng.random() < lit_frac:
                warm = rng.choice([(255, 214, 160), (255, 236, 200), (210, 230, 255), (255, 190, 120)])
                k = rng.uniform(0.35, 1.0)
                c = tuple(int(v * k) for v in warm)
                img[y0:y1, x0:x1] = c
                emi[y0:y1, x0:x1] = c
                if rng.random() < 0.4:  # persiana/cortina pela metade
                    yy = y0 + int((y1 - y0) * rng.uniform(0.2, 0.7))
                    img[y0:yy, x0:x1] = (40, 36, 30)
                    emi[y0:yy, x0:x1] = (24, 20, 16)
    Image.fromarray(img).save(path + '_cor.png')
    Image.fromarray(emi).save(path + '_luz.png')
    return path + '_cor.png', path + '_luz.png'


def tile_texture(path, kind, w=256):
    """Asfalto, calçada (placas de concreto) e granito da praça (com os discos de aço da praça real, estilizados)."""
    rng = np.random.default_rng({'asfalto': 1, 'calcada': 2, 'granito': 3}[kind])
    n = rng.normal(0, 1, (w, w))
    if kind == 'asfalto':
        v = 38 + 10 * n
        img = np.stack([v, v, v * 1.02], -1)
    elif kind == 'calcada':
        v = 120 + 8 * n
        img = np.stack([v, v * 0.98, v * 0.95], -1)
        img[::w // 2, :] = 70
        img[:, ::w // 2] = 70
    else:
        v = 92 + 14 * n
        img = np.stack([v * 0.95, v * 0.95, v], -1)
        img[::w // 4, :] = 55
        img[:, ::w // 4] = 55
        yy, xx = np.mgrid[0:w, 0:w]
        for cx, cy in ((w * 0.25, w * 0.25), (w * 0.75, w * 0.75)):
            m = (xx - cx) ** 2 + (yy - cy) ** 2 < (w * 0.05) ** 2
            img[m] = (150, 150, 155)
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
    return path


def stone_texture(path, kind, w=256):
    """Parede: pedra (blocos com juntas), tijolo (fiadas) ou concreto; ladrilhável, ~2 m por repetição."""
    rng = np.random.default_rng({'pedra': 11, 'tijolo': 12, 'concreto': 13, 'cornija': 14}[kind])
    n = rng.normal(0, 1, (w, w))
    blur = lambda a, k: np.real(np.fft.ifft2(np.fft.fft2(a) * np.exp(-(np.add.outer(np.fft.fftfreq(w) ** 2,
                                                                                         np.fft.fftfreq(w) ** 2)) * k)))
    big = blur(rng.normal(0, 1, (w, w)), 2000.0)
    big /= np.abs(big).max() + 1e-9
    yy, xx = np.mgrid[0:w, 0:w]
    if kind == 'pedra':
        base = np.array([150, 138, 120])
        joint = ((yy % (w // 4)) < 2) | (((xx + (yy // (w // 4)) % 2 * (w // 4)) % (w // 2)) < 2)
    elif kind == 'tijolo':
        base = np.array([132, 62, 46])
        joint = ((yy % (w // 16)) < 2) | (((xx + (yy // (w // 16)) % 2 * (w // 16)) % (w // 8)) < 2)
    elif kind == 'cornija':
        base = np.array([175, 168, 155])
        joint = np.zeros((w, w), bool)
    else:
        base = np.array([128, 126, 122])
        joint = ((yy % (w // 2)) < 1) | ((xx % (w // 2)) < 1)
    img = base[None, None] * (1 + 0.06 * n[..., None] + 0.12 * big[..., None])
    img[joint] *= 0.6
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
    return path


def room_texture(path, rng, lit, w=128, h=128):
    """Interior visto pela janela: parede do fundo, teto com luminária, móveis em silhueta (ou sala apagada)."""
    if lit:
        wall = np.array(rng.choice([(235, 210, 170), (220, 225, 235), (250, 225, 190), (200, 215, 230), (245, 200, 150)]))
        k = rng.uniform(0.45, 1.0)
    else:
        wall = np.array([20, 24, 32])
        k = 1.0
    y = np.linspace(0, 1, h)[:, None]
    shade = 0.55 + 0.45 * np.exp(-((y - 0.15) / 0.35) ** 2)  # luz que vem do teto
    img = np.ones((h, w, 3)) * wall[None, None] * shade[..., None] * k
    if lit:
        img[int(h * 0.04):int(h * 0.09), int(w * 0.2):int(w * 0.8)] = 255  # luminária
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    dark = (25, 22, 20) if lit else (8, 9, 12)
    for _ in range(rng.randrange(1, 4)):  # mesas, estantes, cadeiras
        x0 = rng.randrange(0, w - 20)
        ww, hh = rng.randrange(14, 50), rng.randrange(10, 60)
        d.rectangle([x0, h - hh - rng.randrange(0, 10), x0 + ww, h], fill=dark)
    if lit and rng.random() < 0.35:  # alguém na sala
        cx = rng.randrange(20, w - 20)
        d.ellipse([cx - 6, h * 0.45, cx + 6, h * 0.45 + 12], fill=dark)
        d.rectangle([cx - 9, h * 0.45 + 12, cx + 9, h], fill=dark)
    im.save(path)
    return path


def shop_texture(path, rng, kind, w=256, h=128):
    """Vitrine: loja aberta (interior claro, prateleiras com produtos coloridos, tom quente/frio/colorido) ou fechada
    (porta de aço de enrolar, com pichação leve)."""
    if kind == 'fechada':
        img = np.ones((h, w, 3)) * np.array([70, 72, 76])[None, None]
        img[::6] *= 0.55
        im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
        d = ImageDraw.Draw(im)
        for _ in range(rng.randrange(0, 3)):
            col = tuple(rng.randrange(80, 255) for _ in range(3))
            x, y = rng.randrange(w - 60), rng.randrange(20, h - 30)
            d.text((x, y), rng.choice(['NYC', 'K7', 'ZOE', 'RX', '22']),
                   font=ImageFont.truetype(FONT, rng.randrange(16, 30)), fill=col)
        im.save(path)
        return path
    tint = {'quente': (255, 225, 180), 'fria': (225, 240, 255), 'colorida': rng.choice([(255, 120, 200), (120, 200, 255),
                                                                                        (255, 200, 90), (150, 255, 170)])}[kind]
    img = np.ones((h, w, 3)) * np.array(tint)[None, None] * np.linspace(1.0, 0.55, h)[:, None, None]
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    for row in range(3):
        yb = int(h * (0.35 + 0.22 * row))
        d.rectangle([0, yb, w, yb + 3], fill=(70, 62, 56))
        x = rng.randrange(4)
        while x < w:
            ww = rng.randrange(6, 18)
            col = tuple(rng.randrange(30, 230) for _ in range(3))
            d.rectangle([x, yb - rng.randrange(8, 24), x + ww, yb], fill=col)
            x += ww + rng.randrange(2, 8)
    for _ in range(rng.randrange(0, 4)):  # gente lá dentro (silhuetas)
        cx = rng.randrange(10, w - 10)
        d.ellipse([cx - 5, h * 0.3, cx + 5, h * 0.3 + 10], fill=(30, 26, 24))
        d.rectangle([cx - 8, h * 0.3 + 10, cx + 8, h], fill=(30, 26, 24))
    im.save(path)
    return path


def curtain_texture(path, rng, w=256, h=256):
    """Pele de vidro de torre (4 andares x 8 módulos por ladrilho): vidro escuro reflexivo, faixa de laje opaca,
    algumas salas acesas. Cor + emissiva."""
    img = np.zeros((h, w, 3), np.uint8)
    img[:] = (14, 20, 28)
    emi = np.zeros((h, w, 3), np.uint8)
    fh, mw = h // 4, w // 8
    for j in range(4):
        img[j * fh:j * fh + fh // 5] = (40, 44, 50)  # laje/viga
        for i in range(8):
            if rng.random() < 0.38:
                c = np.array(rng.choice([(255, 236, 205), (225, 238, 255), (255, 215, 160)])) * rng.uniform(0.4, 1.0)
                y0, y1, x0, x1 = j * fh + fh // 5 + 1, (j + 1) * fh - 1, i * mw + 1, (i + 1) * mw - 1
                img[y0:y1, x0:x1] = c * 0.6
                emi[y0:y1, x0:x1] = c
    Image.fromarray(img).save(path + '_cor.png')
    Image.fromarray(emi).save(path + '_luz.png')
    return path + '_cor.png', path + '_luz.png'


class Facades:
    """Fachadas com geometria: janelas recuadas com caixilho e sala atrás, cornijas, vitrines com marquise no térreo,
    pele de vidro com montantes nas torres. As malhas ficam agrupadas por material (self.mesh[nome])."""

    def __init__(self, rng):
        self.rng = rng
        self.mesh = {}

    def m(self, name):
        return self.mesh.setdefault(name, Mesh())

    @staticmethod
    def edges(poly):
        """Lados do contorno: (a, b, comprimento, tangente, normal para fora)."""
        pts = np.array(poly.exterior.coords)
        out = []
        for i in range(len(pts) - 1):
            a, b = pts[i], pts[i + 1]
            L = float(np.hypot(*(b - a)))
            if L < 1.0:
                continue
            t = (b - a) / L
            n = np.array([t[1], -t[0]])
            if poly.contains(Point(*(((a + b) / 2) + n * 0.3))):  # a normal tem que apontar para fora
                n = -n
            out.append((a, b, L, t, n))
        return out

    @staticmethod
    def P(a, t, n, u, y, d):
        return (a[0] + t[0] * u + n[0] * d, y, a[1] + t[1] * u + n[1] * d)

    def wall_quad(self, name, a, t, n, u0, u1, y0, y1, d=0.0, uvs=0.5, unit=False):
        """Retângulo na fachada; unit = UV de 0 a 1 no retângulo (vidro com a sala atrás), senão UV em metros × uvs."""
        P = self.P
        uv = (((0, 1), (1, 1), (1, 0), (0, 0)) if unit else
              ((u0 * uvs, -y0 * uvs), (u1 * uvs, -y0 * uvs), (u1 * uvs, -y1 * uvs), (u0 * uvs, -y1 * uvs)))
        self.m(name).quad(P(a, t, n, u0, y0, d), P(a, t, n, u1, y0, d), P(a, t, n, u1, y1, d), P(a, t, n, u0, y1, d),
                          uv=uv, face=(n[0], 0, n[1]))

    def recess(self, a, t, n, u0, u1, y0, y1, depth, frame):
        """Os 4 lados do vão da janela (da fachada até o vidro)."""
        P, M = self.P, self.m(frame)
        T = (t[0], 0, t[1])
        M.quad(P(a, t, n, u0, y0, 0), P(a, t, n, u0, y0, -depth), P(a, t, n, u1, y0, -depth), P(a, t, n, u1, y0, 0),
               face=(0, 1, 0))  # peitoril
        M.quad(P(a, t, n, u1, y1, 0), P(a, t, n, u1, y1, -depth), P(a, t, n, u0, y1, -depth), P(a, t, n, u0, y1, 0),
               face=(0, -1, 0))  # verga
        M.quad(P(a, t, n, u0, y1, 0), P(a, t, n, u0, y1, -depth), P(a, t, n, u0, y0, -depth), P(a, t, n, u0, y0, 0),
               face=T)
        M.quad(P(a, t, n, u1, y0, 0), P(a, t, n, u1, y0, -depth), P(a, t, n, u1, y1, -depth), P(a, t, n, u1, y1, 0),
               face=(-T[0], 0, -T[2]))

    def bar(self, name, a, t, n, u, y0, y1, w, d0, d1):
        """Montante/caixilho vertical: caixa de largura w, da profundidade d0 à d1."""
        P, M = self.P, self.m(name)
        ua, ub = u - w / 2, u + w / 2
        N, T = (n[0], 0, n[1]), (t[0], 0, t[1])
        M.quad(P(a, t, n, ua, y0, d1), P(a, t, n, ub, y0, d1), P(a, t, n, ub, y1, d1), P(a, t, n, ua, y1, d1), face=N)
        M.quad(P(a, t, n, ub, y0, d1), P(a, t, n, ub, y0, d0), P(a, t, n, ub, y1, d0), P(a, t, n, ub, y1, d1), face=T)
        M.quad(P(a, t, n, ua, y0, d0), P(a, t, n, ua, y0, d1), P(a, t, n, ua, y1, d1), P(a, t, n, ua, y1, d0),
               face=(-T[0], 0, -T[2]))

    def hbar(self, name, a, t, n, u0, u1, y, h, d0, d1):
        """Faixa horizontal saliente (cornija, laje, marquise): de y-h/2 a y+h/2, profundidade d0..d1."""
        P, M = self.P, self.m(name)
        y0, y1 = y - h / 2, y + h / 2
        N = (n[0], 0, n[1])
        M.quad(P(a, t, n, u0, y0, d1), P(a, t, n, u1, y0, d1), P(a, t, n, u1, y1, d1), P(a, t, n, u0, y1, d1), face=N)
        M.quad(P(a, t, n, u0, y1, d1), P(a, t, n, u1, y1, d1), P(a, t, n, u1, y1, d0), P(a, t, n, u0, y1, d0),
               face=(0, 1, 0))
        M.quad(P(a, t, n, u0, y0, d0), P(a, t, n, u1, y0, d0), P(a, t, n, u1, y0, d1), P(a, t, n, u0, y0, d1),
               face=(0, -1, 0))

    def masonry(self, poly, y0, y1, wall, storefront=True):
        """Pódio de alvenaria: térreo de lojas (vitrine + marquise) e andares de janelas recuadas, cornija no alto."""
        rng = self.rng
        ground = 5.0 if storefront else 0.0
        fh = rng.uniform(3.4, 4.0)
        bay = rng.uniform(2.6, 3.4)
        ww = bay * rng.uniform(0.45, 0.62)
        sill, head = rng.uniform(0.8, 1.0), rng.uniform(0.45, 0.7)
        depth = rng.uniform(0.18, 0.3)
        for a, b, L, t, n in self.edges(poly):
            nb = max(1, int(L // bay))
            bw = L / nb
            yb = y0
            if storefront and y1 - y0 > 8:
                # Térreo: vitrines entre pilares, marquise com faixa de luz.
                for i in range(nb):
                    u0, u1 = i * bw + 0.25, (i + 1) * bw - 0.25
                    self.wall_quad(f'vitrine{rng.randrange(6)}', a, t, n, u0, u1, y0 + 0.4, y0 + 3.8, -0.25, unit=True)
                    self.recess(a, t, n, u0, u1, y0 + 0.4, y0 + 3.8, 0.25, 'caixilho')
                    self.wall_quad(wall, a, t, n, i * bw, u0, y0, y0 + ground)
                    self.wall_quad(wall, a, t, n, u1, (i + 1) * bw, y0, y0 + ground)
                    self.wall_quad(wall, a, t, n, u0, u1, y0, y0 + 0.4)
                    self.wall_quad(wall, a, t, n, u0, u1, y0 + 3.8, y0 + ground)
                self.hbar('marquise', a, t, n, 0, L, y0 + 4.0, 0.35, 0.0, 1.4)
                self.hbar('faixa_luz', a, t, n, 0, L, y0 + 3.8, 0.06, 1.35, 1.42)
                yb = y0 + ground
            y = yb
            while y + fh <= y1 - 1.0:
                for i in range(nb):
                    c = (i + 0.5) * bw
                    u0, u1 = c - ww / 2, c + ww / 2
                    wy0, wy1 = y + sill, y + fh - head
                    lit = rng.random() < 0.45
                    self.wall_quad(f'sala{rng.randrange(8)}' if lit else f'sala_apagada{rng.randrange(3)}', a, t, n,
                                   u0, u1, wy0, wy1, -depth, unit=True)
                    self.recess(a, t, n, u0, u1, wy0, wy1, depth, wall)
                    self.bar('caixilho', a, t, n, c, wy0, wy1, 0.05, -depth, -depth + 0.06)
                    self.wall_quad(wall, a, t, n, i * bw, u0, y, y + fh)
                    self.wall_quad(wall, a, t, n, u1, (i + 1) * bw, y, y + fh)
                    self.wall_quad(wall, a, t, n, u0, u1, y, wy0)
                    self.wall_quad(wall, a, t, n, u0, u1, wy1, y + fh)
                if rng.random() < 0.25:  # friso entre andares
                    self.hbar('cornija', a, t, n, 0, L, y + fh, 0.18, 0.0, 0.12)
                y += fh
            self.wall_quad(wall, a, t, n, 0, L, y, y1)  # faixa até o topo
            self.hbar('cornija', a, t, n, -0.6, L + 0.6, y1 - 0.4, 0.8, 0.0, 0.6)
            self.hbar(wall, a, t, n, 0, L, y1 + 0.5, 1.0, -0.2, 0.0)  # platibanda

    def curtain(self, poly, y0, y1, k):
        """Torre de pele de vidro: painéis por módulo (textura com andares) e montantes salientes."""
        mod = self.rng.uniform(1.5, 1.8)
        fh = 4 * 3.8  # o ladrilho da textura tem 4 andares de 3,8 m
        for a, b, L, t, n in self.edges(poly):
            nb = max(1, int(L // mod))
            bw = L / nb
            M = self.m(f'cortina{k}')
            P = self.P
            M.quad(P(a, t, n, 0, y0, 0), P(a, t, n, L, y0, 0), P(a, t, n, L, y1, 0), P(a, t, n, 0, y1, 0),
                   uv=((0, -y0 / fh), (nb / 8.0, -y0 / fh), (nb / 8.0, -y1 / fh), (0, -y1 / fh)), face=(n[0], 0, n[1]))
            for i in range(nb + 1):
                self.bar('montante', a, t, n, i * bw, y0, y1, 0.08, 0.0, 0.22)
            self.hbar('montante', a, t, n, 0, L, y1 + 0.6, 1.2, -0.1, 0.25)


# ---------------------------------------------------------------------------------------------------------- glTF
class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.doc = {'asset': {'version': '2.0', 'generator': 'legacy tools/times_square.py'}, 'buffers': [],
                    'bufferViews': [], 'accessors': [], 'meshes': [], 'nodes': [], 'materials': [], 'textures': [],
                    'images': [], 'samplers': [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}],
                    'scenes': [{'nodes': []}], 'scene': 0, 'extensionsUsed': []}
        self.tex = {}

    def acc(self, arr, typ, comp, target, minmax=False):
        while len(self.bin) % 4:
            self.bin.append(0)
        arr = np.ascontiguousarray(arr)
        self.doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': arr.nbytes,
                                        'target': target})
        self.bin += arr.tobytes()
        a = {'bufferView': len(self.doc['bufferViews']) - 1, 'componentType': comp, 'count': int(arr.shape[0]),
             'type': typ}
        if minmax:
            a['min'] = [float(x) for x in arr.min(0)]
            a['max'] = [float(x) for x in arr.max(0)]
        self.doc['accessors'].append(a)
        return len(self.doc['accessors']) - 1

    def texture(self, uri):
        if uri not in self.tex:
            self.doc['images'].append({'uri': uri})
            self.doc['textures'].append({'source': len(self.doc['images']) - 1, 'sampler': 0})
            self.tex[uri] = len(self.doc['textures']) - 1
        return self.tex[uri]

    def material(self, name, color=(1, 1, 1), rough=0.8, metal=0.0, tex=None, emissive=None, emissive_tex=None,
                 strength=1.0, double=False):
        m = {'name': name, 'pbrMetallicRoughness': {'baseColorFactor': list(color) + [1.0], 'metallicFactor': metal,
                                                     'roughnessFactor': rough}}
        if tex:
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': self.texture(tex)}
        if emissive is not None:
            m['emissiveFactor'] = list(emissive)
            if emissive_tex:
                m['emissiveTexture'] = {'index': self.texture(emissive_tex)}
            if strength != 1.0:
                m['extensions'] = {'KHR_materials_emissive_strength': {'emissiveStrength': strength}}
                if 'KHR_materials_emissive_strength' not in self.doc['extensionsUsed']:
                    self.doc['extensionsUsed'].append('KHR_materials_emissive_strength')
        if double:
            m['doubleSided'] = True
        self.doc['materials'].append(m)
        return len(self.doc['materials']) - 1

    def add_mesh(self, name, mesh, mat):
        if not mesh.idx:
            return
        P = np.array(mesh.p, np.float32)
        N = np.array(mesh.n, np.float32)
        T = np.array(mesh.uv, np.float32)
        I = np.array(mesh.idx, np.uint32)
        prim = {'attributes': {'POSITION': self.acc(P, 'VEC3', 5126, 34962, True), 'NORMAL': self.acc(N, 'VEC3', 5126, 34962),
                               'TEXCOORD_0': self.acc(T, 'VEC2', 5126, 34962)},
                'indices': self.acc(I, 'SCALAR', 5125, 34963), 'material': mat}
        self.doc['meshes'].append({'name': name, 'primitives': [prim]})
        self.doc['nodes'].append({'name': name, 'mesh': len(self.doc['meshes']) - 1})
        self.doc['scenes'][0]['nodes'].append(len(self.doc['nodes']) - 1)

    def light(self, pos, color, intensity, rng_m=40.0):
        ext = self.doc.setdefault('extensions', {}).setdefault('KHR_lights_punctual', {'lights': []})
        if 'KHR_lights_punctual' not in self.doc['extensionsUsed']:
            self.doc['extensionsUsed'].append('KHR_lights_punctual')
        ext['lights'].append({'type': 'point', 'color': list(color), 'intensity': intensity, 'range': rng_m})
        self.doc['nodes'].append({'translation': [float(x) for x in pos],
                                  'extensions': {'KHR_lights_punctual': {'light': len(ext['lights']) - 1}}})
        self.doc['scenes'][0]['nodes'].append(len(self.doc['nodes']) - 1)

    def write(self, path):
        base = os.path.splitext(os.path.basename(path))[0]
        self.doc['buffers'] = [{'uri': base + '.bin', 'byteLength': len(self.bin)}]
        if not self.doc['extensionsUsed']:
            del self.doc['extensionsUsed']
        with open(os.path.join(os.path.dirname(path), base + '.bin'), 'wb') as f:
            f.write(self.bin)
        with open(path, 'w') as f:
            json.dump(self.doc, f, separators=(',', ':'))


# ---------------------------------------------------------------------------------------------------------- traçado
def layout(extent_x=(-300.0, 330.0), streets=range(40, 51)):
    """Polígonos: pistas, calçadas, praça da Broadway e quarteirões (lotes)."""
    zmin, zmax = street_z(streets[-1]) - 60, street_z(streets[0]) + 60
    world = box(extent_x[0], zmin, extent_x[1], zmax)
    roads, walks = [], []
    for n in streets:
        z, w = street_z(n), street_width(n)
        walks.append(box(extent_x[0], z - w / 2, extent_x[1], z + w / 2))
        side = 6.0 if n == 42 else 4.0
        roads.append(box(extent_x[0], z - w / 2 + side, extent_x[1], z + w / 2 - side))
    for x in AVE_X.values():
        walks.append(box(x - 15, zmin, x + 15, zmax))
        roads.append(box(x - 9, zmin, x + 9, zmax))
    bway = LineString([(BWAY_K * zmax, zmax), (BWAY_K * zmin, zmin)])
    bway_row = bway.buffer(15, cap_style=2)
    z42, z47 = street_z(42), street_z(47)
    plaza_zone = box(extent_x[0], z47 - 9, extent_x[1], z42 + 15)
    plaza = bway_row.intersection(plaza_zone)
    bway_road = bway.buffer(9, cap_style=2).difference(plaza_zone)
    roads.append(bway_road)
    walks.append(bway_row)
    road = unary_union(roads).intersection(world)
    row = unary_union(walks).intersection(world)
    plaza = plaza.difference(unary_union(roads[:-1]))  # as ruas transversais continuam cruzando a praça
    sidewalk = row.difference(road).difference(plaza)
    blocks = world.difference(row)
    return road, sidewalk, plaza, blocks, (zmin, zmax)


def lots(block, rng):
    """Divide um quarteirão em lotes: faixas de 8 a 40 m ao longo do lado maior."""
    minx, minz, maxx, maxz = block.bounds
    along_x = (maxx - minx) >= (maxz - minz)
    out = []
    pos = minx if along_x else minz
    end = maxx if along_x else maxz
    while pos < end - 1:
        w = rng.uniform(8, 40)
        if end - (pos + w) < 8:
            w = end - pos
        cut = box(pos, minz, pos + w, maxz) if along_x else box(minx, pos, maxx, pos + w)
        for piece in polys(block.intersection(cut)):
            if piece.area > 30:
                out.append(piece)
        pos += w
    return out


# ---------------------------------------------------------------------------------------------------------- cena
def build(out, seed):
    rng = random.Random(seed)
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    tex = lambda name: os.path.join(out, 'tex', name)
    rel = lambda p: 'tex/' + os.path.basename(p)
    g = Gltf()

    road, sidewalk, plaza, blocks, (zmin, zmax) = layout()
    asphalt = g.material('asfalto', rough=0.55, tex=rel(tile_texture(tex('asfalto.png'), 'asfalto')))
    walk = g.material('calcada', rough=0.7, tex=rel(tile_texture(tex('calcada.png'), 'calcada')))
    granite = g.material('granito', rough=0.5, tex=rel(tile_texture(tex('granito.png'), 'granito')))
    curb = g.material('meio_fio', color=(0.45, 0.44, 0.42), rough=0.7)
    paint = g.material('faixa', color=(0.85, 0.85, 0.8), rough=0.5)

    m = Mesh()
    m.polygon(road, 0.0, uv_scale=0.25)
    g.add_mesh('pista', m, asphalt)
    m = Mesh()
    m.polygon(sidewalk, 0.15, uv_scale=0.5)
    g.add_mesh('calcada', m, walk)
    m = Mesh()
    m.polygon(plaza, 0.15, uv_scale=0.25)
    g.add_mesh('praca', m, granite)
    m = Mesh()
    m.extrude_walls(unary_union([sidewalk, plaza]).buffer(0), 0.0, 0.15)
    g.add_mesh('meio_fio', m, curb)

    # Faixas de pedestre nas esquinas da 7th Ave e linhas da pista.
    m = Mesh()
    for n in range(40, 51):
        z, w = street_z(n), street_width(n)
        for x0 in (-15.0, 15.0):  # faixa atravessando a rua transversal, dos dois lados da 7th
            for k in range(8):
                xs = x0 + (-1 if x0 < 0 else 1) * (1 + k * 1.2)
                m.quad((xs, 0.01, z + w / 2 - 4), (xs + 0.6, 0.01, z + w / 2 - 4), (xs + 0.6, 0.01, z - w / 2 + 4),
                       (xs, 0.01, z - w / 2 + 4))
        for k in range(12):  # faixa atravessando a 7th Ave, ao norte de cada cruzamento
            xs = -9 + k * 1.5
            zc = z - w / 2 - 1.0
            m.quad((xs, 0.01, zc), (xs + 0.6, 0.01, zc), (xs + 0.6, 0.01, zc - 3), (xs, 0.01, zc - 3))
    g.add_mesh('faixas', m, paint)

    # ---- prédios
    facade_styles = ['pedra', 'tijolo', 'vidro', 'concreto']
    facade_mats = {}
    for st in facade_styles:
        for k in range(3):
            c, e = window_texture(tex(f'fachada_{st}{k}'), rng, rng.uniform(0.25, 0.6), st)
            facade_mats[(st, k)] = g.material(f'fachada_{st}{k}', rough=0.3 if st == 'vidro' else 0.85,
                                              metal=0.6 if st == 'vidro' else 0.0, tex=rel(c),
                                              emissive=(0.25, 0.25, 0.25), emissive_tex=rel(e), strength=0.3)
    roof = g.material('telhado', color=(0.18, 0.18, 0.19), rough=0.9)
    fac = Facades(rng)
    walls = {k: f'parede_{k}' for k in ('pedra', 'tijolo', 'concreto')}
    bldg = {k: Mesh() for k in facade_mats}
    roofs = Mesh()
    near_plaza = LineString([(BWAY_K * street_z(41), street_z(41)), (BWAY_K * street_z(48), street_z(48))])
    screens, neons = [], []
    blocks_list = polys(blocks)
    for blk in blocks_list:
        for lot in lots(blk, rng):
            c = lot.centroid
            d = c.distance(near_plaza)
            bowtie = d < 120 and street_z(47) - 20 < c.y < street_z(42) + 20
            podium = rng.uniform(15, 45) if bowtie else rng.uniform(12, 60)
            tower = 0.0
            if rng.random() < (0.55 if bowtie else 0.35):
                tower = podium + rng.uniform(40, 210 if d < 250 else 120)
            st = rng.choice(facade_styles)
            k = rng.randrange(3)
            bl = polys(lot.buffer(-0.3))
            if not bl:
                continue
            body = max(bl, key=lambda q: q.area)
            detailed = d < 260  # perto da praça: fachada com geometria; longe: textura
            if detailed:
                fac.masonry(body, 0.15, podium, walls[rng.choice(['pedra', 'pedra', 'tijolo', 'concreto'])])
            else:
                bldg[(st, k)].extrude_walls(body, 0.15, podium, uv_w=1 / 12.0, uv_h=1 / 14.0)
            top = podium
            if tower > podium:
                cl = polys(body.buffer(-rng.uniform(3, 8)))
                core = max(cl, key=lambda q: q.area) if cl else None
                if core is not None and core.area > 80:
                    st2 = rng.choice(['vidro', 'vidro', 'concreto', 'pedra'])
                    if detailed:
                        fac.curtain(core, podium, tower, rng.randrange(4))
                    else:
                        bldg[(st2, rng.randrange(3))].extrude_walls(core, podium, tower, 1 / 12.0, 1 / 14.0)
                    roofs.polygon(body.difference(core), podium + 0.05)
                    roofs.polygon(core, tower)
                    top = None
            if top is not None:
                roofs.polygon(body, top)
            # Telões e neons nas fachadas voltadas para a praça (bowtie) e nas ruas em volta.
            if bowtie:
                ring = np.array(body.exterior.coords)
                for i in range(len(ring) - 1):
                    a, b = ring[i], ring[i + 1]
                    L = math.hypot(*(b - a))
                    if L < 6:
                        continue
                    mid = (a + b) / 2
                    if LineString([a, b]).distance(near_plaza) > 60:
                        continue
                    nx, nz = (b[1] - a[1]) / L, -(b[0] - a[0]) / L  # normal para fora (anel anti-horário)
                    if not body.exterior.is_ccw:
                        nx, nz = -nx, -nz
                    y = rng.uniform(6, 10)
                    while y < podium - 6:
                        hgt = min(rng.uniform(6, 22), podium - y - 1)
                        wid = min(L * rng.uniform(0.6, 0.95), hgt * rng.uniform(1.4, 3.0))
                        screens.append((mid, (nx, nz), (b - a) / L, y, wid, hgt))
                        y += hgt + rng.uniform(0.5, 3)
                    if rng.random() < 0.7:  # letreiro de neon baixo, na altura da marquise
                        neons.append((mid, (nx, nz), (b - a) / L, 3.6, min(L * 0.8, 9), 1.4))
    for k, mesh in bldg.items():
        g.add_mesh(f'predios_{k[0]}{k[1]}', mesh, facade_mats[k])
    g.add_mesh('telhados', roofs, roof)

    # Materiais das fachadas com geometria.
    fmat = {}
    for k in ('pedra', 'tijolo', 'concreto'):
        fmat[f'parede_{k}'] = g.material(f'parede_{k}', rough=0.85, tex=rel(stone_texture(tex(f'parede_{k}.png'), k)))
    fmat['cornija'] = g.material('cornija', rough=0.75, tex=rel(stone_texture(tex('cornija.png'), 'cornija')))
    fmat['caixilho'] = g.material('caixilho', color=(0.06, 0.06, 0.065), rough=0.35, metal=1.0)
    fmat['montante'] = g.material('montante', color=(0.55, 0.57, 0.6), rough=0.3, metal=1.0)
    fmat['marquise'] = g.material('marquise', color=(0.05, 0.05, 0.055), rough=0.4, metal=1.0)
    fmat['faixa_luz'] = g.material('faixa_luz', color=(1, 1, 1), emissive=(1.0, 0.92, 0.8), strength=25.0)
    for k in range(8):  # vidro com a sala acesa atrás (reflexo do vidro + interior emissivo)
        r = rel(room_texture(tex(f'sala{k}.png'), rng, True))
        fmat[f'sala{k}'] = g.material(f'sala{k}', color=(0.5, 0.5, 0.5), rough=0.05, tex=r, emissive=(1, 1, 1),
                                      emissive_tex=r, strength=0.8)
    for k in range(3):
        r = rel(room_texture(tex(f'sala_apagada{k}.png'), rng, False))
        fmat[f'sala_apagada{k}'] = g.material(f'sala_apagada{k}', rough=0.04, tex=r)
    for k, kind in enumerate(['quente', 'quente', 'fria', 'colorida', 'colorida', 'fechada']):
        r = rel(shop_texture(tex(f'vitrine{k}.png'), rng, kind))
        if kind == 'fechada':
            fmat[f'vitrine{k}'] = g.material(f'vitrine{k}', rough=0.5, metal=0.8, tex=r)
        else:
            fmat[f'vitrine{k}'] = g.material(f'vitrine{k}', color=(0.6, 0.6, 0.6), rough=0.05, tex=r,
                                             emissive=(1, 1, 1), emissive_tex=r, strength=1.2)
    for k in range(4):
        c, e = curtain_texture(tex(f'cortina{k}'), rng)
        fmat[f'cortina{k}'] = g.material(f'cortina{k}', rough=0.06, metal=0.3, tex=rel(c), emissive=(1, 1, 1),
                                         emissive_tex=rel(e), strength=0.6)
    for name, mesh in fac.mesh.items():
        g.add_mesh('fachada_' + name, mesh, fmat[name])

    # Torre de telões empilhados na ponta sul da praça (estilizada; não é cópia do prédio real).
    zt = street_z(42) - 15 - 22
    xt = BWAY_K * zt + 10
    m = Mesh()
    m.box((xt, 55, zt), (9, 55, 14))
    g.add_mesh('torre_sul', m, facade_mats[('concreto', 0)])
    for i, y in enumerate(range(18, 104, 14)):
        screens.append((np.array([xt, zt + 14.3]), (0.0, 1.0), np.array([1.0, 0.0]), y, 16, 11))

    # Telões: 40 anúncios animados fictícios (folhas de 8 quadros) espalhados pelos telões, emissivos fortes (LED:
    # ~6.000 nits). Cada telão sorteia um anúncio e uma velocidade.
    ads = []
    for k in range(40):
        p = rel(ad_flipbook(tex(f'anuncio{k:02d}.png'), rng))
        mat = g.material(f'anuncio{k:02d}', rough=0.35, tex=p, emissive=(1, 1, 1), emissive_tex=p, strength=6.0)
        g.doc['materials'][mat]['extras'] = {'legacy_flipbook': [4, 2, rng.choice([2, 3, 4, 6, 8])]}
        ads.append(mat)
    for i, (mid, (nx, nz), t, y, wid, hgt) in enumerate(screens):
        mat = ads[rng.randrange(len(ads))]
        m = Mesh()
        o = np.array([mid[0] + nx * 0.4, 0, mid[1] + nz * 0.4])
        tx = np.array([t[0], 0, t[1]]) * wid / 2
        A = o - tx + [0, y, 0]
        B = o + tx + [0, y, 0]
        if np.dot(np.cross(B - A, [0, 1, 0]), [nx, 0, nz]) < 0:  # frente para fora da fachada
            A, B = B, A
        m.quad(A, B, B + [0, hgt, 0], A + [0, hgt, 0], uv=((0, 1), (1, 1), (1, 0), (0, 0)))  # v do glTF: 0 em cima
        frame = Mesh()
        g.add_mesh(f'telao{i:03d}', m, mat)
    for i, (mid, (nx, nz), t, y, wid, hgt) in enumerate(neons):
        p = neon_texture(tex(f'neon{i:03d}.png'), rng)
        mat = g.material(f'neon{i:03d}', tex=rel(p), emissive=(1, 1, 1), emissive_tex=rel(p), strength=3.0)
        m = Mesh()
        o = np.array([mid[0] + nx * 0.6, 0, mid[1] + nz * 0.6])
        tx = np.array([t[0], 0, t[1]]) * wid / 2
        A, B = o - tx + [0, y, 0], o + tx + [0, y, 0]
        if np.dot(np.cross(B - A, [0, 1, 0]), [nx, 0, nz]) < 0:
            A, B = B, A
        m.quad(A, B, B + [0, hgt, 0], A + [0, hgt, 0], uv=((0, 1), (1, 1), (1, 0), (0, 0)))  # v do glTF: 0 em cima
        g.add_mesh(f'neon{i:03d}', m, mat)

    # Arquibancada vermelha iluminada (Duffy Square, ao norte da 46th, no meio da gravata).
    zs = street_z(46) - 9 - 18
    xs = BWAY_K * zs
    red = g.material('degraus', color=(0.7, 0.05, 0.05), rough=0.25, emissive=(1.0, 0.08, 0.06), strength=1.5)
    m = Mesh()
    for k in range(14):
        m.box((xs, 0.15 + 0.2 * k + 0.1, zs - 0.45 * k), (7.5, 0.1 + 0.2 * k / 2 + 0.0, 0.45))
    g.add_mesh('arquibancada', m, red)

    # ---- mobiliário urbano: semáforos, hidrantes, lixeiras, frades (pilaretes) na praça, entradas de metrô com os
    # globos verdes, bancas, tampas de bueiro, bancos e floreiras na praça.
    steel = g.material('aco', color=(0.25, 0.26, 0.27), rough=0.35, metal=1.0)
    dark = g.material('pintura_escura', color=(0.03, 0.035, 0.03), rough=0.4)
    yellow = g.material('semaforo', color=(0.55, 0.42, 0.05), rough=0.45)
    red_l = g.material('sinal_vermelho', color=(1, 0.1, 0.05), emissive=(1, 0.08, 0.04), strength=60.0)
    green_l = g.material('sinal_verde', color=(0.1, 1, 0.5), emissive=(0.1, 1.0, 0.55), strength=60.0)
    walk_l = g.material('sinal_pedestre', color=(1, 0.5, 0.1), emissive=(1.0, 0.45, 0.08), strength=40.0)
    hydrant = g.material('hidrante', color=(0.6, 0.05, 0.03), rough=0.5)
    bin_mat = g.material('lixeira', color=(0.06, 0.2, 0.1), rough=0.6, metal=0.5)
    granite_d = g.material('granito_escuro', color=(0.2, 0.2, 0.21), rough=0.35)
    globe = g.material('globo_metro', color=(0.2, 1, 0.4), emissive=(0.25, 1.0, 0.45), strength=30.0)
    wood = g.material('banco', color=(0.32, 0.2, 0.12), rough=0.6)
    plant = g.material('planta', color=(0.06, 0.18, 0.05), rough=0.8)
    stand = g.material('banca', color=(0.08, 0.16, 0.3), rough=0.5, metal=0.3)
    stand_l = g.material('banca_luz', color=(1, 1, 1), emissive=(1, 0.9, 0.7), strength=8.0)
    manhole = g.material('bueiro', color=(0.08, 0.08, 0.08), rough=0.35, metal=1.0)
    props = {k: Mesh() for k in ('aco', 'escuro', 'semaforo', 'vermelho', 'verde', 'pedestre', 'hidrante', 'lixeira',
                                 'granito', 'globo', 'banco', 'planta', 'banca', 'banca_luz', 'bueiro')}
    pr = props
    steam_points = []
    for n in range(41, 49):
        z, w = street_z(n), street_width(n)
        for ax in (AVE_X['7th'], BWAY_K * z):
            for sx in (-1, 1):
                for sz in (-1, 1):  # semáforo em cada canto do cruzamento
                    px, pz = ax + sx * 15.6, z + sz * (w / 2 + 0.6)
                    if abs(ax) < 1e-6 and abs(BWAY_K * z) < 25 and sx < 0:
                        continue
                    pr['escuro'].cylinder((px, pz), 0.11, 0.15, 5.2)
                    arm_dir = -sx
                    pr['escuro'].box((px + arm_dir * 3.2, 5.0, pz), (3.2, 0.08, 0.08))
                    hx = px + arm_dir * 5.8
                    pr['semaforo'].box((hx, 4.4, pz), (0.2, 0.55, 0.18))
                    lit = (n + (1 if sx > 0 else 0)) % 2 == 0
                    (pr['vermelho'] if lit else pr['verde']).sphere((hx, 4.75 if lit else 4.05, pz - sz * 0.19), 0.1, 8)
                    pr['semaforo'].box((px, 2.7, pz - sz * 0.15), (0.17, 0.2, 0.12))
                    pr['pedestre'].box((px, 2.7, pz - sz * 0.27), (0.12, 0.13, 0.005))
        for x in np.arange(-250, 300, 40.0):  # hidrantes e lixeiras nas calçadas
            for zz in (z - w / 2 + 0.7, z + w / 2 - 0.7):
                if any(abs(x - ax) < 20 for ax in AVE_X.values()) or abs(x - BWAY_K * zz) < 20:
                    continue
                xx = x + rng.uniform(-6, 6)
                if rng.random() < 0.5:
                    pr['hidrante'].cylinder((xx, zz), 0.13, 0.15, 0.85)
                    pr['hidrante'].sphere((xx, 0.85, zz), 0.13, 8)
                    pr['hidrante'].box((xx, 0.6, zz), (0.24, 0.05, 0.05))
                else:
                    pr['lixeira'].cylinder((xx, zz), 0.3, 0.15, 1.05, seg=16)
        for x in (-170.0, 130.0):  # bueiros no asfalto (com vapor na cena .json)
            pr['bueiro'].cylinder((x + rng.uniform(-20, 20), z + rng.uniform(-2, 2)), 0.45, 0.0, 0.012, seg=16)
            steam_points.append((float(x), float(z)))
    # Praça: frades de granito na borda (contra carros), bancos e floreiras.
    pl_edge = plaza.boundary
    for k in np.arange(0, pl_edge.length, 2.4):
        p = pl_edge.interpolate(k)
        if road.distance(p) < 1.0 and rng.random() < 0.9:
            pr['granito'].cylinder((p.x, p.y), 0.22, 0.15, 1.0, seg=10)
    for zc in np.arange(street_z(46) + 20, street_z(42) - 20, 14.0):
        if any(abs(zc - street_z(n)) < 16 for n in range(42, 48)):
            continue
        xc = BWAY_K * zc
        for side in (-1, 1):
            if rng.random() < 0.6:
                pr['banco'].box((xc + side * 7.5, 0.6, zc), (0.35, 0.05, 1.8), rot=math.atan(BWAY_K))
                pr['granito'].box((xc + side * 7.5, 0.37, zc), (0.3, 0.22, 1.7), rot=math.atan(BWAY_K))
            else:
                pr['granito'].box((xc + side * 7.0, 0.55, zc), (0.9, 0.4, 0.9))
                pr['planta'].sphere((xc + side * 7.0, 1.25, zc), 0.8, 10)
    # Entradas de metrô (escada com grade e os dois globos verdes acesos) e bancas de jornal.
    for (x, z) in ((-21.0, street_z(42) + 9), (24.0, street_z(42) - 9), (-21.0, street_z(45) + 9), (21.0, street_z(47) - 9)):
        for sx in (-1, 1):
            pr['escuro'].box((x + sx * 1.3, 0.65, z), (0.04, 0.5, 2.6))
            pr['escuro'].cylinder((x + sx * 1.3, z + 2.6), 0.05, 0.15, 2.4)
            pr['globo'].sphere((x + sx * 1.3, 2.6, z + 2.6), 0.2, 10)
            g.light((x + sx * 1.3, 2.6, z + 2.6), (0.3, 1.0, 0.45), 60.0, 12.0)
        pr['escuro'].box((x, 0.65, z - 2.6), (1.3, 0.5, 0.04))
    for (x, z) in ((16.5, street_z(43) - 14), (-17.0, street_z(46) + 20), (-17.5, street_z(44) - 25)):
        pr['banca'].box((x, 1.4, z), (1.2, 1.25, 2.2))
        pr['banca'].box((x + 0.3, 2.8, z), (1.6, 0.06, 2.5))
        pr['banca_luz'].box((x + 1.21, 1.6, z), (0.01, 0.6, 1.9))
    for key, mat in (('aco', steel), ('escuro', dark), ('semaforo', yellow), ('vermelho', red_l), ('verde', green_l),
                     ('pedestre', walk_l), ('hidrante', hydrant), ('lixeira', bin_mat), ('granito', granite_d),
                     ('globo', globe), ('banco', wood), ('planta', plant), ('banca', stand), ('banca_luz', stand_l),
                     ('bueiro', manhole)):
        g.add_mesh('rua_' + key, props[key], mat)
    with open(os.path.join(out, 'vapor.json'), 'w') as f:
        json.dump(steam_points, f)

    # Postes: luz de rua (sódio/LED quente) a cada ~30 m nas calçadas; KHR_lights_punctual.
    pole = g.material('poste', color=(0.08, 0.09, 0.1), rough=0.4, metal=1.0)
    lamp = g.material('lampada', color=(1, 0.9, 0.7), emissive=(1.0, 0.82, 0.55), strength=40.0)
    poles, lamps = Mesh(), Mesh()
    for n in range(40, 51):
        z, w = street_z(n), street_width(n)
        for x in np.arange(-280, 320, 32.0):
            for zz in (z - w / 2 + 0.6, z + w / 2 - 0.6):
                if any(abs(x - ax) < 18 for ax in AVE_X.values()) or abs(x - BWAY_K * zz) < 18:
                    continue
                poles.box((x, 4.5, zz), (0.09, 4.5, 0.09))
                lamps.box((x, 9.0, zz), (0.35, 0.08, 0.2))
                g.light((x, 8.85, zz), (1.0, 0.85, 0.65), 1800.0)
    g.add_mesh('postes', poles, pole)
    g.add_mesh('lampadas', lamps, lamp)

    path = os.path.join(out, 'times_square.gltf')
    g.write(path)
    print(f'{path}: {len(screens)} telões, {len(neons)} neons, {len(g.doc["meshes"])} malhas, '
          f'{len(g.doc.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", []))} postes')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--saida', default=os.path.join('cenas', 'times_square'))
    ap.add_argument('--semente', type=int, default=42)
    ap.add_argument('--gltfpack', help='gltfpack (npm install gltfpack): comprime a malha (meshopt + quantização), '
                                       '~9x menor; sem ele o .bin sai cru (~180 MB)')
    a = ap.parse_args()
    if not a.gltfpack:
        build(a.saida, a.semente)
        return
    import shutil
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        build(tmp, a.semente)
        os.makedirs(a.saida, exist_ok=True)
        subprocess.run([a.gltfpack, '-i', os.path.join(tmp, 'times_square.gltf'), '-o',
                        os.path.join(a.saida, 'times_square.gltf'), '-cc', '-kn', '-km', '-ke'], check=True)
        shutil.copyfile(os.path.join(tmp, 'vapor.json'), os.path.join(a.saida, 'vapor.json'))
        # O gltfpack copia as texturas para o lado do .gltf (e aponta para lá): a pasta tex/ antiga sobra.
        shutil.rmtree(os.path.join(a.saida, 'tex'), ignore_errors=True)


if __name__ == '__main__':
    main()
