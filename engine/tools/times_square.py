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


def interior_image(rng, w, h, room_w, room_h, depth, kind, lit, tint):
    """Interior em perspectiva visto pela janela (o "interior mapping" assado numa imagem, como o vidro de uma
    vitrine visto de frente): cada pixel lança um raio do olho, 1,5 m à frente do vidro, para dentro de uma caixa
    (fundo, chão, teto e laterais) e pega a cor da superfície que acerta: piso de tábua ou ladrilho, paredes com
    rodapé, teto com luminárias e queda de luz, prateleiras de produtos nas laterais (loja), quadro e estante (sala);
    silhuetas de móveis e de gente em profundidade. Assim o vidro mostra uma sala de verdade, não um desenho chapado."""
    ys, xs = np.mgrid[0:h, 0:w]
    px = (xs + 0.5) / w * room_w            # ponto no vidro (m), x da esquerda, y de baixo
    py = (1 - (ys + 0.5) / h) * room_h
    ex, ey, ez = room_w / 2 + rng.uniform(-0.2, 0.2), room_h * 0.55, -1.5
    dx, dy, dz = px - ex, py - ey, 0 - ez
    big = 1e9
    t_back = np.full(px.shape, (depth - ez) / dz)
    t_floor = np.where(dy < 0, (0 - ey) / np.minimum(dy, -1e-6), big)
    t_ceil = np.where(dy > 0, (room_h - ey) / np.maximum(dy, 1e-6), big)
    t_left = np.where(dx < 0, (0 - ex) / np.minimum(dx, -1e-6), big)
    t_right = np.where(dx > 0, (room_w - ex) / np.maximum(dx, 1e-6), big)
    t0 = (0 - ez) / dz  # o raio entra pelo vidro em z = 0
    T = np.stack([t_back, t_floor, t_ceil, t_left, t_right])
    T = np.where(T < t0, big, T)
    hit = np.argmin(T, 0)
    t = np.take_along_axis(T, hit[None], 0)[0]
    X, Y, Z = ex + dx * t, ey + dy * t, ez + dz * t
    img = np.zeros((h, w, 3))
    wallc = np.array(tint, float)
    noise = rng_img(rng, w, h)
    # piso
    if kind == 'loja':
        tile = ((np.floor(X / 0.4) + np.floor(Z / 0.4)) % 2) * 0.12 + 0.75
        floorc = np.array([210, 205, 195]) * tile[..., None]
    else:
        plank = 0.75 + 0.25 * ((np.floor(X / 0.18) * 7.3) % 1.0)[..., None]
        floorc = np.array([120, 80, 50]) * plank
    m = hit == 1
    img[m] = floorc[m]
    # teto
    m = hit == 2
    img[m] = wallc * 0.85
    # paredes (fundo e laterais), com rodapé
    for k in (0, 3, 4):
        m = hit == k
        img[m] = wallc * (0.92 if k == 0 else 0.8)
    base = (hit != 1) & (hit != 2) & (Y < 0.12)
    img[base] = (60, 50, 45)
    sub = rng.choice(['mercado', 'roupa', 'restaurante']) if kind == 'loja' else None
    if sub == 'roupa':  # araras com roupas penduradas nas laterais (faixas verticais coloridas na altura do peito)
        rack = ((hit == 3) | (hit == 4)) & (Y > 0.6) & (Y < 1.6)
        item = np.floor(Z / 0.07)
        hue = (np.sin(item * 78.233) * 43758.5453) % 1.0
        pal = np.array([[30, 30, 35], [200, 200, 195], [150, 30, 40], [40, 60, 120], [190, 160, 120], [80, 90, 70]], float)
        img[rack] = pal[(hue[rack] * 6).astype(int) % 6] * (0.8 + 0.2 * ((Y[rack] - 0.6) / 1.0))[..., None]
        img[((hit == 3) | (hit == 4)) & (np.abs(Y - 1.62) < 0.02)] = (160, 160, 165)
    elif sub == 'restaurante':  # parede de tijolo/madeira, quadros, balcão no fundo
        wood = (hit == 3) | (hit == 4)
        img[wood & (Y < 1.1)] = (90, 55, 35)
        img[(hit == 0) & (Y < 1.1)] = (70, 40, 28)
    if sub == 'mercado':  # prateleiras de produtos nas laterais e no fundo
        shelf = ((hit == 3) | (hit == 4) | (hit == 0)) & (Y > 0.15) & (Y < 2.1)
        row = np.floor((Y - 0.15) / 0.38)
        frac = ((Y - 0.15) / 0.38) % 1
        along = np.where(hit == 0, X, Z)
        item = np.floor(along / 0.11) + row * 13
        hue = (np.sin(item * 12.9898) * 43758.5453) % 1.0
        prod = shelf & (frac > 0.12) & (frac < 0.12 + 0.75 * (0.5 + 0.5 * ((hue * 7) % 1)))
        pal = np.array([[220, 50, 50], [240, 200, 60], [60, 140, 230], [250, 250, 245], [60, 180, 90], [240, 120, 40],
                        [150, 70, 180], [30, 30, 35]], float)
        img[prod] = pal[(hue[prod] * 8).astype(int) % 8]
        board = shelf & (frac <= 0.1)
        img[board] = (90, 85, 80)
    else:  # sala: quadro na parede do fundo, estante numa lateral
        pic = (hit == 0) & (np.abs(X - room_w * rng.uniform(0.3, 0.7)) < 0.35) & (np.abs(Y - 1.5) < 0.25)
        img[pic] = np.array(rng.choice([(40, 70, 110), (150, 60, 40), (30, 90, 60), (200, 170, 90)]))
        side = 3 if rng.random() < 0.5 else 4
        books = (hit == side) & (Z > depth * 0.3) & (Z < depth * 0.75) & (Y < 1.9)
        bk = ((np.floor(Z / 0.05) * 5.1 + np.floor(Y / 0.35) * 3.7) % 1.0)
        img[books] = (np.array([120, 70, 40]) * 0.5 + np.array([200, 150, 90]) * bk[..., None] * 0.6)[books]
    # luz: luminária no teto perto do centro, queda com a distância, cantos mais escuros (oclusão)
    lx, ly, lz = room_w / 2, room_h - 0.05, depth * 0.45
    dist2 = (X - lx) ** 2 + (Y - ly) ** 2 + (Z - lz) ** 2
    light = 0.35 + 1.6 / (1.0 + dist2 * 0.6)
    corner = np.minimum.reduce([X, room_w - X, Y, room_h - Y, depth - Z + 0.3]) / 0.4
    light *= 0.6 + 0.4 * np.clip(corner, 0, 1)
    if kind == 'loja':  # fileiras de luminárias fluorescentes no teto
        tubes = (hit == 2) & ((Z % 1.2) < 0.12) & (np.abs(X - room_w / 2) < room_w * 0.35)
        img[tubes] = 255
        light = light * 1.15
    else:
        lamp = (hit == 2) & ((X - lx) ** 2 + (Z - lz) ** 2 < 0.06)
        img[lamp] = 255
    if not lit:
        light = light * 0.06 + 0.02
    img = img * light[..., None] * (0.92 + 0.16 * noise[..., None])
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    # silhuetas em profundidade (móveis, gente): projetadas pela mesma câmera
    d = ImageDraw.Draw(im)

    def proj(x, y, z):
        s = (0 - ez) / (z - ez)
        return ((ex + (x - ex) * s) / room_w * w, (1 - (ey + (y - ey) * s) / room_h) * h)
    dark = (28, 24, 22) if lit else (6, 6, 8)
    for _ in range(rng.randrange(1, 4) if kind != 'loja' else rng.randrange(0, 4)):
        z = rng.uniform(1.8, depth * 0.9)
        x = rng.uniform(0.3, room_w - 0.3)
        if rng.random() < (0.7 if kind == 'loja' else 0.3):  # pessoa
            (a1, b1), (a2, b2) = proj(x - 0.22, 1.45, z), proj(x + 0.22, 0.0, z)
            hx, hy = proj(x, 1.6, z)
            hr = abs(proj(x + 0.11, 1.6, z)[0] - hx)
            d.rectangle([a1, b1, a2, b2], fill=dark)
            d.ellipse([hx - hr, hy - hr * 1.3, hx + hr, hy + hr * 0.9], fill=dark)
        else:  # móvel (sofá, mesa, balcão): sombreado, com tampo mais claro, não um bloco preto
            hw, hh = rng.uniform(0.2, 0.5), rng.uniform(0.4, 0.8)
            base_c = np.array(rng.choice([(90, 60, 40), (60, 60, 70), (120, 100, 80), (50, 70, 60)])) * (0.6 if lit else 0.08)
            (a1, b1), (a2, b2) = proj(x - hw, hh, z), proj(x + hw, 0.0, z)
            d.rectangle([a1, b1, a2, b2], fill=tuple(int(c) for c in base_c))
            (c1, e1), (c2, e2) = proj(x - hw, hh, z + 0.4), proj(x + hw, hh, z)
            d.rectangle([c1, e1, c2, e2], fill=tuple(int(min(255, c * 1.6)) for c in base_c))
    return im


def rng_img(rng, w, h):
    n = np.random.default_rng(rng.randrange(1 << 30)).random((h // 8 + 1, w // 8 + 1))
    return np.asarray(Image.fromarray((n * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)) / 255.0


def room_texture(path, rng, lit, w=192, h=320):
    """Sala atrás da janela (apartamento ou escritório), em perspectiva; acesa (cor de lâmpada 2700–5000 K) ou
    apagada (só um resto de luz da rua)."""
    tint = rng.choice([(235, 215, 180), (220, 225, 235), (250, 230, 200), (200, 215, 230), (245, 205, 160),
                       (230, 230, 225)])
    k = rng.uniform(0.55, 1.0) if lit else 1.0
    im = interior_image(rng, w, h, 1.2, 2.0, rng.uniform(3.0, 5.0), 'sala', lit, tuple(c * k for c in tint))
    if lit and rng.random() < 0.4:  # cortina ou persiana cobrindo parte do vidro
        d = ImageDraw.Draw(im)
        if rng.random() < 0.5:
            yy = int(h * rng.uniform(0.15, 0.6))
            for y in range(0, yy, 6):
                d.rectangle([0, y, w, y + 3], fill=tuple(int(c * 0.7) for c in tint))
        else:
            xx = int(w * rng.uniform(0.2, 0.45))
            d.rectangle([0, 0, xx, h], fill=tuple(int(c * 0.55) for c in tint))
    im.save(path)
    return path


def shop_texture(path, rng, kind, w=384, h=256):
    """Vitrine: interior da loja em perspectiva (piso de ladrilho, prateleiras de produtos, luminárias de tubo,
    clientes) ou, fechada, a porta de aço de enrolar (essa usa a textura real de porta de aço no material)."""
    if kind == 'fechada':
        img = np.ones((h, w, 3)) * np.array([70, 72, 76])[None, None]
        img[::6] *= 0.55
        Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)
        return path
    tint = {'quente': (255, 225, 180), 'fria': (225, 240, 255), 'colorida': rng.choice([(255, 150, 200), (150, 210, 255),
                                                                                        (255, 210, 120), (170, 255, 190)])}[kind]
    im = interior_image(rng, w, h, 3.0, 3.2, rng.uniform(5.0, 9.0), 'loja', True, tint)
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

    PLAIN = ('vidro_loja', 'escorrido', 'mural', 'grafite', 'cartaz', 'umidade', 'sala', 'vitrine', 'caixilho', 'ferro', 'faixa_luz', 'toldo', 'luz_', 'placa', 'ar_cond', 'montante',
             'cortina', 'coroa', 'tapume', 'andaime', 'aco_teto', 'condensador', 'caixa_agua')

    def __init__(self, rng):
        self.rng = rng
        self.mesh = {}
        self.tint = (1.0, 1.0, 1.0)
        self.grime = None
        self.obstacles = []
        self.lights = []
        self.lod = 0  # 0 = detalhe completo (perto da praça); 1 = janelas simples
        self.ao = 1.0  # oclusão de contato da peça que está sendo feita
        self.bevel = 0.0  # chanfro das arestas da frente dos blocos (m)
        self.is_street = None  # (ponto médio, normal) -> a fachada dá para a rua? (senão é empena de divisa)
        self.lojas = []  # (tipo, posição da porta, giro): interiores 3D a encaixar atrás das vitrines
        self.lojas_restantes = 0
        self.lojas_plantas = []  # plantas das lojas já postas (uma loja não pode cair dentro de outra)
        self.loja_ok = False  # o prédio atual pode ter loja com interior 3D (perto da praça)
        self._poly = None
        self.n_murais = 0

    def m(self, name):
        """Malha do material; a cor de vértice leva o tom do prédio, a sujeira e a oclusão de contato (self.ao:
        < 1 dentro dos vãos e embaixo das saliências, oclusão assada na cor de vértice)."""
        M = self.mesh.setdefault(name, Mesh())
        k = self.ao
        if name.startswith(self.PLAIN):
            M.tint, M.grime = (k, k, k), None
        else:
            M.tint, M.grime = (self.tint[0] * k, self.tint[1] * k, self.tint[2] * k), self.grime
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
        """Os 4 lados do vão da janela (da fachada até o vidro), mais escuros: pouca luz entra no vão."""
        self.ao = 0.5
        P, M = self.P, self.m(frame)
        self.ao = 1.0
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
        """Caixa encostada na fachada (peitoril, verga, pilastra, mísula, ar-condicionado): u0..u1, y0..y1, d0..d1.
        Com self.bevel > 0, as arestas da frente saem chanfradas (a quina pega um filete de luz, como nas peças dos
        jogos; quina viva de 90° é o que dá a cara de "caixa de computador")."""
        b = self.bevel
        if b > 0 and d1 - d0 > 2.5 * b and u1 - u0 > 3 * b and y1 - y0 > 3 * b:
            self._block(name, a, t, n, u0, u1, y0, y1, d0, d1 - b, uvs, back)
            P, M = self.P, self.m(name)
            o = [P(a, t, n, u0, y0, d1 - b), P(a, t, n, u1, y0, d1 - b), P(a, t, n, u1, y1, d1 - b), P(a, t, n, u0, y1, d1 - b)]
            i = [P(a, t, n, u0 + b, y0 + b, d1), P(a, t, n, u1 - b, y0 + b, d1), P(a, t, n, u1 - b, y1 - b, d1),
                 P(a, t, n, u0 + b, y1 - b, d1)]
            N = np.array([n[0], 0.0, n[1]])
            T = np.array([t[0], 0.0, t[1]])
            Y = np.array([0.0, 1.0, 0.0])
            M.quad(*i, uv=((u0 * uvs, -y0 * uvs), (u1 * uvs, -y0 * uvs), (u1 * uvs, -y1 * uvs), (u0 * uvs, -y1 * uvs)),
                   face=tuple(N))
            for k, side in enumerate((-Y, T, Y, -T)):  # baixo, direita, cima, esquerda
                j = (k + 1) % 4
                M.quad(o[k], o[j], i[j], i[k], uv=((0, 0), (1, 0), (1, b), (0, b)), face=tuple(side + N))
            return
        self._block(name, a, t, n, u0, u1, y0, y1, d0, d1, uvs, back)

    def _block(self, name, a, t, n, u0, u1, y0, y1, d0, d1, uvs=0.5, back=False):
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
    def window(self, st, a, t, n, c, ww, wy0, wy1, depth, wall, trim, lit=None):
        """Módulo de janela: vidro com sala atrás, vão, caixilho de guilhotina, peitoril, e verga/arco/frontão.
        lit: a sala está acesa (decidido por apartamento, não por janela). Às vezes a janela foi fechada com tijolo
        (reforma antiga) e embaixo do peitoril escorre sujeira."""
        rng = self.rng
        u0, u1 = c - ww / 2, c + ww / 2
        if lit is None:
            lit = rng.random() < st['acesas']
        if self.lod == 0 and rng.random() < 0.03:  # vão emparedado
            self.ao = 0.85
            self.wall_quad(wall, a, t, n, u0, u1, wy0, wy1, -0.08)
            self.ao = 1.0
            self.recess(a, t, n, u0, u1, wy0, wy1, 0.08, wall)
            self.block(trim, a, t, n, u0 - 0.08, u1 + 0.08, wy0 - 0.1, wy0, -0.02, 0.07)
            return
        self.wall_quad(f'sala{rng.randrange(8)}' if lit else f'sala_apagada{rng.randrange(3)}', a, t, n,
                       u0, u1, wy0, wy1, -depth, unit=True)
        if self.lod == 0 and rng.random() < 0.45:  # escorrido sob o peitoril (a chuva lava a sujeira do peitoril)
            h = rng.uniform(0.5, 1.8)
            self.wall_quad(f'escorrido{rng.randrange(4)}', a, t, n, u0 - 0.04, u1 + 0.04, wy0 - 0.1 - h, wy0 - 0.1,
                           0.006, unit=True)
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
        u = 0.3  # cartazes colados no tapume e nos pilares (shows, filmes, festas: tudo fictício)
        while u < L - 1.0:
            pw = self.rng.uniform(0.6, 1.0)
            if self.rng.random() < 0.7:
                self.wall_quad(f'cartaz{self.rng.randrange(10)}', a, t, n, u, u + pw, y0 + H + 0.25,
                               y0 + H + 0.25 + pw * 1.2 * 0.75, D + 0.004, unit=True)
            u += pw + self.rng.uniform(0.0, 0.8)
        for uu in np.arange(0.3, L, 2.4):
            if self.rng.random() < 0.5:
                self.wall_quad(f'cartaz{self.rng.randrange(10)}', a, t, n, uu - 0.3, uu + 0.3, y0 + 1.2, y0 + 2.0,
                               D - 0.035, unit=True)
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
        # Loja de verdade (interior 3D) atrás de alguns vãos: só perto da praça, com fachada larga e prédio fundo.
        loja, porta = set(), -1
        if self.loja_ok and self.lojas_restantes > 0 and self._poly is not None and rng.random() < 0.9:
            tipo, Wi, Di = rng.choices([('conveniencia', 10.0, 12.0), ('joalheria', 12.0, 14.0), ('lanchonete', 10.0, 12.0), ('roupas', 10.0, 12.0)], [2, 1, 2, 2])[0]
            m = int(Wi // bw)
            if m >= 2 and nb >= m:
                i0 = rng.randrange(0, nb - m + 1)
                uc = (i0 + m / 2) * bw
                cantos = [self.P(a, t, n, uc + su * Wi / 2, 0, -(Di + 0.6)) for su in (-1, 1)]
                pe = [self.P(a, t, n, uc + su * Wi / 2, 0, -dd) for su, dd in ((-1, 0.3), (1, 0.3), (1, Di + 0.6), (-1, Di + 0.6))]
                planta = Polygon([(q[0], q[2]) for q in pe])
                livre = not any(planta.intersects(o) for o in self.lojas_plantas)  # duas lojas no mesmo espaço: não
                if livre and all(self._poly.contains(Point(c[0], c[2])) for c in cantos):
                    self.lojas_plantas.append(planta.buffer(0.3))
                    loja = set(range(i0, i0 + m))
                    porta = i0 + m // 2  # o vão do meio é a porta: sem vidro e sem embasamento (dá para entrar)
                    x, _, z = self.P(a, t, n, uc, 0, -0.34)
                    self.lojas.append((tipo, (x, y0, z), math.atan2(-n[0], -n[1])))
                    self.lojas_restantes -= 1
        for i in range(nb):
            u0, u1 = i * bw + 0.35, (i + 1) * bw - 0.35
            yt = y0 + 3.6
            if i == porta:  # entrada da loja: vão aberto até o chão, com batentes e a verga
                self.recess(a, t, n, u0, u1, y0, yt, 0.3, 'caixilho')
                self.block('caixilho', a, t, n, u0, u1, yt - 0.08, yt, -0.3, -0.22)
                self.block(trim, a, t, n, i * bw, u0, y0, y0 + ground, 0.0, 0.12)
                self.block(trim, a, t, n, u1, (i + 1) * bw, y0, y0 + ground, 0.0, 0.12)
                self.wall_quad(wall, a, t, n, u0, u1, yt, y0 + ground)
                continue
            if i in loja:  # vitrine de vidro de verdade: o interior 3D aparece atrás
                self.wall_quad('vidro_loja', a, t, n, u0, u1, y0 + 0.45, yt, -0.3)
                self.recess(a, t, n, u0, u1, y0 + 0.45, yt, 0.3, 'caixilho')
                self.block('caixilho', a, t, n, u0, u1, yt - 0.08, yt, -0.3, -0.22)
                self.block(trim, a, t, n, i * bw, u0, y0, y0 + ground, 0.0, 0.12)
                self.block(trim, a, t, n, u1, (i + 1) * bw, y0, y0 + ground, 0.0, 0.12)
                self.block(trim, a, t, n, u0, u1, y0, y0 + 0.45, 0.0, 0.06)
                self.wall_quad(wall, a, t, n, u0, u1, yt, y0 + ground)
                continue
            kind = rng.randrange(6) if rng.random() < lit_store else 5
            self.wall_quad(f'vitrine{kind}', a, t, n, u0, u1, y0 + 0.45, yt, -0.3, unit=True)
            if kind == 5 and rng.random() < 0.75:  # porta de enrolar fechada: pichada
                gh = rng.uniform(1.0, 2.4)
                gw = min(u1 - u0, rng.uniform(1.5, 3.2))
                gu = rng.uniform(u0, u1 - gw)
                self.wall_quad(f'grafite{rng.randrange(8)}', a, t, n, gu, gu + gw, y0 + 0.6, y0 + 0.6 + gh, -0.295,
                               unit=True)
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
                self.wall_quad(f'placa{rng.randrange(16)}', a, t, n, u0 + 0.05, u1 - 0.05, y0 + 3.8, y0 + 4.55, 0.32,
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
        for _ in range(self.rng.randrange(1, 4)):  # manchas de umidade grandes na empena
            sw, sh = self.rng.uniform(2, min(8, L)), self.rng.uniform(2, 7)
            su, sy = self.rng.uniform(0, max(0.01, L - sw)), self.rng.uniform(y0, max(y0 + 0.01, y1 - sh))
            self.wall_quad(f'umidade{self.rng.randrange(3)}', a, t, n, su, su + sw, sy, sy + sh, 0.005, unit=True)
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
        self._poly = poly
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
                apt = []  # apartamentos de 2 a 4 vãos: acesos ou apagados juntos
                while len(apt) < nb:
                    on = rng.random() < st['acesas']
                    apt += [on] * rng.randrange(2, 5)
                for i in range(nb):
                    c = (i + 0.5) * bw
                    u0, u1 = c - ww / 2, c + ww / 2
                    wy0, wy1 = fy + sill, fy + fh - head
                    self.window(st, a, t, n, c, ww, wy0, wy1, depth, wall, trim,
                                lit=apt[i] if rng.random() > 0.12 else not apt[i])
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
            if self.lod == 0 and not st.get('cunhais') and floors:  # quina: faixa vertical chanfrada (sem quina viva)
                for u0q, u1q in ((0.0, 0.3), (L - 0.3, L)):
                    self.block(wall, a, t, n, u0q, u1q, floors[0], y1 - 0.4, 0.0, 0.07)
            if self.lod == 0 and st.get('cunhais') and floors:  # cunhais: pedras alternadas nas quinas
                yy, k2 = floors[0], 0
                while yy + 0.62 < y1 - 0.4:
                    wq = 0.55 if k2 % 2 == 0 else 0.32
                    for u0q, u1q in ((0.0, wq), (L - wq, L)):
                        self.block(trim, a, t, n, u0q, u1q, yy + 0.02, yy + 0.6, 0.0, 0.05)
                    yy += 0.62
                    k2 += 1
            if self.lod == 0:  # manchas escorrendo da cornija (a água pinga da saliência)
                u = rng.uniform(0, 2)
                while u < L - 0.5:
                    wst = rng.uniform(0.8, 3.0)
                    if rng.random() < 0.6:
                        self.wall_quad(f'escorrido{rng.randrange(4)}', a, t, n, u, min(L, u + wst),
                                       y1 - 0.4 - rng.uniform(1.5, 5.0), y1 - 0.4, 0.006, unit=True)
                    u += wst + rng.uniform(0.3, 2.5)
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
            if getattr(self, 'sem_luz', False):  # brilha, mas não vira luz de malha (o lugar já tem luzes pontuais)
                m['extras'] = {'legacy_sem_luz': 1}
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

    def instance(self, mesh_ids, pos, yaw):
        """Mais uma cópia (nó do glTF) das malhas já gravadas, girada yaw rad em torno de y: o motor desenha as
        cópias por instância (uma BLAS por malha), sem duplicar vértices."""
        q = [0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2)]
        for mi in mesh_ids:
            self.doc['nodes'].append({'mesh': mi, 'translation': [float(v) for v in pos], 'rotation': q})
            self.doc['scenes'][0]['nodes'].append(len(self.doc['nodes']) - 1)

    def add_mesh(self, name, mesh, mat, scene_node=True):
        if not mesh.idx:
            return None
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
        if scene_node:
            self.doc['nodes'].append({'name': name, 'mesh': len(self.doc['meshes']) - 1})
            self.doc['scenes'][0]['nodes'].append(len(self.doc['nodes']) - 1)
        return len(self.doc['meshes']) - 1

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


def mobiliario(g, pbr, rng, plaza, sidewalk, near_plaza, obstacles, steam_points, tex, rel):
    """Mobiliário da Times Square de verdade (medidas da pesquisa, marcas fictícias): carrinhos de comida com guarda-sol,
    luz e fumaça; chaminés de vapor listradas laranja e branco (o vapor sai pela cena: vapor.json); quiosques de Wi-Fi
    com tela; lixeiras solares; barreiras azuis de contenção; abrigos de ônibus com anúncio iluminado; estações de
    bicicleta compartilhada."""
    near = lambda x, z, r=200: Point(x, z).distance(near_plaza) < r
    M = {k: Mesh() for k in ('aco', 'carrinho', 'guarda_sol', 'luz_quente', 'listra_l', 'listra_b', 'quiosque', 'tela',
                             'lixeira', 'painel_solar', 'barreira', 'vidro', 'bike', 'bike_quadro', 'anuncio_abrigo')}
    # carrinhos de comida (1,5 x 3 m, guarda-sol listrado, lâmpada pendurada, fumaça da chapa)
    walk = unary_union([plaza, sidewalk])
    minx, minz, maxx, maxz = walk.bounds
    feitos = 0
    while feitos < 22:
        x, z = rng.uniform(minx, maxx), rng.uniform(minz, maxz)
        if not near(x, z, 160) or not walk.contains(Point(x, z).buffer(2.0)):
            continue
        rot = rng.choice([0.0, math.pi / 2])
        M['carrinho'].box((x, 0.85, z), (0.75, 0.55, 1.5), rot=rot)
        M['aco'].box((x, 1.42, z), (0.78, 0.02, 1.52), rot=rot)
        for dx, dz in ((-0.6, -1.3), (0.6, -1.3), (-0.6, 1.3), (0.6, 1.3)):
            M['aco'].cylinder((x + dx, z + dz), 0.17, 0.15, 0.3, seg=10)
        M['aco'].cylinder((x, z), 0.025, 1.42, 2.6, seg=6)
        M['guarda_sol'].frustum((x, z), 1.5, 0.05, 2.35, 2.75, seg=12)
        M['luz_quente'].sphere((x, 2.2, z), 0.07, 8)
        g.light((x, 2.15, z), (1.0, 0.75, 0.45), 60.0, 6.0)
        steam_points.append((float(x + 0.3), float(z)))
        obstacles.append((x, z, 1.8))
        feitos += 1
    # chaminés de vapor (laranja e branco, ~3 m) sobre bueiros nas ruas transversais e avenidas
    for k in range(10):
        n = rng.randrange(41, 48)
        z = street_z(n) + rng.uniform(-2.5, 2.5)
        x = rng.choice([-120.0, -60.0, 60.0, 120.0, 200.0]) + rng.uniform(-10, 10)
        if not near(x, z, 220):
            continue
        for j in range(6):  # faixas alternadas
            M['listra_l' if j % 2 == 0 else 'listra_b'].frustum((x, z), 0.32, 0.3, 0.02 + j * 0.5, 0.02 + (j + 1) * 0.5, seg=14,
                                                                 cap=(j == 5))
        steam_points.append((float(x), float(z)))
        obstacles.append((x, z, 0.8))
    # quiosques de Wi-Fi (0,89 x 0,28 x 2,9 m, tela dos dois lados) e lixeiras solares (1,26 m) nas calçadas
    for n in range(41, 49):
        z0, w = street_z(n), street_width(n)
        for ax in AVE_X.values():
            for sx, sz in ((1, 1), (-1, -1)):
                x, z = ax + sx * 13.2, z0 + sz * (w / 2 + 4.0)
                if not near(x, z):
                    continue
                if rng.random() < 0.6:
                    M['quiosque'].box((x, 1.45, z), (0.14, 1.45, 0.445))
                    for f in (-1, 1):
                        M['tela'].quad((x + f * 0.142, 0.9, z - 0.36), (x + f * 0.142, 0.9, z + 0.36),
                                       (x + f * 0.142, 2.6, z + 0.36), (x + f * 0.142, 2.6, z - 0.36),
                                       uv=((0, 1), (1, 1), (1, 0), (0, 0)), face=(f, 0, 0))
                    obstacles.append((x, z, 0.6))
                bx = x + sx * 1.6
                M['lixeira'].box((bx, 0.63, z), (0.33, 0.48, 0.3))
                M['painel_solar'].box((bx, 1.16, z), (0.35, 0.05, 0.32))
                obstacles.append((bx, z, 0.5))
    # barreiras azuis de contenção (2,4 m) em fila perto da praça (eventos)
    for k in range(5):
        zc = street_z(rng.randrange(43, 47)) + rng.uniform(-30, 30)
        xc = BWAY_K * zc + rng.choice([-6.0, 6.0])
        if not plaza.contains(Point(xc, zc).buffer(3.0)):
            continue
        for j in range(rng.randrange(3, 7)):
            zz = zc + j * 2.45
            M['barreira'].box((xc, 1.0, zz), (0.04, 0.08, 1.2))
            M['barreira'].box((xc, 0.55, zz), (0.025, 0.4, 1.15))
            for dz in (-1.0, 1.0):
                M['barreira'].box((xc, 0.15, zz + dz), (0.35, 0.03, 0.03))
            obstacles.append((xc, zz, 1.0))
    # abrigos de ônibus nas avenidas: estrutura, teto, vidro e painel de anúncio iluminado numa ponta
    for k in range(6):
        ax = rng.choice(list(AVE_X.values()))
        z = rng.uniform(-200, 200)
        if any(abs(z - street_z(n)) < 20 for n in range(40, 51)) or not near(ax, z):
            continue
        x = ax + rng.choice([-1, 1]) * 11.6
        for dz in (-2.4, 2.4):
            M['aco'].box((x, 1.3, z + dz), (0.05, 1.3, 0.05))
        M['aco'].box((x, 2.62, z), (0.75, 0.04, 2.6))
        M['vidro'].quad((x + 0.6, 0.2, z - 2.4), (x + 0.6, 0.2, z + 2.4), (x + 0.6, 2.4, z + 2.4), (x + 0.6, 2.4, z - 2.4),
                        face=(1, 0, 0))
        M['anuncio_abrigo'].box((x, 1.3, z + 2.45), (0.6, 1.0, 0.04))
        obstacles.append((x, z, 2.4))
    # estações de bicicleta: base, 10 bicicletas azuis (rodas em tubo) e o totem
    for k in range(4):
        n = rng.randrange(41, 48)
        z = street_z(n) + rng.choice([-1, 1]) * (street_width(n) / 2 - 5.3)
        x0 = rng.uniform(40, 200) * rng.choice([-1, 1])
        if not near(x0, z):
            continue
        M['aco'].box((x0 + 6, 0.05, z), (6.5, 0.05, 0.3))
        M['quiosque'].box((x0 - 1.0, 1.0, z), (0.25, 1.0, 0.2))
        for j in range(10):
            bx = x0 + 0.5 + j * 1.2
            for dz in (-0.55, 0.55):
                M['bike'].tube([(bx + 0.33 * math.cos(a) * 0, 0.35 + 0.33 * math.sin(a), z + dz + 0.33 * math.cos(a))
                                for a in np.linspace(0, 2 * math.pi, 13)], 0.025, seg=5)
            M['bike_quadro'].beam((bx, 0.35, z - 0.55), (bx, 0.75, z + 0.2), 0.05, 0.05)
            M['bike_quadro'].beam((bx, 0.75, z + 0.2), (bx, 0.35, z + 0.55), 0.05, 0.05)
            M['bike_quadro'].beam((bx, 0.75, z - 0.3), (bx, 0.78, z + 0.25), 0.05, 0.05)
            M['aco'].beam((bx - 0.25, 0.95, z + 0.5), (bx + 0.25, 0.95, z + 0.5), 0.03, 0.03)
        obstacles.append((x0 + 6, z, 6.5))
    mats = {
        'aco': g.material('mob_aco', color=(0.55, 0.57, 0.6), rough=0.3, metal=1.0),
        'carrinho': g.material('mob_carrinho', color=(0.82, 0.82, 0.84), rough=0.25, metal=1.0),
        'guarda_sol': g.material('mob_guarda_sol', color=(0.9, 0.75, 0.1), rough=0.6, double=True),
        'luz_quente': g.material('mob_luz', color=(1, 1, 1), emissive=(1.0, 0.8, 0.5), strength=40.0),
        'listra_l': g.material('mob_vapor_laranja', color=(0.95, 0.35, 0.03), rough=0.5),
        'listra_b': g.material('mob_vapor_branco', color=(0.9, 0.9, 0.88), rough=0.5),
        'quiosque': g.material('mob_quiosque', color=(0.12, 0.12, 0.13), rough=0.35, metal=0.8),
        'tela': g.material('mob_tela', color=(0.2, 0.4, 0.9), emissive=(0.25, 0.5, 1.0), strength=12.0),
        'lixeira': g.material('mob_lixeira', color=(0.05, 0.25, 0.12), rough=0.5, metal=0.4),
        'painel_solar': g.material('mob_solar', color=(0.05, 0.07, 0.15), rough=0.1, metal=0.5),
        'barreira': g.material('mob_barreira', color=(0.08, 0.22, 0.65), rough=0.45, metal=0.3),
        'vidro': g.material('mob_vidro', color=(0.8, 0.85, 0.9), rough=0.05, metal=0.0, double=True),
        'bike': g.material('mob_pneu', color=(0.02, 0.02, 0.02), rough=0.8),
        'bike_quadro': g.material('mob_bike', color=(0.05, 0.35, 0.8), rough=0.35, metal=0.4),
        'anuncio_abrigo': g.material('mob_anuncio', color=(1, 1, 1), emissive=(1.0, 0.95, 0.85), strength=8.0),
    }
    for k, mesh in M.items():
        g.add_mesh('mob_' + k, mesh, mats[k])


def street_details(g, pbr, rng, road, sidewalk, plaza, near_plaza, obstacles, tex, rel, texdir):
    """Camada de rua: marcações e remendos no asfalto, manchas, árvores com canteiro, sacos de lixo, placas com o
    nome das ruas, caixas de jornal e de correio. Tudo só perto da praça (até ~260 m), onde a câmera anda."""
    near = lambda x, z, r=260: Point(x, z).distance(near_plaza) < r
    paint = g.material('pintura_solo', color=(0.9, 0.9, 0.86), rough=0.55, tex=rel(text_decal(tex('solo_only.png'), 'ONLY')),
                       mask=0.5)
    bus = g.material('pintura_bus', color=(0.9, 0.9, 0.86), rough=0.55,
                     tex=rel(text_decal(tex('solo_bus.png'), 'BUS ONLY')), mask=0.5)
    redlane = g.material('faixa_onibus', color=(0.2, 0.06, 0.045), rough=0.7)  # vermelho-tijolo gasto, não vivo
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


def streak_texture(path, seed, w=128, h=256):
    """Escorrido de sujeira (RGBA, alfa recortado): listras verticais que afinam e somem para baixo, como a água da
    chuva levando a fuligem do peitoril ou da cornija pela parede."""
    rng = np.random.default_rng(seed)
    x = np.arange(w)[None, :]
    y = (np.arange(h)[:, None]) / h
    cols = np.zeros(w)
    for _ in range(rng.integers(6, 14)):
        c = rng.uniform(0, w)
        cols += rng.uniform(0.4, 1.0) * np.exp(-((np.arange(w) - c) / rng.uniform(1.5, 6)) ** 2)
    length = 0.4 + 0.6 * np.convolve(rng.random(w), np.ones(9) / 9, 'same')
    dens = np.clip(cols, 0, 1)[None, :] * np.clip(1 - y / length[None, :], 0, 1) ** 1.5
    dens = dens * (0.7 + 0.3 * np.clip(1 - y * 3, 0, 1)) + 0.25 * np.clip(1 - y * 8, 0, 1)  # mais forte no topo
    a = (dens > rng.random((h, w)) * 0.9 + 0.08)
    v = 25 + 15 * rng.random((h, w))
    rgb = np.stack([v * 1.05, v, v * 0.9], -1)
    Image.fromarray(np.dstack([rgb, np.where(a, 255, 0)]).astype(np.uint8), 'RGBA').save(path)
    return path


def graffiti_texture(path, seed, w=512, h=256):
    """Pichação/tag (RGBA recortado): palavras inventadas em traço grosso com contorno, cores de spray, respingos."""
    pr = random.Random(seed)
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    word = pr.choice(['ZEKO', 'RAVN', 'KRUZ', 'MOTH', 'SKEP', 'ODIN', 'VEXA', 'PLUM', 'JINX', 'TOKA', 'NOVA', 'BRISK'])
    fill = pr.choice([(230, 40, 60), (40, 200, 230), (250, 210, 40), (150, 80, 230), (60, 220, 90), (245, 245, 245)])
    line = pr.choice([(10, 10, 10), (255, 255, 255), (30, 30, 120)])
    f = ImageFont.truetype(pr.choice([FONT, FONT2]), int(h * 0.62))
    tw = d.textlength(word, font=f)
    x0 = (w - tw) / 2
    layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((x0, h * 0.12), word, font=f, fill=fill + (255,), stroke_width=7, stroke_fill=line + (255,))
    layer = layer.rotate(pr.uniform(-12, 12), resample=Image.BICUBIC).transform(
        (w, h), Image.AFFINE, (1, pr.uniform(-0.35, 0.35), 0, 0, 1, 0), resample=Image.BICUBIC)
    img.alpha_composite(layer)
    for _ in range(40):  # respingos e escorridos de tinta
        x, y = pr.uniform(0, w), pr.uniform(0, h)
        r = pr.uniform(1, 3)
        d.ellipse((x - r, y - r, x + r, y + r), fill=fill + (255,))
    a = np.asarray(img).copy()
    a[..., 3] = np.where(a[..., 3] > 100, 255, 0)
    a[np.random.default_rng(seed).random((h, w)) < 0.12, 3] = 0  # tinta falhada
    Image.fromarray(a, 'RGBA').save(path)
    return path


def poster_texture(path, seed, w=256, h=320):
    """Cartaz colado (show, filme, festa) com nomes inventados, já desbotado e rasgado nas bordas."""
    pr = random.Random(seed)
    bg = pr.choice([(230, 60, 40), (20, 20, 30), (250, 220, 60), (40, 90, 200), (240, 240, 230), (200, 40, 140)])
    fg = (255, 255, 255) if sum(bg) < 400 else (20, 20, 25)
    img = Image.new('RGB', (w, h), bg)
    d = ImageDraw.Draw(img)
    for _ in range(pr.randrange(1, 4)):
        r = pr.randrange(30, 120)
        cx, cy = pr.randrange(w), pr.randrange(h // 2, h)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=pr.choice([fg, (255, 140, 0), (0, 200, 200)]))
    words = pr.choice([['LIVE', 'THE MOTHS'], ['TONIGHT', 'DJ KAIRO'], ['NEW ALBUM', 'GLASS OWL'], ['FRI 9PM', 'NEON RIOT'],
                       ['IN THEATERS', 'DEEP HARBOR'], ['SALE', '50% OFF'], ['COMEDY', 'BIG LAUGHS'], ['JAZZ', 'BLUE NOTEZ']])
    for i, wd in enumerate(words):
        f = ImageFont.truetype(FONT, 34 if i == 0 else 44)
        tw = d.textlength(wd, font=f)
        if tw > w - 16:
            f = ImageFont.truetype(FONT, int((34 if i == 0 else 44) * (w - 16) / tw))
            tw = d.textlength(wd, font=f)
        d.text(((w - tw) / 2, 20 + i * 52), wd, font=f, fill=fg)
    a = np.asarray(img).astype(np.float32)
    a = a * 0.75 + 40  # desbotado pelo sol e pela chuva
    alpha = np.full((h, w), 255, np.uint8)
    rng = np.random.default_rng(seed)
    for side in range(4):  # bordas rasgadas
        depth = (rng.random(w if side < 2 else h) ** 3 * 18).astype(int)
        for i, dd in enumerate(depth):
            if side == 0:
                alpha[:dd, i] = 0
            elif side == 1:
                alpha[h - dd:, i] = 0
            elif side == 2:
                alpha[i, :dd] = 0
            else:
                alpha[i, w - dd:] = 0
    Image.fromarray(np.dstack([np.clip(a, 0, 255), alpha]).astype(np.uint8), 'RGBA').save(path)
    return path


def damp_texture(path, seed, w=256):
    """Mancha de umidade/fuligem (RGBA recortado com borda pontilhada), escura e esverdeada."""
    rng = np.random.default_rng(seed)
    low = np.asarray(Image.fromarray((rng.random((8, 8)) * 255).astype(np.uint8)).resize((w, w), Image.BICUBIC)) / 255.0
    yy, xx = np.mgrid[0:w, 0:w] / w - 0.5
    r = np.hypot(xx, yy * 0.8)
    v = low - r * 1.4
    a = v > rng.random((w, w)) * 0.35 - 0.05
    c = 30 + 25 * low
    rgb = np.stack([c * 0.9, c, c * 0.85], -1)
    Image.fromarray(np.dstack([rgb, np.where(a, 255, 0)]).astype(np.uint8), 'RGBA').save(path)
    return path


def plaza_textures(base, texdir, W=1024, metros=2.4):
    """Piso da praça como o da reforma de 2017: placas de concreto pré-moldado em dois tons, em faixas, com discos de
    aço embutidos que brilham com a luz dos telões. Gera cor, normal (das juntas e discos) e ORM (disco = metal liso)."""
    px = W / metros
    yy, xx = np.mgrid[0:W, 0:W] / px  # metros
    pw, ph = 0.6, 0.3
    row = np.floor(yy / ph)
    xo = xx + (row % 2) * pw / 2
    jx = np.minimum(xo % pw, pw - xo % pw)
    jy = np.minimum(yy % ph, ph - yy % ph)
    joint = np.minimum(jx, jy)
    band = (np.floor(yy / (ph * 4)) % 3 == 0)  # faixas escuras
    src = Image.open(os.path.join(texdir, 'concrete_pavement', 'cor.jpg')).convert('L').resize((W, W))
    noise = np.asarray(src, np.float32) / 255.0
    tone = np.where(band, 0.30, 0.52) * (0.8 + 0.4 * noise)
    pid = (np.floor(xo / pw) * 31 + row * 17) % 7 / 7.0  # cada placa um pouco diferente
    tone *= 0.92 + 0.12 * pid
    # discos de aço (~11 cm) espalhados numa grade irregular
    rng = np.random.default_rng(5)
    disc = np.zeros((W, W), bool)
    for _ in range(int(metros * metros * 9)):
        cx, cy = rng.uniform(0, W), rng.uniform(0, W)
        r = 0.055 * px
        x0, x1, y0, y1 = int(cx - r - 1), int(cx + r + 2), int(cy - r - 1), int(cy + r + 2)
        sub = (np.arange(max(0, y0), min(W, y1))[:, None] - cy) ** 2 + (np.arange(max(0, x0), min(W, x1))[None, :] - cx) ** 2
        disc[max(0, y0):min(W, y1), max(0, x0):min(W, x1)] |= sub < r * r
    height = np.clip(joint / 0.006, 0, 1) - 0.2 * disc
    col = np.stack([tone, tone, tone * 1.03], -1)
    col[joint < 0.004] *= 0.45
    col[disc] = (0.55, 0.56, 0.58)
    gy, gx = np.gradient(height * 0.004 * px)
    nrm = np.stack([-gx, gy, np.ones_like(gx)], -1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    orm = np.stack([np.where(joint < 0.004, 0.6, 1.0), np.where(disc, 0.22, 0.62 + 0.1 * noise), disc * 1.0], -1)
    out = []
    for arr, name in ((col, 'cor'), (nrm * 0.5 + 0.5, 'normal'), (orm, 'arm')):
        Image.fromarray(np.clip(arr * 255, 0, 255).astype(np.uint8)).save(f'{base}_{name}.jpg', quality=90)
        out.append(f'{base}_{name}.jpg')
    return out


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
    import interiores  # rbox/colar_y: móveis de rua com forma (quinas boleadas, estofados)
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
    pc, pn, pa = plaza_textures(tex('praca'), texdir)
    granite = g.material('praca', tex=rel(pc), normal_tex=rel(pn), orm_tex=rel(pa), rough=1.0, metal=1.0)
    curb = pbr('meio_fio', 'brushed_concrete', tint=(0.75, 0.74, 0.72))
    paint = g.material('faixa', color=(0.85, 0.85, 0.8), rough=0.5)

    m = Mesh()
    m.polygon(road, 0.0, uv_scale=0.22)
    g.add_mesh('pista', m, asphalt)
    m = Mesh()
    m.polygon(sidewalk, 0.15, uv_scale=0.35)
    g.add_mesh('calcada', m, walk)
    m = Mesh()
    m.polygon(plaza, 0.15, uv_scale=1 / 2.4)
    g.add_mesh('praca', m, granite)
    m = Mesh()  # chão dos quarteirões: fecha a fresta entre a calçada e as fachadas (via-se o céu por baixo)
    m.polygon(blocks, 0.149, uv_scale=0.35)
    g.add_mesh('quarteiroes', m, walk)
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
    fac.lojas_restantes = 70  # quase toda loja perto da praça tem interior 3D e porta de entrar
    from shapely.prepared import prep
    blocks_p = prep(blocks)
    fac.is_street = lambda mid, nrm: not blocks_p.contains(Point(mid[0] + nrm[0] * 2.5, mid[1] + nrm[1] * 2.5))
    WALLS = {'tijolo': ['tijolo0', 'tijolo1', 'tijolo2', 'tijolo3', 'tijolo4'], 'pedra': ['pedra0', 'pedra1', 'pedra2'],
             'reboco': ['reboco0', 'reboco1'], 'concreto': ['concreto0']}
    TRIMS = ['cornija', 'cornija', 'cornija_branca', 'cornija_escura']
    bldg = {k: Mesh() for k in facade_mats}
    roofs = Mesh()
    near_plaza = LineString([(BWAY_K * street_z(41), street_z(41)), (BWAY_K * street_z(48), street_z(48))])
    screens, neons, neon_verticais = [], [], []
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
                         'cunhais': rng.random() < 0.4,
                         'caixilho': rng.choice(['caixilho', 'caixilho', 'caixilho_branco', 'caixilho_verde'])}
                fac.lod = 0 if d < 140 else 1
                fac.loja_ok = d < 120 and rng.random() < 0.85
                fac.bevel = 0.025 if fac.lod == 0 else 0.0
                top = fac.masonry(body, 0.15, podium, wall, st=style)
                fac.bevel = 0.0
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
                    if rng.random() < 0.3 and podium > 14:  # tubo de neon vertical na quina do pódio
                        neon_verticais.append((a[0] + nx * 0.25, 6.5, a[1] + nz * 0.25, podium - 9.0))
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
    for k in range(8):
        fmat[f'grafite{k}'] = g.material(f'grafite{k}', tex=rel(graffiti_texture(tex(f'grafite{k}.png'), 60 + k)),
                                         rough=0.5, mask=0.5)
    for k in range(10):
        fmat[f'cartaz{k}'] = g.material(f'cartaz{k}', tex=rel(poster_texture(tex(f'cartaz{k}.png'), 70 + k)),
                                        rough=0.7, mask=0.5)
    for k in range(3):
        fmat[f'umidade{k}'] = g.material(f'umidade{k}', tex=rel(damp_texture(tex(f'umidade{k}.png'), 80 + k)),
                                         rough=0.8, mask=0.5)
    for k in range(4):
        fmat[f'escorrido{k}'] = g.material(f'escorrido{k}', tex=rel(streak_texture(tex(f'escorrido{k}.png'), 50 + k)),
                                           rough=0.75, mask=0.5)
    brick_src = os.path.join(texdir, 'large_red_bricks', 'cor.jpg')
    for k in range(12):  # anúncios pintados e murais nas empenas
        r = rel(mural_texture(tex(f'mural{k:02d}.png'), rng, brick_src, k))
        fmat[f'mural{k}'] = g.material(f'mural{k}', tex=r, rough=0.85)
    m_v = g.material('vidro_loja', color=(0.95, 0.97, 0.98), rough=0.02, double=True)
    g.doc['materials'][m_v]['extensions'] = {'KHR_materials_transmission': {'transmissionFactor': 0.92},
                                             'KHR_materials_ior': {'ior': 1.5}}
    for ext in ('KHR_materials_transmission', 'KHR_materials_ior'):
        if ext not in g.doc['extensionsUsed']:
            g.doc['extensionsUsed'].append(ext)
    fmat['vidro_loja'] = m_v
    for name, mesh in fac.mesh.items():
        g.add_mesh('fachada_' + name, mesh, fmat[name])
    # Interiores 3D (tools/interiores.py): cada tipo de loja é gravado uma vez e instanciado atrás das vitrines.
    if fac.lojas:
        import interiores
        feitos = {}
        # 68 lojas: se cada painel de luz, letreiro e cardápio virasse luz de malha seriam +230 mil luzes (a RTX de 8
        # GB ficou sem memória). As luzes pontuais de cada loja já iluminam o interior.
        g.sem_luz = True
        for tipo, pos, yaw in fac.lojas:
            if tipo not in feitos:
                feitos[tipo] = getattr(interiores, tipo)(out, texdir, 7 + len(feitos), g=g, pbr=pbr)
            ids, luzes, _ = feitos[tipo]
            g.instance(ids, pos, yaw)
            c, sn = math.cos(yaw), math.sin(yaw)
            for (lx, ly, lz), col, inten, rr in luzes:  # gira (x, z) por yaw em torno de y e soma a posição
                g.light((pos[0] + lx * c + lz * sn, pos[1] + ly, pos[2] - lx * sn + lz * c), col, inten, rr)
        g.sem_luz = False
        print(f'{len(fac.lojas)} lojas com interior 3D:', {k: sum(1 for l in fac.lojas if l[0] == k) for k in feitos})
        for tipo, pos, yaw in fac.lojas:
            print('  ', tipo, [round(v, 1) for v in pos], round(math.degrees(yaw)))
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
    # Neon de verdade: a placa com o letreiro (texto luminoso forte, o bloom faz o halo) e, em volta, um tubo de
    # vidro aceso de cor viva — geometria emissiva que vira luz de malha e se reflete no asfalto molhado, como nos
    # letreiros de Manhattan à noite. Mais tubos verticais nas quinas dos pódios da praça.
    NEON_CORES = [(1.0, 0.12, 0.45), (0.1, 0.85, 1.0), (1.0, 0.55, 0.05), (0.35, 1.0, 0.25), (1.0, 0.15, 0.08),
                  (0.75, 0.25, 1.0)]
    tubos = [Mesh() for _ in NEON_CORES]
    for i, (mid, (nx, nz), t, y, wid, hgt) in enumerate(neons):
        p = neon_texture(tex(f'neon{i:03d}.png'), rng)
        mat = g.material(f'neon{i:03d}', tex=rel(p), emissive=(1, 1, 1), emissive_tex=rel(p), strength=9.0)
        m = Mesh()
        o = np.array([mid[0] + nx * 0.6, 0, mid[1] + nz * 0.6])
        tx = np.array([t[0], 0, t[1]]) * wid / 2
        A, B = o - tx + [0, y, 0], o + tx + [0, y, 0]
        if np.dot(np.cross(B - A, [0, 1, 0]), [nx, 0, nz]) < 0:
            A, B = B, A
        m.quad(A, B, B + [0, hgt, 0], A + [0, hgt, 0], uv=((0, 1), (1, 1), (1, 0), (0, 0)))  # v do glTF: 0 em cima
        g.add_mesh(f'neon{i:03d}', m, mat)
        # moldura de tubo de neon (cantos arredondados), 8 cm à frente da placa
        n3 = np.array([nx, 0.0, nz]) * 0.08
        # contorno simples e robusto: retângulo com cantos chanfrados
        tt = np.array([t[0], 0.0, t[1]])
        P0 = A + n3 + [0, y, 0]
        W = float(np.linalg.norm(B - A))
        sgn = (B - A) / max(W, 1e-6)
        q = lambda u, v: P0 + sgn * u + np.array([0.0, v, 0.0])
        c = 0.15
        contorno = [q(c, -0.1), q(W - c, -0.1), q(W + 0.1, c), q(W + 0.1, hgt - c), q(W - c, hgt + 0.1), q(c, hgt + 0.1),
                    q(-0.1, hgt - c), q(-0.1, c), q(c, -0.1)]
        tubos[i % len(NEON_CORES)].tube(contorno, 0.03, seg=6)
    for bx, by, bz, h in neon_verticais:
        tubos[rng.randrange(len(NEON_CORES))].tube([(bx, by, bz), (bx, by + h, bz)], 0.05, seg=8)
    for k, (cor, mesh) in enumerate(zip(NEON_CORES, tubos)):
        g.add_mesh(f'neon_tubo{k}', mesh, g.material(f'neon_tubo{k}', color=cor, emissive=cor, strength=30.0))

    # Arquibancada vermelha (Duffy Square, na 47th): 27 degraus de vidro com LED vermelho por dentro, a largura
    # afinando de 13,7 m embaixo para 9,75 m em cima (como a real); a face de cima de cada degrau é vidro escuro
    # brilhante e o espelho (a frente) acende vermelho. Corrimãos de aço dos lados.
    zs = street_z(46) - 9 - 16
    xs = BWAY_K * zs
    vidro_d = g.material('degraus_vidro', color=(0.12, 0.01, 0.01), rough=0.04, metal=0.0)
    led = g.material('degraus_led', color=(1, 0.05, 0.04), emissive=(1.0, 0.04, 0.03), strength=3.5)
    aco_d = g.material('degraus_aco', color=(0.6, 0.62, 0.64), rough=0.25, metal=1.0)
    topo, frente, corr = Mesh(), Mesh(), Mesh()
    n_deg, alt, prof = 27, 0.17, 0.42
    for k in range(n_deg):
        hw = (13.7 - (13.7 - 9.75) * k / (n_deg - 1)) / 2
        y0, y1 = 0.15 + alt * k, 0.15 + alt * (k + 1)
        z0, z1 = zs - prof * k, zs - prof * (k + 1)
        topo.quad((xs - hw, y1, z0), (xs + hw, y1, z0), (xs + hw, y1, z1), (xs - hw, y1, z1), face=(0, 1, 0))
        frente.quad((xs - hw, y0, z0), (xs + hw, y0, z0), (xs + hw, y1, z0), (xs - hw, y1, z0), face=(0, 0, 1))
        for sx in (-1, 1):  # laterais fechadas (vidro escuro)
            topo.quad((xs + sx * hw, 0.15, z0), (xs + sx * hw, 0.15, z1), (xs + sx * hw, y1, z1), (xs + sx * hw, y1, z0),
                      face=(sx, 0, 0))
    for sx in (-1, 1):
        hw0, hw1 = 13.7 / 2 + 0.15, 9.75 / 2 + 0.15
        pts = [(xs + sx * (hw0 + (hw1 - hw0) * f), 0.15 + 1.0 + alt * n_deg * f, zs - prof * n_deg * f)
               for f in np.linspace(0, 1, 8)]
        corr.tube(pts, 0.035, seg=8)
        for f in np.linspace(0, 1, 6):
            x = xs + sx * (hw0 + (hw1 - hw0) * f)
            yb = 0.15 + alt * n_deg * f
            corr.cylinder((x, zs - prof * n_deg * f), 0.025, yb, yb + 1.0, seg=8)
    g.add_mesh('arquibancada_topo', topo, vidro_d)
    g.add_mesh('arquibancada_led', frente, led)
    g.add_mesh('arquibancada_corrimao', corr, aco_d)

    # Marquises de teatro (o "Theater District" em volta da 44th/45th, a oeste da praça): caixa saliente sobre a
    # calçada com o nome do teatro e o espetáculo em letreiros acesos dos 3 lados e a moldura de lâmpadas (as
    # "chaser lights"), luz quente embaixo. Nomes e espetáculos fictícios.
    TEATROS = ['THE AURORA', 'HALCYON', 'THE MERIDIAN', 'STARLIGHT', 'THE VELVET', 'CRESCENT', 'BELMONT', 'THE GILDED',
               'SILVER BELL', 'THE CARDINAL', 'EVERGREEN', 'THE ROSEWOOD']
    SHOWS = ['MIDNIGHT ECHO', 'QUEEN OF HEARTS', 'THE LAST WALTZ', 'NEON DREAMS', 'CITY OF STARS', 'BROKEN CLOCK',
             'GOLDEN HOUR', 'THE LONG RAIN', 'PAPER MOON', 'WILD LIGHTS', 'BLUE VELVET', 'HEARTBEAT']

    def placa_teatro(path, nome, show, w=1024, h=256):
        im = Image.new('RGB', (w, h), (12, 10, 14))
        d = ImageDraw.Draw(im)
        cor = random.Random(nome).choice([(255, 210, 120), (255, 240, 220), (255, 120, 150), (140, 220, 255)])
        for txt, y, sz, fnt in ((nome, 0.06, 0.38, FONT2), (show, 0.55, 0.32, FONT)):
            f = ImageFont.truetype(fnt, int(h * sz))
            tw = d.textlength(txt, font=f)
            if tw > w * 0.94:
                f = ImageFont.truetype(fnt, int(h * sz * w * 0.94 / tw))
                tw = d.textlength(txt, font=f)
            d.text(((w - tw) / 2, h * y), txt, font=f, fill=cor)
        im.save(path)
        return path

    caixa_t = g.material('teatro_caixa', color=(0.05, 0.04, 0.035), rough=0.3, metal=0.6)
    lampadas = g.material('teatro_lampadas', color=(1, 0.9, 0.6), emissive=(1.0, 0.82, 0.5), strength=35.0)
    luz_baixo = g.material('teatro_luz', color=(1, 1, 1), emissive=(1.0, 0.88, 0.7), strength=18.0)
    m_caixa, m_lamp, m_luz = Mesh(), Mesh(), Mesh()
    feitos = 0
    for k in range(40):
        if feitos >= 12:
            break
        n = rng.choice([44, 45])
        z, w = street_z(n), street_width(n)
        x = rng.uniform(-225, -30)
        if abs(x - BWAY_K * z) < 30:
            continue
        sz = rng.choice([-1, 1])
        zf = z + sz * (w / 2 + 0.3)  # linha da fachada
        L, D, H, yb = rng.uniform(8, 12), 2.8, 1.6, 4.0
        zc = zf - sz * D / 2
        m_caixa.box((x, yb + H / 2, zc), (L / 2, H / 2, D / 2))
        img_t = rel(placa_teatro(tex(f'teatro{feitos:02d}.png'), TEATROS[feitos], SHOWS[feitos]))
        mat = g.material(f'teatro{feitos}', tex=img_t, emissive=(1, 1, 1), emissive_tex=img_t, strength=6.0)
        pl = Mesh()
        zfr = zf - sz * (D + 0.01)  # face voltada para a rua
        pl.quad((x - L / 2 + 0.2, yb + 0.2, zfr), (x + L / 2 - 0.2, yb + 0.2, zfr), (x + L / 2 - 0.2, yb + H - 0.2, zfr),
                (x - L / 2 + 0.2, yb + H - 0.2, zfr), uv=((0, 1), (1, 1), (1, 0), (0, 0)) if sz < 0 else
                ((1, 1), (0, 1), (0, 0), (1, 0)), face=(0, 0, -sz))
        for sx in (-1, 1):  # laterais
            xx = x + sx * (L / 2 + 0.01)
            pl.quad((xx, yb + 0.2, zf - sz * 0.3), (xx, yb + 0.2, zf - sz * (D - 0.2)), (xx, yb + H - 0.2, zf - sz * (D - 0.2)),
                    (xx, yb + H - 0.2, zf - sz * 0.3), uv=((0, 1), (0.35, 1), (0.35, 0), (0, 0)), face=(sx, 0, 0))
        g.add_mesh(f'teatro_placa{feitos}', pl, mat)
        # moldura de lâmpadas: em cima e embaixo da frente e das laterais
        for yy in (yb + 0.08, yb + H - 0.08):
            for u in np.arange(-L / 2 + 0.1, L / 2, 0.22):
                m_lamp.sphere((x + u, yy, zfr - sz * 0.03), 0.045, 6)
            for sx in (-1, 1):
                for v in np.arange(0.2, D, 0.22):
                    m_lamp.sphere((x + sx * (L / 2 + 0.03), yy, zf - sz * v), 0.045, 6)
        # luz embaixo da marquise (iluminando a entrada)
        m_luz.box((x, yb - 0.01, zc), (L / 2 - 0.3, 0.01, D / 2 - 0.3))
        g.light((x, yb - 0.4, zc), (1.0, 0.85, 0.65), 400.0, 12.0)
        feitos += 1
    g.add_mesh('teatro_caixas', m_caixa, caixa_t)
    g.add_mesh('teatro_lampadas', m_lamp, lampadas)
    g.add_mesh('teatro_luz_baixo', m_luz, luz_baixo)

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
    obstacles.append((xs, zs - prof * n_deg / 2, 6.5))  # a arquibancada vermelha
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
                if rng.random() < 0.5:  # hidrante de NY: flange, corpo, anel, bocais laterais com tampa, castelo
                    H = pr['hidrante']
                    H.frustum((xx, zz), 0.17, 0.15, 0.15, 0.2, seg=16)
                    H.cylinder((xx, zz), 0.115, 0.2, 0.72, seg=16)
                    H.frustum((xx, zz), 0.15, 0.15, 0.62, 0.68, seg=16)
                    H.frustum((xx, zz), 0.13, 0.1, 0.72, 0.84, seg=16)
                    H.frustum((xx, zz), 0.045, 0.025, 0.84, 0.92, seg=8)
                    for dx in (-1, 1):
                        H.tube([(xx + dx * 0.1, 0.52, zz), (xx + dx * 0.2, 0.52, zz)], 0.045, seg=10)
                        H.tube([(xx + dx * 0.2, 0.52, zz), (xx + dx * 0.225, 0.52, zz)], 0.055, seg=6)
                else:  # cesto de lixo de arame: boca alargada, aros e costelas verticais, fundo
                    L = pr['lixeira']
                    L.frustum((xx, zz), 0.25, 0.3, 0.15, 0.17, seg=16)
                    for yy, rr in ((0.18, 0.25), (0.45, 0.27), (0.75, 0.29), (1.02, 0.31)):
                        L.tube([(xx + rr * math.cos(t), yy, zz + rr * math.sin(t)) for t in np.linspace(0, 2 * math.pi, 25)],
                               0.012, seg=5)
                    for t in np.linspace(0, 2 * math.pi, 15)[:-1]:
                        L.tube([(xx + 0.25 * math.cos(t), 0.18, zz + 0.25 * math.sin(t)),
                                (xx + 0.31 * math.cos(t), 1.02, zz + 0.31 * math.sin(t))], 0.006, seg=4)
        for x in (-170.0, 130.0):  # bueiros no asfalto (com vapor na cena .json)
            pr['bueiro'].cylinder((x + rng.uniform(-20, 20), z + rng.uniform(-2, 2)), 0.45, 0.0, 0.012, seg=16)
            steam_points.append((float(x), float(z)))
    # Praça: frades de granito na borda (contra carros), bancos e floreiras.
    pl_edge = plaza.boundary
    for k in np.arange(0, pl_edge.length, 2.4):
        p = pl_edge.interpolate(k)
        if road.distance(p) < 1.0 and rng.random() < 0.9:
            pr['granito'].cylinder((p.x, p.y), 0.22, 0.15, 0.92, seg=16, cap=False)
            pr['granito'].sphere((p.x, 0.92, p.y), 0.22, 16, sy=0.4)
    for zc in np.arange(street_z(46) + 20, street_z(42) - 20, 14.0):
        if any(abs(zc - street_z(n)) < 16 for n in range(42, 48)):
            continue
        xc = BWAY_K * zc
        for side in (-1, 1):
            if not plaza.contains(Point(xc + side * 7.5, zc).buffer(2.0)):  # no cruzamento com a 7th, a praça some
                continue
            if rng.random() < 0.6:  # banco: base de granito boleada, ripas de madeira no assento e no encosto
                b_gran, b_mad = Mesh(), Mesh()
                interiores.rbox(b_gran, (-0.32, 0.15, -1.7), (0.32, 0.42, 1.7), r=0.04)
                for k in range(5):
                    xs = -0.3 + k * 0.125
                    interiores.rbox(b_mad, (xs, 0.42, -1.8), (xs + 0.1, 0.46, 1.8), r=0.012, seg=2)
                for yy in (0.62, 0.76):  # encosto em ripas sobre suportes de ferro
                    interiores.rbox(b_mad, (side * 0.31 - 0.02, yy, -1.8), (side * 0.31 + 0.02, yy + 0.1, 1.8), r=0.01, seg=2)
                for zz in (-1.4, 0.0, 1.4):
                    pr['escuro'].tube([(xc + side * 7.5 + side * 0.31, 0.46, zc + zz), (xc + side * 7.5 + side * 0.33, 0.88, zc + zz)],
                                      0.02, seg=6)
                yaw = -math.atan(BWAY_K)
                interiores.colar_y(pr['granito'], b_gran, yaw, (xc + side * 7.5, 0.0, zc))
                interiores.colar_y(pr['banco'], b_mad, yaw, (xc + side * 7.5, 0.0, zc))
                obstacles.append((xc + side * 7.5, zc, 2.1))
            else:  # floreira de granito boleada, terra e um arbusto
                fl = Mesh()
                interiores.rbox(fl, (-0.9, 0.15, -0.9), (0.9, 0.95, 0.9), r=0.06)
                interiores.colar_y(pr['granito'], fl, 0.0, (xc + side * 7.0, 0.0, zc))
                pr['planta'].sphere((xc + side * 7.0, 1.3, zc), 0.8, 14, sy=0.8)
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
        bc = Mesh()
        interiores.rbox(bc, (-1.2, 0.15, -2.2), (1.2, 2.65, 2.2), r=0.05)
        interiores.rbox(bc, (-1.3, 2.74, -2.5), (1.9, 2.86, 2.5), r=0.04)  # toldo
        interiores.colar_y(pr['banca'], bc, 0.0, (x, 0.0, z))
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
    mobiliario(g, pbr, rng, plaza, sidewalk, near_plaza, obstacles, steam_points, tex, rel)
    with open(os.path.join(out, 'vapor.json'), 'w') as f:  # de novo: carrinhos e chaminés soltam vapor
        json.dump(steam_points, f)
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
                        os.path.join(a.saida, 'times_square.gltf'), '-cc', '-kn', '-km', '-ke',
                        # posições em ponto flutuante: quantizadas, cada malha (do tamanho da cidade) cai numa grade de
                        # 6 cm e os decalques (letreiros a 2 cm da placa, pintura a 1,2 cm do asfalto) colam na
                        # superfície de trás e piscam (z-fighting)
                        '-vpf'], check=True)
        for extra in ('vapor.json', 'obstaculos.json'):
            shutil.copyfile(os.path.join(tmp, extra), os.path.join(a.saida, extra))
        # O gltfpack copia as texturas para o lado do .gltf (e aponta para lá): a pasta tex/ antiga sobra.
        shutil.rmtree(os.path.join(a.saida, 'tex'), ignore_errors=True)


if __name__ == '__main__':
    main()
