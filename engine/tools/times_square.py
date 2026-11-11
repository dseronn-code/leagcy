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
from shapely.geometry import LineString, MultiPolygon, Polygon, box
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

    def quad(self, a, b, c, d, uv=((0, 0), (1, 0), (1, 1), (0, 1))):
        """Quadrilátero a-b-c-d (anti-horário visto de fora)."""
        a, b, c, d = (np.asarray(x, np.float64) for x in (a, b, c, d))
        nrm = np.cross(b - a, d - a)
        nrm /= max(np.linalg.norm(nrm), 1e-9)
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
            bldg[(st, k)].extrude_walls(body, 0.15, podium, uv_w=1 / 12.0, uv_h=1 / 14.0)
            top = podium
            if tower > podium:
                cl = polys(body.buffer(-rng.uniform(3, 8)))
                core = max(cl, key=lambda q: q.area) if cl else None
                if core is not None and core.area > 80:
                    st2 = rng.choice(['vidro', 'vidro', 'concreto', 'pedra'])
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

    # Torre de telões empilhados na ponta sul da praça (estilizada; não é cópia do prédio real).
    zt = street_z(42) - 15 - 22
    xt = BWAY_K * zt + 10
    m = Mesh()
    m.box((xt, 55, zt), (9, 55, 14))
    g.add_mesh('torre_sul', m, facade_mats[('concreto', 0)])
    for i, y in enumerate(range(18, 104, 14)):
        screens.append((np.array([xt, zt + 14.3]), (0.0, 1.0), np.array([1.0, 0.0]), y, 16, 11))

    # Telões: cada um um material com textura própria (anúncio fictício), emissivo forte (LED: ~6.000 nits).
    for i, (mid, (nx, nz), t, y, wid, hgt) in enumerate(screens):
        p = ad_texture(tex(f'tel{i:03d}.png'), rng, 512, max(128, int(512 * hgt / max(wid, 1)) // 32 * 32))
        mat = g.material(f'telao{i:03d}', rough=0.35, tex=rel(p), emissive=(1, 1, 1), emissive_tex=rel(p),
                         strength=6.0)
        m = Mesh()
        o = np.array([mid[0] + nx * 0.4, 0, mid[1] + nz * 0.4])
        tx = np.array([t[0], 0, t[1]]) * wid / 2
        A = o - tx + [0, y, 0]
        B = o + tx + [0, y, 0]
        if np.dot(np.cross(B - A, [0, 1, 0]), [nx, 0, nz]) < 0:  # frente para fora da fachada
            A, B = B, A
        m.quad(A, B, B + [0, hgt, 0], A + [0, hgt, 0])
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
        m.quad(A, B, B + [0, hgt, 0], A + [0, hgt, 0])
        g.add_mesh(f'neon{i:03d}', m, mat)

    # Arquibancada vermelha iluminada (Duffy Square, ao norte da 46th, no meio da gravata).
    zs = street_z(46) - 9 - 18
    xs = BWAY_K * zs
    red = g.material('degraus', color=(0.7, 0.05, 0.05), rough=0.25, emissive=(1.0, 0.08, 0.06), strength=1.5)
    m = Mesh()
    for k in range(14):
        m.box((xs, 0.15 + 0.2 * k + 0.1, zs - 0.45 * k), (7.5, 0.1 + 0.2 * k / 2 + 0.0, 0.45))
    g.add_mesh('arquibancada', m, red)

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
    a = ap.parse_args()
    build(a.saida, a.semente)


if __name__ == '__main__':
    main()
