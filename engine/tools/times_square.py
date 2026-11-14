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
from array import array
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
        # Vetores planos (array do Python, float32): a cidade tem dezenas de milhões de valores
        self.p, self.n, self.uv, self.c, self.idx = (array('f'), array('f'), array('f'), array('f'), array('I'))
        self.nv = 0
        self.tint = (1.0, 1.0, 1.0)  # cor por vértice (COLOR_0): variação de tom por prédio...
        self.grime = None            # ...e sujeira: (y do chão, altura, força, y do topo) escurece base e topo

    def col(self, q):
        """Cor do vértice: tom do prédio × sujeira (mais escuro junto à calçada, onde respinga, e sob a cornija)."""
        t = self.tint
        if self.grime is None:
            return t
        y0, h, k, ytop = self.grime
        s = min(max((q[1] - y0) / h, 0.0), 1.0)
        f = 1.0 - k * (1.0 - s * s * (3 - 2 * s))
        if ytop is not None:
            s2 = min(max((ytop - q[1]) / 2.5, 0.0), 1.0)
            f *= 1.0 - 0.25 * (1.0 - s2)
        return (t[0] * f, t[1] * f, t[2] * f)

    def vert(self, q, nrm, t):
        q = (float(q[0]), float(q[1]), float(q[2]))
        self.p.extend(q)
        self.n.extend((float(nrm[0]), float(nrm[1]), float(nrm[2])))
        self.uv.extend((float(t[0]), float(t[1])))
        self.c.extend(self.col(q))
        self.nv += 1

    def quad(self, a, b, c, d, uv=((0, 0), (1, 0), (1, 1), (0, 1)), face=None, nrms=None):
        """Quadrilátero a-b-c-d (anti-horário visto de fora). face: para onde a frente deve apontar (vira a ordem se
        preciso). nrms: normais por vértice (superfície curva lisa); senão a normal da face."""
        a, b, c, d = (np.asarray(x, np.float64) for x in (a, b, c, d))
        nrm = np.cross(b - a, d - a)
        nrm /= max(np.linalg.norm(nrm), 1e-9)
        if face is not None and np.dot(nrm, face) < 0:
            a, b, c, d = a, d, c, b
            uv = (uv[0], uv[3], uv[2], uv[1])
            if nrms is not None:
                nrms = (nrms[0], nrms[3], nrms[2], nrms[1])
            nrm = -nrm
        base = self.nv
        for k, (q, t) in enumerate(zip((a, b, c, d), uv)):
            self.vert(q, nrm if nrms is None else nrms[k], t)
        self.idx.extend((base, base + 1, base + 2, base, base + 2, base + 3))

    def tri(self, a, b, c, face, uv=((0, 0), (1, 0), (0, 1))):
        a, b, c = (np.asarray(x, np.float64) for x in (a, b, c))
        nrm = np.cross(b - a, c - a)
        nrm /= max(np.linalg.norm(nrm), 1e-9)
        if np.dot(nrm, face) < 0:
            b, c = c, b
            uv = (uv[0], uv[2], uv[1])
            nrm = -nrm
        base = self.nv
        for q, t in zip((a, b, c), uv):
            self.vert(q, nrm, t)
        self.idx.extend((base, base + 1, base + 2))

    def frustum(self, c, r0, r1, y0, y1, seg=12, cap=True, uvs=None):
        """Tronco de cone vertical liso (poste afunilado, telhado cônico com r1=0). uvs: u em metros × uvs ao redor
        (madeira da caixa d'água); senão 0..1."""
        x, z = c
        U = 1.0 if uvs is None else 2 * math.pi * max(r0, r1) * uvs
        slope = (r0 - r1) / max(y1 - y0, 1e-6)
        for i in range(seg):
            a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
            N = [np.array([math.cos(a), slope, math.sin(a)]) / math.hypot(1, slope) for a in (a0, a1)]
            P = lambda a, r, y: (x + r * math.cos(a), y, z + r * math.sin(a))
            self.quad(P(a0, r0, y0), P(a1, r0, y0), P(a1, r1, y1), P(a0, r1, y1),
                      uv=((U * i / seg, y0), (U * (i + 1) / seg, y0), (U * (i + 1) / seg, y1), (U * i / seg, y1)),
                      face=(math.cos((a0 + a1) / 2), 0, math.sin((a0 + a1) / 2)), nrms=(N[0], N[1], N[1], N[0]))
            if cap and r1 > 0:
                base = self.nv
                for q in ((x, y1, z), P(a0, r1, y1), P(a1, r1, y1)):
                    self.vert(q, (0, 1, 0), (q[0], q[2]))
                self.idx.extend((base, base + 2, base + 1))

    def tube(self, path, r, seg=8, uvs=1.0):
        """Tubo liso ao longo de um caminho 3D (braço de poste, corrimão, aro de caixa d'água)."""
        path = [np.asarray(q, np.float64) for q in path]
        prev = None
        rings = []
        for i, q in enumerate(path):
            tng = path[min(i + 1, len(path) - 1)] - path[max(i - 1, 0)]
            tng /= max(np.linalg.norm(tng), 1e-9)
            if prev is None:
                up = np.array([0.0, 1.0, 0.0]) if abs(tng[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
                u = np.cross(tng, up)
            else:
                u = prev - tng * np.dot(prev, tng)  # transporte paralelo: o anel não torce
            u /= max(np.linalg.norm(u), 1e-9)
            v = np.cross(tng, u)
            prev = u
            rings.append([(q + r * (math.cos(a) * u + math.sin(a) * v), math.cos(a) * u + math.sin(a) * v)
                          for a in (2 * math.pi * k / seg for k in range(seg + 1))])
        L = 0.0
        for i in range(len(path) - 1):
            L1 = L + float(np.linalg.norm(path[i + 1] - path[i]))
            for k in range(seg):
                (p00, n00), (p01, n01) = rings[i][k], rings[i][k + 1]
                (p10, n10), (p11, n11) = rings[i + 1][k], rings[i + 1][k + 1]
                self.quad(p00, p01, p11, p10, uv=((k / seg, L * uvs), ((k + 1) / seg, L * uvs), ((k + 1) / seg, L1 * uvs),
                                                  (k / seg, L1 * uvs)), face=n00 + n11, nrms=(n00, n01, n11, n10))
            L = L1

    def beam(self, p0, p1, w, h, up=(0.0, 1.0, 0.0)):
        """Barra de seção w × h de p0 a p1 (degrau, viga inclinada, grade)."""
        p0, p1 = np.asarray(p0, np.float64), np.asarray(p1, np.float64)
        ax = p1 - p0
        L = float(np.linalg.norm(ax))
        if L < 1e-6:
            return
        ax /= L
        up = np.asarray(up, np.float64)
        sd = np.cross(ax, up)
        if np.linalg.norm(sd) < 1e-6:
            sd = np.cross(ax, np.array([1.0, 0.0, 0.0]))
        sd /= np.linalg.norm(sd)
        upv = np.cross(sd, ax)
        sd, upv = sd * w / 2, upv * h / 2
        c = [p0 - sd - upv, p0 + sd - upv, p0 + sd + upv, p0 - sd + upv]
        e = [q + ax * L for q in c]
        for i in range(4):
            j = (i + 1) % 4
            mid = (c[i] + c[j]) / 2 - p0
            self.quad(c[i], c[j], e[j], e[i], uv=((0, 0), (1, 0), (1, L), (0, L)), face=mid)
        self.quad(c[0], c[1], c[2], c[3], face=-ax)
        self.quad(e[0], e[1], e[2], e[3], face=ax)

    def polygon(self, poly, y, up=True, uv_scale=1.0):
        """Polígono horizontal (shapely) na altura y, triangulado (com furos)."""
        for pg in polys(poly):
            if pg.is_empty or pg.area < 1e-3:
                continue
            rings = [np.array(pg.exterior.coords)[:-1]] + [np.array(r.coords)[:-1] for r in pg.interiors]
            verts = np.concatenate(rings)
            ends = np.cumsum([len(r) for r in rings]).astype(np.uint32)
            tri = earcut.triangulate_float64(verts, ends).reshape(-1, 3)
            base = self.nv
            for x, z in verts:
                self.vert((x, y, z), (0.0, 1.0 if up else -1.0, 0.0), (x * uv_scale, z * uv_scale))
            for t in tri:
                # earcut no plano xz: orientação para a normal +y (x, z -> anti-horário visto de cima = z para baixo)
                a, b, c = (base + int(i) for i in t)
                pa, pb, pc = (self.p[3 * k:3 * k + 3] for k in (a, b, c))
                ny = (pb[2] - pa[2]) * (pc[0] - pa[0]) - (pb[0] - pa[0]) * (pc[2] - pa[2])
                if (ny > 0) != up:
                    b, c = c, b
                self.idx.extend((a, b, c))

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
        """Cilindro vertical liso de raio r em c=(x, z), de y0 a y1 (com tampa em cima)."""
        self.frustum(c, r, r, y0, y1, seg, cap)

    def sphere(self, c, r, seg=10, sy=1.0):
        cx, cy, cz = c
        for j in range(seg // 2):
            t0, t1 = math.pi * j / (seg // 2) - math.pi / 2, math.pi * (j + 1) / (seg // 2) - math.pi / 2
            for i in range(seg):
                a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
                P = lambda t, a: (cx + r * math.cos(t) * math.cos(a), cy + r * sy * math.sin(t), cz + r * math.cos(t) * math.sin(a))
                tm, am = (t0 + t1) / 2, (a0 + a1) / 2
                Nn = lambda t, a: (math.cos(t) * math.cos(a), math.sin(t), math.cos(t) * math.sin(a))
                self.quad(P(t0, a0), P(t0, a1), P(t1, a1), P(t1, a0),
                          face=(math.cos(tm) * math.cos(am), math.sin(tm), math.cos(tm) * math.sin(am)),
                          nrms=(Nn(t0, a0), Nn(t0, a1), Nn(t1, a1), Nn(t1, a0)))

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


def ogee(y0, d0, y1, d1, n=6):
    """Curva em S (cimácio) de (y0, d0) a (y1, d1): perfil de moldura clássica."""
    return [(y0 + (y1 - y0) * k / n, d0 + (d1 - d0) * (0.5 - 0.5 * math.cos(math.pi * k / n))) for k in range(1, n + 1)]


# Perfis (altura, saliência) de baixo para cima, começando e terminando na parede (d = 0). Escala 1 = ~1,2 m.
CORNIJA_GRANDE = ([(0, 0), (0, 0.06), (0.1, 0.06), (0.1, 0.12), (0.55, 0.12), (0.55, 0.2)] + ogee(0.55, 0.2, 0.66, 0.3, 3)
                  + [(0.66, 0.85), (0.86, 0.85)] + ogee(0.86, 0.85, 1.12, 1.05) + [(1.18, 1.05), (1.18, 0)])
CORNIJA_SIMPLES = [(0, 0), (0, 0.05)] + ogee(0, 0.05, 0.16, 0.22, 4) + [(0.22, 0.22), (0.26, 0.26), (0.32, 0.26),
                                                                           (0.32, 0)]
FRISO = [(0, 0), (0, 0.04), (0.05, 0.08), (0.17, 0.08), (0.22, 0.04), (0.22, 0)]
BASE_PEDRA = [(0, 0), (0, 0.14), (0.75, 0.14)] + ogee(0.75, 0.14, 0.9, 0.04, 3) + [(0.9, 0)]


class Facades:
    """Kit de fachada no estilo de Manhattan (gramática de divisão: lote → térreo / faixa / andares / coroamento →
    vãos → módulo de janela), todo em geometria: cornijas com perfil curvo e mísulas, janelas recuadas com verga,
    peitoril, arco ou frontão, pilastras, frisos, escadas de incêndio de ferro, aparelhos de ar-condicionado, toldos,
    letreiros, portas de enrolar, andaimes de calçada (sidewalk sheds), caixas d'água de madeira, casas de máquinas e
    condensadores no telhado. Cada prédio sorteia o estilo; cada janela sorteia detalhes. Malhas agrupadas por
    material (self.mesh[nome]); cor por vértice com o tom do prédio e a sujeira (base e topo mais escuros)."""

    PLAIN = ('sala', 'vitrine', 'caixilho', 'ferro', 'faixa_luz', 'toldo', 'luz_', 'placa', 'ar_cond', 'montante',
             'cortina', 'coroa', 'tapume', 'andaime', 'aco_teto', 'condensador', 'caixa_agua')

    def __init__(self, rng):
        self.rng = rng
        self.mesh = {}
        self.tint = (1.0, 1.0, 1.0)
        self.grime = None
        self.obstacles = []
        self.lights = []
        self.lod = 0  # 0 = detalhe completo (perto da praça); 1 = janelas simples
        self.is_street = None  # (ponto médio, normal) -> a fachada dá para a rua? (senão é empena de divisa)
        self.n_murais = 0

    def m(self, name):
        M = self.mesh.setdefault(name, Mesh())
        if name.startswith(self.PLAIN):
            M.tint, M.grime = (1.0, 1.0, 1.0), None
        else:
            M.tint, M.grime = self.tint, self.grime
        return M

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
        s = 0.5
        M.quad(P(a, t, n, u0, y0, 0), P(a, t, n, u0, y0, -depth), P(a, t, n, u1, y0, -depth), P(a, t, n, u1, y0, 0),
               uv=((u0 * s, 0), (u0 * s, depth * s), (u1 * s, depth * s), (u1 * s, 0)), face=(0, 1, 0))
        M.quad(P(a, t, n, u1, y1, 0), P(a, t, n, u1, y1, -depth), P(a, t, n, u0, y1, -depth), P(a, t, n, u0, y1, 0),
               uv=((u1 * s, 0), (u1 * s, depth * s), (u0 * s, depth * s), (u0 * s, 0)), face=(0, -1, 0))
        M.quad(P(a, t, n, u0, y1, 0), P(a, t, n, u0, y1, -depth), P(a, t, n, u0, y0, -depth), P(a, t, n, u0, y0, 0),
               uv=((0, y1 * s), (depth * s, y1 * s), (depth * s, y0 * s), (0, y0 * s)), face=T)
        M.quad(P(a, t, n, u1, y0, 0), P(a, t, n, u1, y0, -depth), P(a, t, n, u1, y1, -depth), P(a, t, n, u1, y1, 0),
               uv=((0, y0 * s), (depth * s, y0 * s), (depth * s, y1 * s), (0, y1 * s)), face=(-T[0], 0, -T[2]))

    def block(self, name, a, t, n, u0, u1, y0, y1, d0, d1, uvs=0.5, back=False):
        """Caixa encostada na fachada (peitoril, verga, pilastra, mísula, ar-condicionado): u0..u1, y0..y1, d0..d1."""
        P, M = self.P, self.m(name)
        N, T = (n[0], 0, n[1]), (t[0], 0, t[1])
        q = lambda *c: c
        M.quad(P(a, t, n, u0, y0, d1), P(a, t, n, u1, y0, d1), P(a, t, n, u1, y1, d1), P(a, t, n, u0, y1, d1),
               uv=((u0 * uvs, -y0 * uvs), (u1 * uvs, -y0 * uvs), (u1 * uvs, -y1 * uvs), (u0 * uvs, -y1 * uvs)), face=N)
        M.quad(P(a, t, n, u0, y1, d1), P(a, t, n, u1, y1, d1), P(a, t, n, u1, y1, d0), P(a, t, n, u0, y1, d0),
               uv=((u0 * uvs, d1 * uvs), (u1 * uvs, d1 * uvs), (u1 * uvs, d0 * uvs), (u0 * uvs, d0 * uvs)), face=(0, 1, 0))
        M.quad(P(a, t, n, u0, y0, d0), P(a, t, n, u1, y0, d0), P(a, t, n, u1, y0, d1), P(a, t, n, u0, y0, d1),
               uv=((u0 * uvs, d0 * uvs), (u1 * uvs, d0 * uvs), (u1 * uvs, d1 * uvs), (u0 * uvs, d1 * uvs)), face=(0, -1, 0))
        M.quad(P(a, t, n, u1, y0, d1), P(a, t, n, u1, y0, d0), P(a, t, n, u1, y1, d0), P(a, t, n, u1, y1, d1),
               uv=((d1 * uvs, -y0 * uvs), (d0 * uvs, -y0 * uvs), (d0 * uvs, -y1 * uvs), (d1 * uvs, -y1 * uvs)), face=T)
        M.quad(P(a, t, n, u0, y0, d0), P(a, t, n, u0, y0, d1), P(a, t, n, u0, y1, d1), P(a, t, n, u0, y1, d0),
               uv=((d0 * uvs, -y0 * uvs), (d1 * uvs, -y0 * uvs), (d1 * uvs, -y1 * uvs), (d0 * uvs, -y1 * uvs)),
               face=(-T[0], 0, -T[2]))
        if back:
            M.quad(P(a, t, n, u0, y0, d0), P(a, t, n, u1, y0, d0), P(a, t, n, u1, y1, d0), P(a, t, n, u0, y1, d0),
                   face=(-N[0], 0, -N[2]))

    # Compatibilidade com o código antigo (montantes, cornijas planas).
    def bar(self, name, a, t, n, u, y0, y1, w, d0, d1):
        self.block(name, a, t, n, u - w / 2, u + w / 2, y0, y1, d0, d1)

    def hbar(self, name, a, t, n, u0, u1, y, h, d0, d1):
        self.block(name, a, t, n, u0, u1, y - h / 2, y + h / 2, d0, d1)

    def profile(self, name, a, t, n, u0, u1, y, pts, s=1.0, caps=True):
        """Moldura corrida: extruda o perfil (altura, saliência) ao longo da fachada de u0 a u1, com tampas nas
        pontas. As normais seguem a curva (média dos dois segmentos vizinhos): a luz corre pela moldura."""
        P, M = self.P, self.m(name)
        pts = [(y + dy * s, d * s) for dy, d in pts]
        segs = []
        for (ya, da), (yb, db) in zip(pts[:-1], pts[1:]):
            L = math.hypot(yb - ya, db - da)
            segs.append(None if L < 1e-6 else ((yb - ya) / L, -(db - da) / L))  # normal 2D (d, y)
        arc = 0.0
        for i, ((ya, da), (yb, db)) in enumerate(zip(pts[:-1], pts[1:])):
            if segs[i] is None:
                continue
            nd, ny = segs[i]

            def vn(j):  # normal suave no vértice j do perfil, se o ângulo com o vizinho for pequeno
                o = segs[j] if 0 <= j < len(segs) else None
                if o is None or nd * o[0] + ny * o[1] < 0.8:
                    return np.array([n[0] * nd, ny, n[1] * nd])
                m2 = np.array([nd + o[0], ny + o[1]])
                m2 /= np.linalg.norm(m2)
                return np.array([n[0] * m2[0], m2[1], n[1] * m2[0]])
            na, nb = vn(i - 1), vn(i + 1)
            L = math.hypot(yb - ya, db - da)
            M.quad(P(a, t, n, u0, ya, da), P(a, t, n, u1, ya, da), P(a, t, n, u1, yb, db), P(a, t, n, u0, yb, db),
                   uv=((u0 * 0.5, arc * 0.5), (u1 * 0.5, arc * 0.5), (u1 * 0.5, (arc + L) * 0.5), (u0 * 0.5, (arc + L) * 0.5)),
                   face=(n[0] * nd, ny, n[1] * nd), nrms=(na, na, nb, nb))
            arc += L
        if caps:
            ring = [(d, yy) for yy, d in pts]
            if ring[0] == ring[-1]:
                ring = ring[:-1]
            tri = earcut.triangulate_float64(np.array(ring, np.float64), np.array([len(ring)], np.uint32)).reshape(-1, 3)
            for u, f in ((u0, -1), (u1, 1)):
                for i0, i1, i2 in tri:
                    q = [P(a, t, n, u, ring[k][1], ring[k][0]) for k in (i0, i1, i2)]
                    M.tri(*q, face=(t[0] * f, 0, t[1] * f),
                          uv=tuple((ring[k][0] * 0.5, ring[k][1] * 0.5) for k in (i0, i1, i2)))

    # ------------------------------------------------------------------------------------------- elementos
    def window(self, st, a, t, n, c, ww, wy0, wy1, depth, wall, trim):
        """Módulo de janela: vidro com sala atrás, vão, caixilho de guilhotina, peitoril, e verga/arco/frontão."""
        rng = self.rng
        u0, u1 = c - ww / 2, c + ww / 2
        lit = rng.random() < st['acesas']
        self.wall_quad(f'sala{rng.randrange(8)}' if lit else f'sala_apagada{rng.randrange(3)}', a, t, n,
                       u0, u1, wy0, wy1, -depth, unit=True)
        self.recess(a, t, n, u0, u1, wy0, wy1, depth, wall)
        # peitoril de pedra saliente, um pouco mais largo que o vão
        self.block(trim, a, t, n, u0 - 0.08, u1 + 0.08, wy0 - 0.1, wy0, -0.02, 0.07)
        if self.lod > 0 or st.get('leve'):  # longe da praça: só vidro, vão e peitoril
            return
        fr = st['caixilho']
        d = -depth + 0.06  # caixilho: moldura (frente) 6 cm à frente do vidro, trilho da guilhotina no meio
        ym = (wy0 + wy1) / 2
        for (ua, ub, ya, yb) in ((u0, u1, wy0, wy0 + 0.06), (u0, u1, wy1 - 0.06, wy1), (u0, u0 + 0.06, wy0 + 0.06, wy1 - 0.06),
                                 (u1 - 0.06, u1, wy0 + 0.06, wy1 - 0.06), (u0 + 0.06, u1 - 0.06, ym - 0.03, ym + 0.03)):
            self.wall_quad(fr, a, t, n, ua, ub, ya, yb, d)
        if st['folhas'] > 1:
            self.wall_quad(fr, a, t, n, c - 0.025, c + 0.025, wy0 + 0.06, wy1 - 0.06, d)
        head = st['verga']
        if head == 'verga':
            self.block(trim, a, t, n, u0 - 0.12, u1 + 0.12, wy1, wy1 + 0.28, 0.0, 0.05)
            self.block(trim, a, t, n, c - 0.11, c + 0.11, wy1 - 0.04, wy1 + 0.34, 0.0, 0.09)  # pedra-chave
        elif head == 'arco':  # arco de moldura (capelo italianizante)
            rise = ww * 0.22
            R = (ww * ww / 4 + rise * rise) / (2 * rise)
            yc = wy1 + 0.12 - (R - rise)
            th = math.asin(min(1.0, (ww / 2 + 0.15) / R))
            M = self.m(trim)
            pts = [self.P(a, t, n, c + R * math.sin(f), yc + R * math.cos(f), 0.05) for f in np.linspace(-th, th, 6)]
            for p0, p1 in zip(pts[:-1], pts[1:]):
                M.beam(p0, p1, 0.2, 0.1, up=(n[0], 0, n[1]))
            self.block(trim, a, t, n, u0 - 0.2, u0 - 0.02, wy1 - 0.1, wy1 + 0.05, 0.0, 0.1)  # mísulas do capelo
            self.block(trim, a, t, n, u1 + 0.02, u1 + 0.2, wy1 - 0.1, wy1 + 0.05, 0.0, 0.1)
            self.block(trim, a, t, n, c - 0.09, c + 0.09, wy1 + rise - 0.05, wy1 + rise + 0.3, 0.0, 0.12)
        elif head == 'frontao':  # frontão triangular sobre cornija pequena
            self.profile(trim, a, t, n, u0 - 0.15, u1 + 0.15, wy1 + 0.02, CORNIJA_SIMPLES, s=0.6)
            M = self.m(trim)
            h = ww * 0.28
            y = wy1 + 0.22
            pl = [self.P(a, t, n, u0 - 0.15, y, 0.04), self.P(a, t, n, c, y + h, 0.04), self.P(a, t, n, u1 + 0.15, y, 0.04)]
            M.beam(pl[0], pl[1], 0.14, 0.12, up=(n[0], 0, n[1]))
            M.beam(pl[1], pl[2], 0.14, 0.12, up=(n[0], 0, n[1]))
        # ar-condicionado de janela (o som de NY no verão): caixa bege saliente no vão de baixo
        if rng.random() < st['ar']:
            self.block('ar_cond', a, t, n, c - 0.33, c + 0.33, wy0 + 0.02, wy0 + 0.44, -depth + 0.05, 0.38, back=True)

    def fire_escape(self, a, t, n, uc, width, ys, fh, nfl):
        """Escada de incêndio de ferro preto: plataformas em cada andar, guarda-corpo, lances inclinados alternando o
        lado e a escada de correr pendurada no primeiro patamar."""
        P, M = self.P, self.m('ferro')
        D = 1.15
        u0, u1 = uc - width / 2, uc + width / 2
        N = (n[0], 0, n[1])
        for k in range(nfl):
            y = ys + k * fh
            self.block('ferro', a, t, n, u0, u1, y - 0.05, y, 0.02, D, uvs=1.0)  # piso de grade
            for yy in (y + 0.55, y + 1.05):  # guarda-corpo
                M.beam(P(a, t, n, u0, yy, D), P(a, t, n, u1, yy, D), 0.035, 0.035)
                M.beam(P(a, t, n, u0, yy, 0.02), P(a, t, n, u0, yy, D), 0.035, 0.035)
                M.beam(P(a, t, n, u1, yy, 0.02), P(a, t, n, u1, yy, D), 0.035, 0.035)
            for uu in np.linspace(u0, u1, max(3, int(width / 0.45))):
                M.beam(P(a, t, n, uu, y, D), P(a, t, n, uu, y + 1.05, D), 0.022, 0.022)
            for dd in (0.45, D):
                M.beam(P(a, t, n, u0, y, dd), P(a, t, n, u0, y + 1.05, dd), 0.022, 0.022)
                M.beam(P(a, t, n, u1, y, dd), P(a, t, n, u1, y + 1.05, dd), 0.022, 0.022)
            # mísulas (suportes em diagonal até a parede)
            for uu in (u0 + 0.1, u1 - 0.1):
                M.beam(P(a, t, n, uu, y - 0.05, D - 0.1), P(a, t, n, uu, y - 0.8, 0.02), 0.04, 0.04)
            if k < nfl - 1:  # lance para o andar de cima
                run = min(fh * 0.95, width - 0.6)
                sgn = 1 if k % 2 == 0 else -1
                ua = (u1 - 0.25) if sgn > 0 else (u0 + 0.25)
                ub = ua - sgn * run
                for dd in (0.4, 0.95):
                    M.beam(P(a, t, n, ua, y, dd), P(a, t, n, ub, y + fh, dd), 0.03, 0.12, up=N)
                    M.beam(P(a, t, n, ua, y + 0.9, dd), P(a, t, n, ub, y + fh + 0.9, dd), 0.025, 0.025)
                steps = int(fh / 0.24)
                for j in range(1, steps):
                    f = j / steps
                    uu = ua + (ub - ua) * f
                    M.beam(P(a, t, n, uu, y + fh * f, 0.4), P(a, t, n, uu, y + fh * f, 0.95), 0.2, 0.025)
        # escada de correr do primeiro patamar (recolhida, pendurada)
        uu = u0 + 0.5
        y = ys
        for dd in (0.6, 1.0):
            M.beam(P(a, t, n, uu, y - 0.05, dd), P(a, t, n, uu, y - 2.6, dd), 0.04, 0.04)
        for yy in np.arange(y - 0.35, y - 2.6, -0.3):
            M.beam(P(a, t, n, uu, yy, 0.6), P(a, t, n, uu, yy, 1.0), 0.025, 0.025)

    def awning(self, a, t, n, u0, u1, y, k, depth=1.3):
        """Toldo de lona inclinado com sanefa e laterais."""
        P, M = self.P, self.m(f'toldo{k}')
        top, low = y, y - 0.65
        M.quad(P(a, t, n, u0, top, 0.02), P(a, t, n, u1, top, 0.02), P(a, t, n, u1, low, depth), P(a, t, n, u0, low, depth),
               uv=((u0 * 0.5, 0), (u1 * 0.5, 0), (u1 * 0.5, 0.75), (u0 * 0.5, 0.75)), face=(n[0], 1, n[1]))
        M.quad(P(a, t, n, u0, low, depth), P(a, t, n, u1, low, depth), P(a, t, n, u1, low - 0.28, depth),
               P(a, t, n, u0, low - 0.28, depth), uv=((u0 * 0.5, 0.75), (u1 * 0.5, 0.75), (u1 * 0.5, 0.9), (u0 * 0.5, 0.9)),
               face=(n[0], 0, n[1]))
        for u, f in ((u0, -1), (u1, 1)):
            M.tri(P(a, t, n, u, top, 0.02), P(a, t, n, u, low, depth), P(a, t, n, u, low, 0.02), face=(t[0] * f, 0, t[1] * f))
            M.quad(P(a, t, n, u, low, 0.02), P(a, t, n, u, low, depth), P(a, t, n, u, low - 0.28, depth),
                   P(a, t, n, u, low - 0.28, depth - 0.01), face=(t[0] * f, 0, t[1] * f))

    def shed(self, a, t, n, L, y0):
        """Andaime de calçada (sidewalk shed): deque de madeira a 3,3 m, pilares de aço, tapume verde em cima e luzes
        embaixo. Os pilares viram obstáculo para a multidão."""
        D, H = 3.1, 3.3
        P = self.P
        self.block('tapume', a, t, n, 0, L, y0 + H, y0 + H + 0.22, 0.0, D, back=False)
        self.block('tapume', a, t, n, 0, L, y0 + H + 0.22, y0 + H + 1.15, D - 0.04, D, back=True)
        M = self.m('andaime')
        for u in np.arange(0.3, L, 2.4):
            self.block('andaime', a, t, n, u - 0.06, u + 0.06, y0, y0 + H, D - 0.16, D - 0.04)
            self.block('andaime', a, t, n, u - 0.06, u + 0.06, y0 + 2.6, y0 + H, 0.0, D - 0.04)  # travessa
            x, _, z = P(a, t, n, u, 0, D - 0.1)
            self.obstacles.append((x, z, 0.45))
            if u + 2.4 < L:
                M.beam(P(a, t, n, u, y0 + 0.4, D - 0.1), P(a, t, n, u + 2.4, y0 + H - 0.2, D - 0.1), 0.05, 0.05)
        for u in np.arange(1.5, L, 3.0):
            self.block('luz_andaime', a, t, n, u - 0.3, u + 0.3, y0 + H - 0.05, y0 + H, D / 2 - 0.08, D / 2 + 0.08)
            x, _, z = P(a, t, n, u, 0, D / 2)
            self.lights.append(((x, y0 + H - 0.3, z), (1.0, 0.92, 0.8), 120.0, 8.0))

    def storefront(self, st, a, t, n, L, y0, ground, wall, trim, nb, bw):
        """Térreo: vitrines (ou portas de enrolar) entre pilastras, faixa de letreiro, toldo ou marquise."""
        rng = self.rng
        lit_store = st['loja_luz']
        for i in range(nb):
            u0, u1 = i * bw + 0.35, (i + 1) * bw - 0.35
            yt = y0 + 3.6
            kind = rng.randrange(6) if rng.random() < lit_store else 5
            self.wall_quad(f'vitrine{kind}', a, t, n, u0, u1, y0 + 0.45, yt, -0.3, unit=True)
            self.recess(a, t, n, u0, u1, y0 + 0.45, yt, 0.3, 'caixilho')
            self.block('caixilho', a, t, n, u0, u1, yt - 0.08, yt, -0.3, -0.22)
            if kind != 5 and rng.random() < 0.6:  # montante da vitrine
                self.block('caixilho', a, t, n, (u0 + u1) / 2 - 0.03, (u0 + u1) / 2 + 0.03, y0 + 0.45, yt, -0.3, -0.24)
            self.block(trim, a, t, n, i * bw, u0, y0, y0 + ground, 0.0, 0.12)        # pilastra
            self.block(trim, a, t, n, u1, (i + 1) * bw, y0, y0 + ground, 0.0, 0.12)
            self.block(trim, a, t, n, u0, u1, y0, y0 + 0.45, 0.0, 0.06)              # embasamento
            self.wall_quad(wall, a, t, n, u0, u1, yt, y0 + ground)
        # letreiros: caixa de luz por loja (2 a 3 vãos)
        i = 0
        while i < nb:
            span = min(nb - i, rng.choice([1, 2, 2, 3]))
            u0, u1 = i * bw + 0.5, (i + span) * bw - 0.5
            if rng.random() < 0.8:
                self.block('marquise', a, t, n, u0, u1, y0 + 3.75, y0 + 4.6, 0.12, 0.3)
                self.wall_quad(f'placa{rng.randrange(16)}', a, t, n, u0 + 0.05, u1 - 0.05, y0 + 3.8, y0 + 4.55, 0.305,
                               unit=True)
            if st['toldo'] and rng.random() < 0.75:
                self.awning(a, t, n, i * bw + 0.3, (i + span) * bw - 0.3, y0 + 3.7, st['toldo_cor'])
            i += span
        if not st['toldo']:
            self.block('marquise', a, t, n, 0, L, y0 + 4.75, y0 + 4.95, 0.0, 1.2)
            for u in np.arange(1.0, L - 0.5, 2.5):  # spots embaixo da marquise (não uma faixa contínua)
                self.block('faixa_luz', a, t, n, u - 0.12, u + 0.12, y0 + 4.71, y0 + 4.75, 0.55, 0.8)
        self.profile(trim, a, t, n, -0.15, L + 0.15, y0 + ground - 0.32, CORNIJA_SIMPLES, s=1.0)

    def blank_wall(self, a, t, n, L, y0, y1, wall, trim):
        """Empena cega na divisa do lote (tijolo liso). Acima dos vizinhos baixos aparece: às vezes com anúncio
        antigo pintado na parede (ghost sign) ou mural, como em Manhattan."""
        rng = self.rng
        self.wall_quad(wall, a, t, n, 0, L, y0, y1)
        self.block(wall, a, t, n, -0.05, L + 0.05, y1, y1 + 0.9, -0.25, 0.0)  # platibanda
        self.block(trim, a, t, n, -0.08, L + 0.08, y1 + 0.9, y1 + 1.0, -0.3, 0.04)
        if y1 - y0 > 22 and L > 8 and rng.random() < 0.45:
            h = min(y1 - y0 - 16, rng.uniform(8, 16), L * 0.9)
            w = min(L * rng.uniform(0.6, 0.9), h * rng.uniform(0.8, 1.6))
            u0 = (L - w) / 2
            yt = y1 - rng.uniform(1.5, 4)
            self.wall_quad(f'mural{rng.randrange(12)}', a, t, n, u0, u0 + w, yt - h, yt, 0.02, unit=True)
            self.n_murais += 1

    def blade_sign(self, a, t, n, u, y, k):
        """Letreiro de lâmina perpendicular à fachada (luminoso dos dois lados) num suporte de ferro."""
        P, M = self.P, self.m('ferro')
        D = 0.95
        H = 3.2  # a placa é 4:1 (texto deitado): o texto corre de baixo para cima, como os letreiros de hotel
        M.beam(P(a, t, n, u, y + H + 0.08, 0.0), P(a, t, n, u, y + H + 0.08, D + 0.25), 0.05, 0.05)
        M.beam(P(a, t, n, u, y + H * 0.4, 0.0), P(a, t, n, u, y + H + 0.08, D * 0.7), 0.04, 0.04)
        S = self.m(f'placa{k}')
        T = (t[0], 0, t[1])
        for sgn in (1, -1):  # as duas faces da lâmina
            uu = u + sgn * 0.06
            S.quad(P(a, t, n, uu, y, 0.25), P(a, t, n, uu, y, D + 0.2), P(a, t, n, uu, y + H, D + 0.2),
                   P(a, t, n, uu, y + H, 0.25), uv=((0, 0), (0, 1), (1, 1), (1, 0)) if sgn > 0 else
                   ((0, 1), (0, 0), (1, 0), (1, 1)), face=(T[0] * sgn, 0, T[2] * sgn))
        self.block('marquise', a, t, n, u - 0.06, u + 0.06, y - 0.05, y + H + 0.05, 0.22, D + 0.23)

    def drainpipe(self, a, t, n, u, y0, y1):
        """Calha/condutor de água descendo pela fachada, com as braçadeiras."""
        P, M = self.P, self.m('ferro')
        M.tube([P(a, t, n, u, y1 + 0.3, 0.3), P(a, t, n, u, y1, 0.12), P(a, t, n, u, y0 + 0.4, 0.12),
                P(a, t, n, u, y0, 0.25)], 0.055, seg=6)
        for yy in np.arange(y0 + 2, y1, 3.0):
            M.beam(P(a, t, n, u, yy, 0.0), P(a, t, n, u, yy, 0.12), 0.03, 0.03)

    def masonry(self, poly, y0, y1, wall, storefront=True, st=None, street_edge=None):
        """Prédio de alvenaria (pré-guerra): térreo de lojas, andares de janelas com moldura, frisos, pilastras,
        escadas de incêndio nas fachadas de tijolo das ruas, cornija grande com mísulas e platibanda."""
        rng = self.rng
        st = st or {}
        st.setdefault('acesas', 0.45)
        st.setdefault('verga', rng.choice(['verga', 'verga', 'arco', 'frontao', 'simples']))
        st.setdefault('folhas', rng.choice([1, 1, 2]))
        st.setdefault('caixilho', 'caixilho')
        st.setdefault('ar', 0.12 if 'tijolo' in wall else 0.05)
        st.setdefault('loja_luz', 0.85)
        st.setdefault('toldo', rng.random() < 0.5)
        st.setdefault('toldo_cor', rng.randrange(6))
        trim = st.get('trim', 'cornija')
        ground = 5.2 if storefront else 0.0
        fh = st.get('fh', rng.uniform(3.2, 3.7))
        bay = rng.uniform(2.1, 2.8)
        ww = min(bay * rng.uniform(0.42, 0.55), 1.3)
        sill, head = rng.uniform(0.75, 0.95), rng.uniform(0.55, 0.8)
        depth = rng.uniform(0.2, 0.3)
        pil = st.get('pilastras', rng.random() < 0.3)
        escape = st.get('incendio', 'tijolo' in wall and rng.random() < 0.6)
        self.grime = (y0, 3.0, 0.4, y1)
        for a, b, L, t, n in self.edges(poly):
            nb = max(1, int(L // bay))
            bw = L / nb
            yb = y0
            if self.is_street is not None and not self.is_street((a + b) / 2, n):
                self.blank_wall(a, t, n, L, y0, y1, wall, trim)  # divisa com o vizinho: empena cega
                continue
            if storefront and y1 - y0 > 8:
                self.storefront(st, a, t, n, L, y0, ground, wall, trim, nb, bw)
                yb = y0 + ground
                if self.lod == 0 and rng.random() < 0.55:
                    self.blade_sign(a, t, n, rng.uniform(1.0, max(1.1, L - 1.0)), y0 + ground + 0.6, rng.randrange(16))
                if rng.random() < 0.4:
                    self.drainpipe(a, t, n, rng.choice([0.25, L - 0.25]), y0, y1)
                if st.get('andaime') and abs(n[1]) > 0.7:
                    self.shed(a, t, n, L, y0)
            elif y1 - y0 > 8:
                self.profile(trim, a, t, n, -0.1, L + 0.1, y0, BASE_PEDRA, s=1.0, caps=True)
            y = yb
            floors = []
            while y + fh <= y1 - 1.6:
                floors.append(y)
                y += fh
            for k, fy in enumerate(floors):
                for i in range(nb):
                    c = (i + 0.5) * bw
                    u0, u1 = c - ww / 2, c + ww / 2
                    wy0, wy1 = fy + sill, fy + fh - head
                    self.window(st, a, t, n, c, ww, wy0, wy1, depth, wall, trim)
                    self.wall_quad(wall, a, t, n, i * bw, u0, fy, fy + fh)
                    self.wall_quad(wall, a, t, n, u1, (i + 1) * bw, fy, fy + fh)
                    self.wall_quad(wall, a, t, n, u0, u1, fy, wy0)
                    self.wall_quad(wall, a, t, n, u0, u1, wy1, fy + fh)
                if k == 0 or (st.get('friso', 0.25) > rng.random() and k < len(floors) - 1):
                    self.profile(trim, a, t, n, -0.05, L + 0.05, fy - 0.11, FRISO, s=1.0)
            if pil and floors:
                for i in range(nb + 1):
                    u = i * bw
                    self.block(wall, a, t, n, max(0, u - 0.28), min(L, u + 0.28), floors[0], y, 0.0, 0.1)
            if escape and abs(n[1]) > 0.7 and len(floors) >= 3 and nb >= 3 and L < 40:
                i0 = rng.randrange(0, nb - 1)
                self.fire_escape(a, t, n, (i0 + 1) * bw, 2 * bw - 0.2, floors[1] if storefront else floors[0] + fh,
                                 fh, len(floors) - 1)
            self.wall_quad(wall, a, t, n, 0, L, y, y1)  # faixa até a cornija
            big = st.get('cornija_grande', y1 - y0 < 70)
            if big:
                yc = y1 - 0.25
                self.profile(trim, a, t, n, -1.1, L + 1.1, yc, CORNIJA_GRANDE, s=1.0)
                if self.lod == 0:
                    for u in np.arange(0.4, L - 0.2, 0.9):  # mísulas sob a coroa
                        self.block(trim, a, t, n, u - 0.08, u + 0.08, yc + 0.3, yc + 0.66, 0.2, 0.78)
                ytop = yc + 1.18
            else:
                self.profile(trim, a, t, n, -0.3, L + 0.3, y1 - 0.3, CORNIJA_SIMPLES, s=1.2)
                ytop = y1 + 0.1
            self.block(wall, a, t, n, -0.05, L + 0.05, ytop, ytop + 0.9, -0.25, 0.0)  # platibanda
            self.block(trim, a, t, n, -0.08, L + 0.08, ytop + 0.9, ytop + 1.0, -0.3, 0.04)  # capeamento
        self.grime = None
        return y1

    def roof(self, poly, y, small=True):
        """Telhado de Nova York: caixa d'água de cedro em pernas de aço, casa de máquinas, condensadores, respiros."""
        rng = self.rng
        inner = polys(poly.buffer(-2.5, join_style=2))
        if not inner:
            return
        inner = max(inner, key=lambda q: q.area)
        minx, minz, maxx, maxz = inner.bounds

        def spot(r):
            for _ in range(20):
                x, z = rng.uniform(minx + r, maxx - r), rng.uniform(minz + r, maxz - r)
                if inner.contains(Point(x, z).buffer(r)):
                    return x, z
            return None
        if small and inner.area > 40 and rng.random() < 0.55:
            r = rng.uniform(1.8, 2.6)
            p = spot(r + 0.5)
            if p:
                x, z = p
                legs = rng.uniform(2.8, 4.5)
                S = self.m('aco_teto')
                yb = y + legs
                k = 6
                for i in range(k):
                    ang = 2 * math.pi * i / k
                    lx, lz = x + r * 0.8 * math.cos(ang), z + r * 0.8 * math.sin(ang)
                    S.beam((lx, y, lz), (lx, yb, lz), 0.14, 0.14)
                    ang2 = 2 * math.pi * (i + 1) / k
                    l2 = (x + r * 0.8 * math.cos(ang2), z + r * 0.8 * math.sin(ang2))
                    S.beam((lx, y + 0.3, lz), (l2[0], yb - 0.3, l2[1]), 0.05, 0.05)  # contraventamento
                S.cylinder((x, z), r + 0.15, yb - 0.25, yb, seg=20)  # plataforma
                W = self.m('caixa_agua')
                h = rng.uniform(3.6, 5.0)
                W.frustum((x, z), r, r * 0.97, yb, yb + h, seg=28, cap=False, uvs=0.5)
                W.frustum((x, z), r + 0.12, 0.0, yb + h, yb + h + r * 0.55, seg=28, uvs=0.5)
                for hy in np.linspace(yb + 0.4, yb + h - 0.3, int(h / 0.7)):  # aros de aço
                    S.tube([(x + (r + 0.03) * math.cos(f), hy, z + (r + 0.03) * math.sin(f))
                            for f in np.linspace(0, 2 * math.pi, 29)], 0.025, seg=5)
                S.beam((x + r, yb + h, z), (x + r, y, z), 0.04, 0.04)  # escada de acesso
        if inner.area > 30 and rng.random() < 0.7:  # casa de máquinas do elevador
            p = spot(2.0)
            if p:
                x, z = p
                hx, hz = rng.uniform(1.5, 2.5), rng.uniform(1.5, 3.0)
                self.m('telhado_casa').box((x, y + 1.7, z), (hx, 1.7, hz))
                self.m('aco_teto').box((x, y + 3.45, z), (hx + 0.1, 0.06, hz + 0.1))
        for _ in range(rng.randrange(1, 5)):  # condensadores de ar com o ventilador em cima
            p = spot(1.2)
            if p:
                x, z = p
                C = self.m('condensador')
                C.box((x, y + 0.65, z), (0.9, 0.65, 0.6), rot=rng.choice([0, math.pi / 2]))
                self.m('aco_teto').cylinder((x, z), 0.42, y + 1.3, y + 1.36, seg=14)
        for _ in range(rng.randrange(0, 4)):  # respiros e chaminés
            p = spot(0.3)
            if p:
                self.m('aco_teto').cylinder(p, rng.uniform(0.08, 0.2), y, y + rng.uniform(0.8, 2.2), seg=8)

    def curtain(self, poly, y0, y1, k, crown=True):
        """Torre de pele de vidro: painéis por módulo (textura com andares), montantes salientes, faixas de laje a cada
        4 andares e coroamento iluminado."""
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
                self.block('montante', a, t, n, i * bw - 0.05, i * bw + 0.05, y0, y1, 0.0, 0.2)
            yy = y0 + fh
            while yy < y1 - 2:
                self.block('montante', a, t, n, -0.1, L + 0.1, yy - 0.2, yy, 0.0, 0.12)
                yy += fh
            if crown:
                self.block('montante', a, t, n, -0.2, L + 0.2, y1, y1 + 3.5, -0.2, 0.25)
                self.block('coroa', a, t, n, 0.0, L, y1 + 0.6, y1 + 0.75, 0.25, 0.3)
                self.block('coroa', a, t, n, 0.0, L, y1 + 2.6, y1 + 2.75, 0.25, 0.3)


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
                 strength=1.0, double=False, normal_tex=None, orm_tex=None, normal_scale=1.0, mask=None):
        m = {'name': name, 'pbrMetallicRoughness': {'baseColorFactor': list(color) + [1.0], 'metallicFactor': metal,
                                                     'roughnessFactor': rough}}
        if tex:
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': self.texture(tex)}
        if normal_tex:
            m['normalTexture'] = {'index': self.texture(normal_tex), 'scale': normal_scale}
        if orm_tex:  # Poly Haven "arm": R = oclusão, G = rugosidade, B = metal (o mesmo arranjo do glTF)
            m['pbrMetallicRoughness']['metallicRoughnessTexture'] = {'index': self.texture(orm_tex)}
            m['occlusionTexture'] = {'index': self.texture(orm_tex)}
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
        if mask is not None:  # recorte por alfa (folhas, marcas pintadas, remendos)
            m['alphaMode'] = 'MASK'
            m['alphaCutoff'] = mask
        self.doc['materials'].append(m)
        return len(self.doc['materials']) - 1

    def add_mesh(self, name, mesh, mat):
        if not mesh.idx:
            return
        P = np.frombuffer(mesh.p, np.float32).reshape(-1, 3)
        N = np.frombuffer(mesh.n, np.float32).reshape(-1, 3)
        T = np.frombuffer(mesh.uv, np.float32).reshape(-1, 2)
        I = np.frombuffer(mesh.idx, np.uint32)
        prim = {'attributes': {'POSITION': self.acc(P, 'VEC3', 5126, 34962, True), 'NORMAL': self.acc(N, 'VEC3', 5126, 34962),
                               'TEXCOORD_0': self.acc(T, 'VEC2', 5126, 34962)},
                'indices': self.acc(I, 'SCALAR', 5125, 34963), 'material': mat}
        C = np.frombuffer(mesh.c, np.float32).reshape(-1, 3)
        if len(C) == len(P) and np.abs(C - 1.0).max() > 1e-3:  # tom/sujeira por vértice só quando há
            prim['attributes']['COLOR_0'] = self.acc(np.clip(C, 0, 1), 'VEC3', 5126, 34962)
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


# ---------------------------------------------------------------------------------------------------------- detalhes
# O que faz a rua parecer de verdade não é prédio complicado: é camada sobre camada de informação. Pintura e
# remendo no asfalto, manchas, placas, letreiros, árvores, lixo, coisas penduradas, paredes laterais pintadas.
ADS_VELHOS = ['HARLAN & SONS\nFURNITURE', 'GOLDEN STAR\nPIANOS', 'MERCER TONIC\n5 CENTS', 'HOTEL ROYALE\nROOMS $2',
              'ATLAS STORAGE\nFIREPROOF', 'UNCLE OTTO\nPICKLES', 'BROADWAY\nLAUNDRY', 'NEW ERA\nCIGARS']
ADS_MURAL = ['THE LAST\nENCORE', 'NOW\nPLAYING', 'SKYLINE\nRADIO 98.1', 'KING\nCOFFEE', 'WEST SIDE\nSTORIES',
             'NEON\nNIGHTS', 'MIDTOWN\nMUSIC HALL', 'LUCKY 7\nLOTTERY']


def mural_texture(path, rng, brick_path, k):
    """Anúncio pintado na empena de tijolo: metade 'ghost signs' desbotados (letras brancas em fundo escuro, como os
    do começo do século XX em Nova York), metade murais coloridos; a tinta deixa o tijolo aparecer (desgaste)."""
    W = 512
    brick = Image.open(brick_path).convert('RGB').resize((128, 128))
    base = Image.new('RGB', (W, W))
    for x in range(0, W, 128):
        for y in range(0, W, 128):
            base.paste(brick, (x, y))
    paint = Image.new('RGB', (W, W))
    d = ImageDraw.Draw(paint)
    ghost = k % 2 == 0
    if ghost:
        bg = rng.choice([(90, 30, 25), (30, 30, 34), (40, 55, 70), (95, 70, 30)])
        d.rectangle((0, 0, W, W), fill=bg)
        d.rectangle((14, 14, W - 14, W - 14), outline=(225, 215, 190), width=6)
        txt = rng.choice(ADS_VELHOS)
        col = (230, 222, 200)
        font = FONT2
    else:
        c1 = np.array(rng.choice([(240, 90, 60), (40, 120, 220), (250, 200, 40), (160, 60, 200), (30, 170, 140)]))
        c2 = np.array(rng.choice([(20, 20, 40), (250, 240, 220), (200, 30, 80), (20, 80, 60)]))
        yy = np.linspace(0, 1, W)[:, None, None]
        arr = (c1 * (1 - yy) + c2 * yy) * np.ones((1, W, 1))
        paint = Image.fromarray(arr.astype(np.uint8))
        d = ImageDraw.Draw(paint)
        for _ in range(rng.randrange(2, 5)):  # formas grandes (sol, círculos, faixas)
            r = rng.randrange(60, 200)
            cx, cy = rng.randrange(W), rng.randrange(W)
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=tuple(int(v) for v in rng.choice([c2, c1 * 0.6, (255, 255, 255)])))
        txt = rng.choice(ADS_MURAL)
        col = (255, 255, 255) if c2.sum() < 300 else (20, 20, 30)
        font = FONT
    lines = txt.split('\n')
    f = ImageFont.truetype(font, 92)
    for i, line in enumerate(lines):
        tw = d.textlength(line, font=f)
        fs = int(92 * min(1.0, (W - 60) / max(tw, 1)))
        ff = ImageFont.truetype(font, fs)
        tw = d.textlength(line, font=ff)
        d.text(((W - tw) / 2, W * (0.28 + 0.3 * i) - fs / 2), line, font=ff, fill=col)
    # desgaste: a tinta falha em manchas (ruído de baixa frequência) e nas juntas do tijolo
    n = np.random.default_rng(k).random((32, 32))
    n = np.asarray(Image.fromarray((n * 255).astype(np.uint8)).resize((W, W), Image.BICUBIC)) / 255.0
    a = np.clip((0.55 if ghost else 0.85) + 0.5 * (n - 0.5), 0, 1)[..., None]
    lum = np.asarray(base.convert('L'), np.float32)[..., None] / 255.0
    out = np.asarray(paint, np.float32) * (0.55 + 0.6 * lum)  # a tinta acompanha a textura do tijolo
    res = out * a + np.asarray(base, np.float32) * (1 - a)
    Image.fromarray(np.clip(res, 0, 255).astype(np.uint8)).save(path)
    return path


def decal_texture(path, kind, seed, w=256):
    """Decalques RGBA (alfa recortado): remendo de asfalto, mancha de óleo, chiclete/sujeira de calçada, folhas."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:w, 0:w] / w - 0.5
    low = np.asarray(Image.fromarray((rng.random((16, 16)) * 255).astype(np.uint8)).resize((w, w), Image.BICUBIC)) / 255.0
    hi = rng.random((w, w))
    if kind == 'remendo':  # retângulo torto de piche mais novo
        edge = np.maximum(np.abs(xx) / rng.uniform(0.3, 0.46), np.abs(yy) / rng.uniform(0.3, 0.46))
        m = edge + 0.25 * (low - 0.5) < 1.0
        v = 22 + 10 * hi + 8 * low
        rgb = np.stack([v, v, v * 1.03], -1)
    elif kind == 'oleo':
        r = np.hypot(xx, yy * 1.4)
        m = r + 0.18 * (low - 0.5) < 0.32
        m &= hi > r * 1.6
        v = 8 + 6 * low
        rgb = np.stack([v, v * 0.95, v * 0.9], -1)
    elif kind == 'sujeira':  # chicletes e manchas pretas da calçada de Nova York
        m = (hi > 0.993) | ((low > 0.72) & (hi > 0.6))
        v = 30 + 30 * low
        rgb = np.stack([v, v, v], -1)
    else:  # 'folhas': grupo de folhas para a copa das árvores
        img = Image.new('RGBA', (w, w), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        pr = random.Random(seed)
        for _ in range(420):
            cx, cy = pr.gauss(w / 2, w / 5), pr.gauss(w / 2, w / 5)
            r = pr.uniform(5, 11)
            g = pr.uniform(0.6, 1.0)
            d.ellipse((cx - r, cy - r * 0.6, cx + r, cy + r * 0.6),
                      fill=(int(40 * g), int(85 * g + pr.uniform(0, 25)), int(25 * g), 255))
        img.save(path)
        return path
    a = np.where(m, 255, 0)
    Image.fromarray(np.dstack([np.clip(rgb, 0, 255), a]).astype(np.uint8), 'RGBA').save(path)
    return path


def text_decal(path, text, w=512, h=256, color=(240, 240, 235)):
    """Pintura de solo (ONLY, BUS ONLY, STOP): letras alongadas no sentido da via, com tinta gasta."""
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, int(h * 0.8))
    tw = d.textlength(text, font=f)
    d.text(((w - tw) / 2, h * 0.05), text, font=f, fill=color + (255,))
    img = img.resize((w, h))
    a = np.asarray(img).copy()
    wear = np.random.default_rng(len(text)).random((h, w)) < 0.18
    a[wear, 3] = 0
    Image.fromarray(a, 'RGBA').save(path)
    return path


def sign_texture(path, text, bg=(10, 95, 50), fg=(245, 245, 240), w=512, h=128):
    img = Image.new('RGB', (w, h), bg)
    d = ImageDraw.Draw(img)
    d.rectangle((4, 4, w - 5, h - 5), outline=fg, width=4)
    f = ImageFont.truetype(FONT, int(h * 0.62))
    tw = d.textlength(text, font=f)
    if tw > w - 30:
        f = ImageFont.truetype(FONT, int(h * 0.62 * (w - 30) / tw))
        tw = d.textlength(text, font=f)
    d.text(((w - tw) / 2, h * 0.14), text, font=f, fill=fg)
    img.save(path)
    return path


def flat(mesh, cx, cz, w, h, rot, y, flip_v=False):
    """Decalque deitado no chão, centrado em (cx, cz), w ao longo de rot, h na perpendicular."""
    c, s = math.cos(rot), math.sin(rot)
    P = lambda u, v: (cx + u * c - v * s, y, cz + u * s + v * c)
    uv = ((0, 1), (1, 1), (1, 0), (0, 0)) if not flip_v else ((1, 0), (0, 0), (0, 1), (1, 1))
    mesh.quad(P(-w / 2, -h / 2), P(w / 2, -h / 2), P(w / 2, h / 2), P(-w / 2, h / 2), uv=uv, face=(0, 1, 0))


def tree(trunk, leaves, x, z, y, rng):
    """Árvore de rua (plátano de Nova York): tronco afunilado, galhos curvos e copa de cartões de folhas cruzados."""
    hgt = rng.uniform(4.5, 7.5)
    trunk.frustum((x, z), 0.17, 0.11, y, y + hgt * 0.45, seg=9)
    tops = []
    for i in range(rng.randrange(4, 7)):
        ang = rng.uniform(0, 2 * math.pi)
        out = rng.uniform(1.2, 2.4)
        y0 = y + hgt * rng.uniform(0.38, 0.45)
        pts = [(x + math.cos(ang) * out * f * f, y0 + (hgt - (y0 - y)) * rng.uniform(0.7, 1.0) * f,
                z + math.sin(ang) * out * f * f) for f in np.linspace(0, 1, 5)]
        trunk.tube(pts, 0.07, seg=6)
        tops.append(pts[-1])
        tops.append(pts[2])
    for (px, py, pz) in tops:
        r = rng.uniform(0.9, 1.5)
        for k in range(3):  # três cartões cruzados por cacho
            a = k * math.pi / 3 + rng.uniform(0, 0.5)
            dx, dz = math.cos(a) * r, math.sin(a) * r
            tilt = rng.uniform(-0.3, 0.3)
            leaves.quad((px - dx, py - r + tilt, pz - dz), (px + dx, py - r - tilt, pz + dz),
                        (px + dx, py + r - tilt, pz + dz), (px - dx, py + r + tilt, pz - dz),
                        uv=((0, 1), (1, 1), (1, 0), (0, 0)))
        leaves.quad((px - r, py + 0.1, pz - r), (px + r, py + 0.1, pz - r), (px + r, py + 0.1, pz + r),
                    (px - r, py + 0.1, pz + r), uv=((0, 1), (1, 1), (1, 0), (0, 0)), face=(0, 1, 0))


def street_details(g, pbr, rng, road, sidewalk, plaza, near_plaza, obstacles, tex, rel, texdir):
    """Camada de rua: marcações e remendos no asfalto, manchas, árvores com canteiro, sacos de lixo, placas com o
    nome das ruas, caixas de jornal e de correio. Tudo só perto da praça (até ~260 m), onde a câmera anda."""
    near = lambda x, z, r=260: Point(x, z).distance(near_plaza) < r
    paint = g.material('pintura_solo', color=(0.9, 0.9, 0.86), rough=0.55, tex=rel(text_decal(tex('solo_only.png'), 'ONLY')),
                       mask=0.5)
    bus = g.material('pintura_bus', color=(0.9, 0.9, 0.86), rough=0.55,
                     tex=rel(text_decal(tex('solo_bus.png'), 'BUS ONLY')), mask=0.5)
    redlane = g.material('faixa_onibus', color=(0.42, 0.08, 0.06), rough=0.6)
    line = g.material('linha_branca', color=(0.82, 0.82, 0.78), rough=0.55)
    yellow = g.material('linha_amarela', color=(0.75, 0.55, 0.06), rough=0.55)
    patches = [g.material(f'remendo{k}', rough=0.45, tex=rel(decal_texture(tex(f'remendo{k}.png'), 'remendo', 10 + k)),
                          mask=0.5) for k in range(4)]
    oil = [g.material(f'oleo{k}', rough=0.15, tex=rel(decal_texture(tex(f'oleo{k}.png'), 'oleo', 20 + k)), mask=0.5)
           for k in range(3)]
    dirt = g.material('sujeira', rough=0.6, tex=rel(decal_texture(tex('sujeira.png'), 'sujeira', 30)), mask=0.5)
    M = {k: Mesh() for k in ('only', 'bus', 'red', 'line', 'yellow', 'dirt')}
    MP = [Mesh() for _ in patches]
    MO = [Mesh() for _ in oil]

    # Ruas transversais (mão única, para leste ou oeste): linha tracejada entre as faixas, faixa de retenção antes
    # das avenidas, "ONLY" com seta nas aproximações.
    for n in range(40, 51):
        z, w = street_z(n), street_width(n)
        for x in np.arange(-290, 325, 9.0):
            if any(abs(x - ax) < 17 for ax in AVE_X.values()) or abs(x - BWAY_K * z) < 17 or not near(x, z):
                continue
            flat(M['line'], x, z, 3.0, 0.12, 0.0, 0.012)
        for ax in AVE_X.values():
            for sx in (-1, 1):
                xs = ax + sx * 17.5
                if not near(xs, z):
                    continue
                flat(M['line'], xs, z, 0.45, w - (12 if n == 42 else 8) - 0.3, 0.0, 0.012)
                if rng.random() < 0.6:
                    flat(M['only'], xs + sx * 6, z - 1.3 * rng.choice([-1, 1]), 3.2, 1.6, math.pi / 2 * sx, 0.012)
    # Avenidas (várias faixas, mão única): tracejados, faixa de ônibus vermelha junto à calçada com "BUS ONLY".
    for name, ax in AVE_X.items():
        for z in np.arange(-500, 500, 9.0):
            if any(abs(z - street_z(n)) < 13 for n in range(40, 51)) or not near(ax, z):
                continue
            for off in (-5.4, -1.8, 1.8):
                flat(M['line'], ax + off, z, 0.12, 3.0, 0.0, 0.012)
            flat(M['red'], ax + 7.4, z, 3.2, 9.0, 0.0, 0.011)
            if int(z / 9) % 5 == 0:
                flat(M['bus'], ax + 7.4, z, 4.5, 2.2, -math.pi / 2, 0.013)
        for z in np.arange(-500, 500, 3.0):
            if near(ax, z) and not any(abs(z - street_z(n)) < 13 for n in range(40, 51)):
                flat(M['yellow'], ax - 8.6, z, 0.1, 3.0, 0.0, 0.012)  # borda amarela do lado esquerdo
    # Remendos, manchas de óleo (no meio das faixas, onde pinga dos carros parados) e sujeira das calçadas.
    minx, minz, maxx, maxz = road.bounds
    placed = 0
    while placed < 700:
        x, z = rng.uniform(minx, maxx), rng.uniform(minz, maxz)
        if not near(x, z) or not road.contains(Point(x, z)):
            continue
        k = rng.randrange(4)
        flat(MP[k], x, z, rng.uniform(1.0, 4.5), rng.uniform(0.8, 3.0), rng.uniform(0, math.pi), 0.008 + 0.0005 * k)
        if rng.random() < 0.7:
            flat(MO[rng.randrange(3)], x + rng.uniform(-3, 3), z + rng.uniform(-3, 3), rng.uniform(0.6, 1.6),
                 rng.uniform(0.6, 1.4), rng.uniform(0, math.pi), 0.0105)
        placed += 1
    walk = unary_union([sidewalk, plaza])
    minx, minz, maxx, maxz = walk.bounds
    placed = 0
    while placed < 900:
        x, z = rng.uniform(minx, maxx), rng.uniform(minz, maxz)
        if not near(x, z) or not walk.contains(Point(x, z)):
            continue
        flat(M['dirt'], x, z, rng.uniform(1.5, 4), rng.uniform(1.5, 4), rng.uniform(0, math.pi), 0.157)
        placed += 1
    for key, mat in (('only', paint), ('bus', bus), ('red', redlane), ('line', line), ('yellow', yellow), ('dirt', dirt)):
        g.add_mesh('solo_' + key, M[key], mat)
    for k, (mesh, mat) in enumerate(zip(MP, patches)):
        g.add_mesh(f'solo_remendo{k}', mesh, mat)
    for k, (mesh, mat) in enumerate(zip(MO, oil)):
        g.add_mesh(f'solo_oleo{k}', mesh, mat)

    # Árvores nas calçadas das ruas transversais, com canteiro de ferro baixo; sacos de lixo empilhados no meio-fio
    # (a coleta em Nova York é à noite: as pilhas de sacos pretos fazem parte da paisagem).
    bark = pbr('casca', 'raw_plank_wall', tint=(0.35, 0.3, 0.26), normal=0.6)
    leaf = g.material('folhas', rough=0.6, tex=rel(decal_texture(tex('folhas.png'), 'folhas', 40)), mask=0.4,
                      double=True)
    soil = g.material('terra', color=(0.08, 0.06, 0.04), rough=0.95)
    bagm = g.material('saco_lixo', color=(0.015, 0.015, 0.018), rough=0.22)
    bagw = g.material('saco_lixo_claro', color=(0.55, 0.55, 0.5), rough=0.3)
    trunk, leaves, pits, fences, bags, bags2 = Mesh(), Mesh(), Mesh(), Mesh(), Mesh(), Mesh()
    poles_x = set(round(x) for x in np.arange(-280, 320, 32.0))
    for n in range(40, 51):
        z, w = street_z(n), street_width(n)
        side = 6.0 if n == 42 else 4.0
        for sgn in (-1, 1):  # 1 m para dentro da calçada a partir da guia
            zt = z + sgn * (w / 2 - side + 1.0)
            x = -285.0
            while x < 320:
                x += rng.uniform(8, 12)
                if (any(abs(x - ax) < 22 for ax in AVE_X.values()) or abs(x - BWAY_K * zt) < 22 or not near(x, zt, 230)
                        or any(abs(x - px) < 2.5 for px in poles_x)):
                    continue
                if rng.random() < 0.72:
                    tree(trunk, leaves, x, zt, 0.15, rng)
                    pits.box((x, 0.16, zt), (0.75, 0.01, 0.75))
                    for (ax0, az0, ax1, az1) in ((-0.8, -0.8, 0.8, -0.8), (0.8, -0.8, 0.8, 0.8), (0.8, 0.8, -0.8, 0.8),
                                                 (-0.8, 0.8, -0.8, -0.8)):
                        fences.beam((x + ax0, 0.45, zt + az0), (x + ax1, 0.45, zt + az1), 0.03, 0.03)
                    for (ax0, az0) in ((-0.8, -0.8), (0.8, -0.8), (0.8, 0.8), (-0.8, 0.8)):
                        fences.beam((x + ax0, 0.15, zt + az0), (x + ax0, 0.47, zt + az0), 0.035, 0.035)
                    obstacles.append((x, zt, 1.1))
                elif rng.random() < 0.5:  # pilha de sacos de lixo
                    for _ in range(rng.randrange(3, 9)):
                        bx, bz = x + rng.uniform(-1.2, 1.2), zt + rng.uniform(-0.5, 0.5)
                        r = rng.uniform(0.28, 0.42)
                        (bags if rng.random() < 0.8 else bags2).sphere((bx, 0.15 + r * 0.75 + rng.uniform(0, 0.3), bz), r,
                                                                       10, sy=rng.uniform(0.75, 1.1))
                    obstacles.append((x, zt, 1.4))
    g.add_mesh('rua_troncos', trunk, bark)
    g.add_mesh('rua_folhas', leaves, leaf)
    g.add_mesh('rua_canteiros', pits, soil)
    g.add_mesh('rua_grades', fences, g.material('grade_canteiro', color=(0.03, 0.03, 0.03), rough=0.4, metal=1.0))
    g.add_mesh('rua_sacos', bags, bagm)
    g.add_mesh('rua_sacos_claros', bags2, bagw)

    # Placas com o nome das ruas (verdes, nas esquinas) e caixas de jornal/correio perto das esquinas.
    signs = Mesh()
    sign_meshes = {}
    for n in range(41, 49):
        z, w = street_z(n), street_width(n)
        for name, ax in AVE_X.items():
            if not near(ax, z):
                continue
            for sx, sz in ((1, -1), (-1, 1)):
                px, pz = ax + sx * 14.6, z + sz * (w / 2 + 0.6)
                signs.cylinder((px, pz), 0.05, 0.15, 3.4, seg=8)
                for label, along_x in ((f'W {n} ST', True), (f'{name[:-2]} AV', False)):
                    key = label
                    if key not in sign_meshes:
                        sign_meshes[key] = (Mesh(), g.material('placa_' + label.replace(' ', '_'),
                                                               tex=rel(sign_texture(tex('rua_' + label.replace(' ', '_') + '.png'), label)),
                                                               rough=0.4))
                    m = sign_meshes[key][0]
                    yy = 3.15 if along_x else 2.9
                    if along_x:
                        for f in (1, -1):
                            m.quad((px - 0.7, yy - 0.15, pz + 0.02 * f), (px + 0.7, yy - 0.15, pz + 0.02 * f),
                                   (px + 0.7, yy + 0.15, pz + 0.02 * f), (px - 0.7, yy + 0.15, pz + 0.02 * f),
                                   uv=((0, 1), (1, 1), (1, 0), (0, 0)) if f > 0 else ((1, 1), (0, 1), (0, 0), (1, 0)),
                                   face=(0, 0, f))
                    else:
                        for f in (1, -1):
                            m.quad((px + 0.02 * f, yy - 0.15, pz + 0.7), (px + 0.02 * f, yy - 0.15, pz - 0.7),
                                   (px + 0.02 * f, yy + 0.15, pz - 0.7), (px + 0.02 * f, yy + 0.15, pz + 0.7),
                                   uv=((0, 1), (1, 1), (1, 0), (0, 0)) if f > 0 else ((1, 1), (0, 1), (0, 0), (1, 0)),
                                   face=(f, 0, 0))
            for k in range(rng.randrange(2, 5)):  # caixas de jornal coloridas em fila, perto da esquina
                bx = ax + rng.choice([-1, 1]) * rng.uniform(16.5, 20)
                bz = z + rng.choice([-1, 1]) * (w / 2 - 0.5)
                col = rng.randrange(5)
                sign_meshes.setdefault(f'_caixa{col}', (Mesh(), g.material(f'caixa_jornal{col}', color=[
                    (0.6, 0.05, 0.04), (0.05, 0.15, 0.45), (0.85, 0.75, 0.1), (0.9, 0.9, 0.88), (0.05, 0.3, 0.12)][col],
                    rough=0.35)))[0].box((bx, 0.65, bz), (0.25, 0.5, 0.22))
                obstacles.append((bx, bz, 0.5))
    g.add_mesh('rua_postes_placas', signs, g.material('poste_placa', color=(0.05, 0.05, 0.05), rough=0.4, metal=1.0))
    for key, (m, mat) in sign_meshes.items():
        g.add_mesh('rua_placa_' + key, m, mat)


# ---------------------------------------------------------------------------------------------------------- PBR
# Texturas reais CC0 do Poly Haven (polyhaven.com, licença CC0): cor, normal (OpenGL) e "arm" (oclusão, rugosidade,
# metal = o ORM do glTF), em 1K. Baixadas uma vez para --texturas (python3 tools/times_square.py baixa sozinho).
PH_IDS = ['large_red_bricks', 'red_brick_03', 'brick_wall_001', 'dark_brick_wall', 'painted_brick',
          'white_sandstone_bricks', 'large_sandstone_blocks_01', 'rectangular_facade_tiles', 'concrete_wall_006',
          'brushed_concrete', 'white_stucco', 'beige_wall_001', 'grey_plaster', 'asphalt_02', 'concrete_pavement',
          'concrete_tiles_02', 'painted_metal_shutter', 'rusty_metal_02', 'corrugated_iron', 'raw_plank_wall',
          'rough_linen', 'metal_plate']


def baixar_texturas(pasta):
    import subprocess
    for pid in PH_IDS:
        d = os.path.join(pasta, pid)
        if all(os.path.exists(os.path.join(d, f + '.jpg')) for f in ('cor', 'normal', 'arm')):
            continue
        os.makedirs(d, exist_ok=True)
        info = json.loads(subprocess.run(['curl', '-sS', f'https://api.polyhaven.com/files/{pid}'], check=True,
                                         capture_output=True).stdout)
        for key, name in (('Diffuse', 'cor'), ('nor_gl', 'normal'), ('arm', 'arm')):
            subprocess.run(['curl', '-sS', '-o', os.path.join(d, name + '.jpg'), info[key]['1k']['jpg']['url']], check=True)


class Pbr:
    """Materiais com as texturas reais: copia os mapas para tex/ uma vez e cria o material do glTF."""

    def __init__(self, g, src, out):
        self.g, self.src, self.out = g, src, out
        self.done = set()

    def maps(self, pid):
        if pid not in self.done:
            for f in ('cor', 'normal', 'arm'):
                im = Image.open(os.path.join(self.src, pid, f + '.jpg'))
                im.convert('RGB').save(os.path.join(self.out, 'tex', f'{pid}_{f}.jpg'), quality=88)
            self.done.add(pid)
        return tuple(f'tex/{pid}_{f}.jpg' for f in ('cor', 'normal', 'arm'))

    def __call__(self, name, pid, tint=(1, 1, 1), rough=1.0, metal=0.0, normal=1.0, **kw):
        # metal 0 nas alvenarias: o canal B do "arm" tem ruído de JPEG (até 0,26) que vira pontinhos de metal
        c, nm, arm = self.maps(pid)
        return self.g.material(name, color=tint, rough=rough, metal=metal, tex=c, normal_tex=nm, orm_tex=arm,
                               normal_scale=normal, **kw)


# ---------------------------------------------------------------------------------------------------------- cena
def build(out, seed, texdir):
    rng = random.Random(seed)
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    tex = lambda name: os.path.join(out, 'tex', name)
    rel = lambda p: 'tex/' + os.path.basename(p)
    g = Gltf()

    road, sidewalk, plaza, blocks, (zmin, zmax) = layout()
    baixar_texturas(texdir)
    pbr = Pbr(g, texdir, out)
    asphalt = pbr('asfalto', 'asphalt_02', tint=(0.8, 0.8, 0.82))
    walk = pbr('calcada', 'concrete_pavement', tint=(0.85, 0.84, 0.82))
    granite = pbr('granito', 'concrete_tiles_02', tint=(0.8, 0.8, 0.85))
    curb = pbr('meio_fio', 'brushed_concrete', tint=(0.75, 0.74, 0.72))
    paint = g.material('faixa', color=(0.85, 0.85, 0.8), rough=0.5)

    m = Mesh()
    m.polygon(road, 0.0, uv_scale=0.22)
    g.add_mesh('pista', m, asphalt)
    m = Mesh()
    m.polygon(sidewalk, 0.15, uv_scale=0.35)
    g.add_mesh('calcada', m, walk)
    m = Mesh()
    m.polygon(plaza, 0.15, uv_scale=0.5)
    g.add_mesh('praca', m, granite)
    m = Mesh()
    m.extrude_walls(unary_union([sidewalk, plaza]).buffer(0), 0.0, 0.15, uv_w=0.5, uv_h=0.5)
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
    roof = pbr('telhado', 'brushed_concrete', tint=(0.35, 0.34, 0.33))
    fac = Facades(rng)
    from shapely.prepared import prep
    blocks_p = prep(blocks)
    fac.is_street = lambda mid, nrm: not blocks_p.contains(Point(mid[0] + nrm[0] * 2.5, mid[1] + nrm[1] * 2.5))
    WALLS = {'tijolo': ['tijolo0', 'tijolo1', 'tijolo2', 'tijolo3', 'tijolo4'], 'pedra': ['pedra0', 'pedra1', 'pedra2'],
             'reboco': ['reboco0', 'reboco1'], 'concreto': ['concreto0']}
    TRIMS = ['cornija', 'cornija', 'cornija_branca', 'cornija_escura']
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
            bl = polys(lot.buffer(-0.3, join_style=2))
            if not bl:
                continue
            body = max(bl, key=lambda q: q.area)
            detailed = d < 260  # perto da praça: fachada com geometria; longe: textura
            top = podium
            if detailed:
                fam = rng.choices(['tijolo', 'pedra', 'reboco', 'concreto'], [5, 3, 2, 1])[0]
                wall = rng.choice(WALLS[fam])
                v = rng.uniform(0.85, 1.1)
                fac.tint = (v * rng.uniform(0.94, 1.06), v * rng.uniform(0.94, 1.06), v * rng.uniform(0.94, 1.06))
                style = {'trim': rng.choice(TRIMS), 'andaime': (not bowtie) and rng.random() < 0.1,
                         'caixilho': rng.choice(['caixilho', 'caixilho', 'caixilho_branco', 'caixilho_verde'])}
                fac.lod = 0 if d < 140 else 1
                top = fac.masonry(body, 0.15, podium, wall, st=style)
            else:
                bldg[(st, k)].extrude_walls(body, 0.15, podium, uv_w=1 / 12.0, uv_h=1 / 14.0)
            if tower > podium:
                cl = polys(body.buffer(-rng.uniform(3, 8), join_style=2))
                core = max(cl, key=lambda q: q.area) if cl else None
                if core is not None and core.area > 80:
                    roofs.polygon(body.difference(core), top + 0.05)
                    if detailed:  # torre com recuos ("bolo de noiva" do zoneamento de 1916) e coroamento
                        glass = rng.random() < 0.65
                        tiers = rng.choice([1, 2, 2, 3])
                        cur, yb, kk = core, top, rng.randrange(4)
                        fac.tint = (1.0, 1.0, 1.0)
                        for ti in range(tiers):
                            yt = yb + (tower - yb) * (1.0 / (tiers - ti)) * rng.uniform(0.85, 1.0) if ti < tiers - 1 else tower
                            last = ti == tiers - 1
                            if glass:
                                fac.curtain(cur, yb, yt, kk, crown=last)
                                yr = yt
                            else:
                                yr = fac.masonry(cur, yb, yt, rng.choice(WALLS['pedra']), storefront=False,
                                                 st={'trim': 'cornija', 'verga': 'simples', 'incendio': False, 'ar': 0.0,
                                                     'pilastras': True, 'fh': 3.8, 'cornija_grande': False,
                                                     'acesas': 0.35, 'leve': True})
                            roofs.polygon(cur, yr + (3.5 if glass and last else 0.0))
                            if not last:
                                nx = polys(cur.buffer(-rng.uniform(2.5, 5.0), join_style=2))
                                if not nx or max(nx, key=lambda q: q.area).area < 60:
                                    break
                                cur = max(nx, key=lambda q: q.area)
                            yb = yr
                        if rng.random() < 0.3 and tower > 120:  # mastro/antena no topo
                            cc = cur.centroid
                            fac.m('montante').frustum((cc.x, cc.y), 0.6, 0.08, yr, yr + rng.uniform(15, 40), seg=10)
                        else:
                            fac.roof(cur, yr + (3.5 if glass else 0.0), small=False)
                    else:
                        bldg[(rng.choice(['vidro', 'vidro', 'concreto', 'pedra']), rng.randrange(3))].extrude_walls(
                            core, podium, tower, 1 / 12.0, 1 / 14.0)
                        roofs.polygon(core, tower)
                    top = None
            if top is not None:
                roofs.polygon(body, top)
                if detailed:
                    fac.roof(body, top, small=podium < 80)
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
                    if rng.random() < 0.7:  # letreiro de neon baixo, acima da faixa de letreiros das lojas
                        neons.append((mid, (nx, nz), (b - a) / L, 5.4, min(L * 0.8, 9), 1.4))
    for k, mesh in bldg.items():
        g.add_mesh(f'predios_{k[0]}{k[1]}', mesh, facade_mats[k])
    g.add_mesh('telhados', roofs, roof)

    # Materiais das fachadas com geometria.
    fmat = {}
    for k, (pid, tint) in enumerate([('large_red_bricks', (0.85, 0.8, 0.78)), ('red_brick_03', (1, 1, 1)),
                                     ('brick_wall_001', (0.95, 0.92, 0.9)), ('dark_brick_wall', (1, 1, 1)),
                                     ('large_red_bricks', (0.62, 0.52, 0.42))]):
        fmat[f'tijolo{k}'] = pbr(f'tijolo{k}', pid, tint=tint)
    for k, (pid, tint) in enumerate([('white_sandstone_bricks', (0.95, 0.92, 0.86)),
                                     ('large_sandstone_blocks_01', (1.0, 0.97, 0.92)),
                                     ('rectangular_facade_tiles', (0.9, 0.9, 0.9))]):
        fmat[f'pedra{k}'] = pbr(f'pedra{k}', pid, tint=tint)
    fmat['reboco0'] = pbr('reboco0', 'white_stucco', tint=(0.82, 0.78, 0.7))
    fmat['reboco1'] = pbr('reboco1', 'grey_plaster', tint=(0.9, 0.86, 0.8))
    fmat['concreto0'] = pbr('concreto0', 'concrete_wall_006', tint=(0.95, 0.95, 0.95))
    fmat['cornija'] = pbr('cornija', 'beige_wall_001', tint=(0.95, 0.9, 0.82))
    fmat['cornija_branca'] = pbr('cornija_branca', 'white_stucco', tint=(0.9, 0.9, 0.88))
    fmat['cornija_escura'] = pbr('cornija_escura', 'painted_metal_shutter', tint=(0.18, 0.2, 0.19), normal=0.3)
    fmat['caixilho'] = g.material('caixilho', color=(0.05, 0.05, 0.055), rough=0.35, metal=0.0)
    fmat['caixilho_branco'] = g.material('caixilho_branco', color=(0.75, 0.74, 0.7), rough=0.4)
    fmat['caixilho_verde'] = g.material('caixilho_verde', color=(0.04, 0.12, 0.08), rough=0.4)
    fmat['montante'] = g.material('montante', color=(0.55, 0.57, 0.6), rough=0.3, metal=1.0)
    fmat['marquise'] = g.material('marquise', color=(0.04, 0.04, 0.045), rough=0.4, metal=1.0)
    fmat['faixa_luz'] = g.material('faixa_luz', color=(1, 1, 1), emissive=(1.0, 0.9, 0.75), strength=5.0)
    fmat['ferro'] = pbr('ferro', 'rusty_metal_02', tint=(0.07, 0.065, 0.06), rough=0.9, metal=1.0)
    fmat['ar_cond'] = pbr('ar_cond', 'corrugated_iron', tint=(0.78, 0.75, 0.68), normal=0.6)
    fmat['condensador'] = pbr('condensador', 'corrugated_iron', tint=(0.7, 0.7, 0.7))
    fmat['aco_teto'] = pbr('aco_teto', 'rusty_metal_02', tint=(0.3, 0.3, 0.3), metal=1.0)
    fmat['caixa_agua'] = pbr('caixa_agua', 'raw_plank_wall', tint=(0.62, 0.5, 0.4))
    fmat['telhado_casa'] = pbr('telhado_casa', 'dark_brick_wall', tint=(0.8, 0.8, 0.8))
    fmat['tapume'] = pbr('tapume', 'raw_plank_wall', tint=(0.12, 0.3, 0.18), normal=0.5)
    fmat['andaime'] = g.material('andaime', color=(0.08, 0.1, 0.16), rough=0.45, metal=0.6)
    fmat['luz_andaime'] = g.material('luz_andaime', color=(1, 1, 1), emissive=(1.0, 0.93, 0.82), strength=30.0)
    fmat['coroa'] = g.material('coroa', color=(1, 1, 1), emissive=(0.75, 0.85, 1.0), strength=40.0)
    for k, cor in enumerate([(0.45, 0.04, 0.04), (0.05, 0.18, 0.08), (0.06, 0.08, 0.25), (0.08, 0.08, 0.08),
                             (0.5, 0.28, 0.04), (0.3, 0.05, 0.18)]):
        fmat[f'toldo{k}'] = pbr(f'toldo{k}', 'rough_linen', tint=cor, double=True)
    for k in range(16):  # letreiros de loja (caixa de luz com nome fictício)
        p = rel(neon_texture(tex(f'placa{k:02d}.png'), rng))
        fmat[f'placa{k}'] = g.material(f'placa{k}', tex=p, emissive=(1, 1, 1), emissive_tex=p, strength=2.5)
    for k in range(8):  # vidro com a sala acesa atrás (reflexo do vidro + interior emissivo)
        r = rel(room_texture(tex(f'sala{k}.png'), rng, True))
        fmat[f'sala{k}'] = g.material(f'sala{k}', color=(0.5, 0.5, 0.5), rough=0.05, tex=r, emissive=(1, 1, 1),
                                      emissive_tex=r, strength=0.8)
    for k in range(3):
        r = rel(room_texture(tex(f'sala_apagada{k}.png'), rng, False))
        fmat[f'sala_apagada{k}'] = g.material(f'sala_apagada{k}', rough=0.04, tex=r)
    for k, kind in enumerate(['quente', 'quente', 'fria', 'colorida', 'colorida', 'fechada']):
        r = rel(shop_texture(tex(f'vitrine{k}.png'), rng, kind))
        if kind == 'fechada':  # porta de enrolar de aço
            fmat[f'vitrine{k}'] = pbr(f'vitrine{k}', 'painted_metal_shutter', tint=(0.75, 0.75, 0.75))
        else:
            fmat[f'vitrine{k}'] = g.material(f'vitrine{k}', color=(0.6, 0.6, 0.6), rough=0.05, tex=r,
                                             emissive=(1, 1, 1), emissive_tex=r, strength=1.2)
    for k in range(4):
        c, e = curtain_texture(tex(f'cortina{k}'), rng)
        fmat[f'cortina{k}'] = g.material(f'cortina{k}', rough=0.06, metal=0.3, tex=rel(c), emissive=(1, 1, 1),
                                         emissive_tex=rel(e), strength=0.6)
    brick_src = os.path.join(texdir, 'large_red_bricks', 'cor.jpg')
    for k in range(12):  # anúncios pintados e murais nas empenas
        r = rel(mural_texture(tex(f'mural{k:02d}.png'), rng, brick_src, k))
        fmat[f'mural{k}'] = g.material(f'mural{k}', tex=r, rough=0.85)
    for name, mesh in fac.mesh.items():
        g.add_mesh('fachada_' + name, mesh, fmat[name])
    for pos, col, inten, rr in fac.lights:
        g.light(pos, col, inten, rr)

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
    obstacles = []  # (x, z, raio): a multidão desvia (cenas/times_square.json, "obstaculos")
    for n in range(41, 49):
        z, w = street_z(n), street_width(n)
        for ax in (AVE_X['7th'], BWAY_K * z):
            for sx in (-1, 1):
                for sz in (-1, 1):  # semáforo em cada canto do cruzamento
                    px, pz = ax + sx * 15.6, z + sz * (w / 2 + 0.6)
                    if abs(ax) < 1e-6 and abs(BWAY_K * z) < 25 and sx < 0:
                        continue
                    pr['escuro'].frustum((px, pz), 0.14, 0.09, 0.15, 5.4, seg=10)
                    pr['escuro'].frustum((px, pz), 0.2, 0.16, 0.15, 0.9, seg=10)
                    obstacles.append((px, pz, 0.6))
                    arm_dir = -sx
                    pr['escuro'].tube([(px + arm_dir * 6.2 * f, 5.0 + 0.35 * f * (1 - f), pz) for f in np.linspace(0, 1, 6)],
                                      0.07, seg=7)
                    pr['escuro'].beam((px, 4.2, pz), (px + arm_dir * 1.6, 4.95, pz), 0.05, 0.05)  # mão-francesa
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
                obstacles.append((xc + side * 7.5, zc, 2.1))
                pr['granito'].box((xc + side * 7.5, 0.37, zc), (0.3, 0.22, 1.7), rot=math.atan(BWAY_K))
            else:
                pr['granito'].box((xc + side * 7.0, 0.55, zc), (0.9, 0.4, 0.9))
                pr['planta'].sphere((xc + side * 7.0, 1.25, zc), 0.8, 10)
                obstacles.append((xc + side * 7.0, zc, 1.4))
    # Entradas de metrô (escada com grade e os dois globos verdes acesos) e bancas de jornal.
    for (x, z) in ((-21.0, street_z(42) + 9), (24.0, street_z(42) - 9), (-21.0, street_z(45) + 9), (21.0, street_z(47) - 9)):
        for sx in (-1, 1):
            pr['escuro'].box((x + sx * 1.3, 0.65, z), (0.04, 0.5, 2.6))
            pr['escuro'].cylinder((x + sx * 1.3, z + 2.6), 0.05, 0.15, 2.4)
            pr['globo'].sphere((x + sx * 1.3, 2.6, z + 2.6), 0.2, 10)
            g.light((x + sx * 1.3, 2.6, z + 2.6), (0.3, 1.0, 0.45), 60.0, 12.0)
        pr['escuro'].box((x, 0.65, z - 2.6), (1.3, 0.5, 0.04))
        obstacles.append((x, z, 3.2))
    for (x, z) in ((16.5, street_z(43) - 14), (-17.0, street_z(46) + 20), (-17.5, street_z(44) - 25)):
        pr['banca'].box((x, 1.4, z), (1.2, 1.25, 2.2))
        pr['banca'].box((x + 0.3, 2.8, z), (1.6, 0.06, 2.5))
        pr['banca_luz'].box((x + 1.21, 1.6, z), (0.01, 0.6, 1.9))
        obstacles.append((x, z, 2.8))
    for key, mat in (('aco', steel), ('escuro', dark), ('semaforo', yellow), ('vermelho', red_l), ('verde', green_l),
                     ('pedestre', walk_l), ('hidrante', hydrant), ('lixeira', bin_mat), ('granito', granite_d),
                     ('globo', globe), ('banco', wood), ('planta', plant), ('banca', stand), ('banca_luz', stand_l),
                     ('bueiro', manhole)):
        g.add_mesh('rua_' + key, props[key], mat)
    with open(os.path.join(out, 'vapor.json'), 'w') as f:
        json.dump(steam_points, f)
    with open(os.path.join(out, 'obstaculos.json'), 'w') as f:
        json.dump([[round(x, 2), round(z, 2), r] for x, z, r in obstacles + fac.obstacles], f)

    street_details(g, pbr, rng, road, sidewalk, plaza, near_plaza, obstacles, tex, rel, texdir)
    with open(os.path.join(out, 'obstaculos.json'), 'w') as f:  # de novo: árvores, lixo e caixas entraram
        json.dump([[round(x, 2), round(z, 2), r] for x, z, r in obstacles + fac.obstacles], f)

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
                # poste afunilado com braço curvo sobre a pista e luminária "cabeça de cobra"
                sz = 1.0 if z > zz else -1.0
                poles.frustum((x, zz), 0.16, 0.07, 0.15, 8.2, seg=10)
                poles.frustum((x, zz), 0.22, 0.17, 0.15, 1.1, seg=10)  # base mais grossa
                arm = [(x, 8.0 + 1.0 * math.sin(f * math.pi / 2), zz + sz * 1.2 * (1 - math.cos(f * math.pi / 2)) +
                        sz * 1.3 * max(0.0, f - 1)) for f in np.linspace(0, 2.0, 12)]
                poles.tube(arm, 0.05, seg=7)
                hx, hy, hz = arm[-1]
                poles.sphere((hx, hy - 0.05, hz + sz * 0.25), 0.42, 12, sy=0.32)
                lamps.cylinder((hx, hz + sz * 0.25), 0.26, hy - 0.2, hy - 0.17, seg=12)
                g.light((hx, hy - 0.3, hz + sz * 0.25), (1.0, 0.85, 0.65), 1800.0)
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
    ap.add_argument('--texturas', default='/home/user/p3/downloads/ph',
                    help='pasta das texturas CC0 do Poly Haven (baixadas sozinhas se faltarem)')
    ap.add_argument('--gltfpack', help='gltfpack (npm install gltfpack): comprime a malha (meshopt + quantização), '
                                       '~9x menor; sem ele o .bin sai cru (~180 MB)')
    a = ap.parse_args()
    if not a.gltfpack:
        build(a.saida, a.semente, a.texturas)
        return
    import shutil
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        build(tmp, a.semente, a.texturas)
        os.makedirs(a.saida, exist_ok=True)
        subprocess.run([a.gltfpack, '-i', os.path.join(tmp, 'times_square.gltf'), '-o',
                        os.path.join(a.saida, 'times_square.gltf'), '-cc', '-kn', '-km', '-ke'], check=True)
        for extra in ('vapor.json', 'obstaculos.json'):
            shutil.copyfile(os.path.join(tmp, extra), os.path.join(a.saida, extra))
        # O gltfpack copia as texturas para o lado do .gltf (e aponta para lá): a pasta tex/ antiga sobra.
        shutil.rmtree(os.path.join(a.saida, 'tex'), ignore_errors=True)


if __name__ == '__main__':
    main()
