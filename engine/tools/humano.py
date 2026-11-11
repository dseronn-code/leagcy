#!/usr/bin/env python3
"""Gerador de pessoas (glTF com esqueleto e pesos) a partir dos dados CC0 do MakeHuman. Uso (de engine/):

    python3 -I tools/humano.py --mpfb <mpfb2/src/mpfb/data> --assets <makehuman_system_assets> \\
        --saida pasta/nome.gltf [--semente 7] [--perfil perfil.json]

Lê os arquivos (não roda código de terceiros): a malha base hm08, os alvos de forma (.target.gz), o esqueleto
"game_engine" (53 ossos, com pesos), e os acessórios ajustados ao corpo (.mhclo: olhos, sobrancelhas, cílios, cabelo,
roupa, sapatos). Tudo do MakeHuman é CC0 (LICENSE.ASSETS.md do MPFB2).

  - Corpo: os macros do MakeHuman (gênero, idade, músculo, peso, proporções, altura, busto, etnia) com a mesma
    interpolação do MPFB (TargetService.calculate_target_stack_from_macro_info_dict), mais alvos de detalhe do rosto
    e do corpo sorteados pela semente: cada semente dá um rosto diferente.
  - Esqueleto: a cabeça de cada osso é o centro do cubo de junta ("joint-*") depois dos alvos; rotação de repouso
    identidade (o retargeting do motor trabalha com a diferença de rotação no espaço do modelo).
  - Acessórios (.mhclo): cada vértice é uma combinação de 3 vértices do corpo mais um deslocamento escalado pelo corpo
    (x_scale/y_scale/z_scale); os pesos vêm do corpo pela mesma combinação. "delete_verts" esconde o corpo sob a roupa.
  - Unidades: decímetros do MakeHuman -> metros; pés no chão (y = 0), de frente para +Z.

O perfil (JSON) fixa o que a semente sortearia, por exemplo:
    {"macros": {"gender": 1.0, "age": 0.5, "muscle": 0.6, "weight": 0.5, "height": 0.6,
                "race": {"african": 0.2, "caucasian": 0.5, "asian": 0.3}},
     "detalhes": {"nose/nose-scale-horiz-incr": 0.3}, "pele": "young_caucasian_male", "cabelo": "short02",
     "roupa": ["male_casualsuit03", "shoes02"], "sobrancelha": "eyebrow003", "olhos": "brown"}
"""
import argparse
import gzip
import json
import math
import os
import random
import re
import shutil
import struct

import numpy as np

UNIT = 0.1  # decímetro -> metro
_target_cache = {}


# ---------------------------------------------------------------------------------------------------------- leitura
def load_obj(path):
    """Vértices, UVs e faces (listas de índices de vértice e de UV) de um .obj; faces viram triângulos depois."""
    v, vt, faces = [], [], []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('v '):
                v.append([float(x) for x in line.split()[1:4]])
            elif line.startswith('vt '):
                vt.append([float(x) for x in line.split()[1:3]])
            elif line.startswith('f '):
                vi, ti = [], []
                for tok in line.split()[1:]:
                    p = tok.split('/')
                    vi.append(int(p[0]) - 1)
                    ti.append(int(p[1]) - 1 if len(p) > 1 and p[1] else -1)
                faces.append((vi, ti))
    return np.array(v, np.float64), np.array(vt, np.float64) if vt else np.zeros((0, 2)), faces


def load_target(path):
    if path not in _target_cache:
        idx, d = [], []
        with gzip.open(path, 'rt') as f:
            for line in f:
                p = line.split()
                if len(p) == 4 and not line.startswith('#'):
                    idx.append(int(p[0]))
                    d.append([float(p[1]), float(p[2]), float(p[3])])
        _target_cache[path] = (np.array(idx, np.int64), np.array(d, np.float64).reshape(-1, 3))
    return _target_cache[path]


def ranges(groups, name):
    out = []
    for a, b in groups[name]:
        out.extend(range(a, b + 1))
    return np.array(out, np.int64)


def parse_mhmat(path):
    m = {}
    if not os.path.exists(path):
        return m
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.strip().split(None, 1)
            if len(p) == 2 and not p[0].startswith('#'):
                m[p[0]] = p[1].strip()
    return m


def parse_mhclo(path):
    """Cabeçalho, ligações dos vértices (3 índices, 3 pesos, deslocamento) e vértices do corpo a esconder."""
    hdr, refs, delete = {}, [], []
    mode = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            p = line.split()
            if p[0] == 'verts':
                mode = 'verts'
                continue
            if p[0] == 'delete_verts':
                mode = 'delete'
                continue
            if mode == 'verts' and re.match(r'^-?\d', p[0]):
                if len(p) == 1:
                    refs.append(((int(p[0]), 0, 0), (1.0, 0.0, 0.0), (0.0, 0.0, 0.0)))
                else:
                    refs.append(((int(p[0]), int(p[1]), int(p[2])), (float(p[3]), float(p[4]), float(p[5])),
                                 (float(p[6]), float(p[7]), float(p[8]))))
                continue
            if mode == 'delete' and re.match(r'^\d', p[0]):
                toks = line.replace('-', ' - ').split()
                i = 0
                while i < len(toks):
                    if i + 2 < len(toks) and toks[i + 1] == '-':
                        delete.extend(range(int(toks[i]), int(toks[i + 2]) + 1))
                        i += 3
                    else:
                        delete.append(int(toks[i]))
                        i += 1
                continue
            if p[0] in ('x_scale', 'y_scale', 'z_scale'):
                hdr[p[0]] = (int(p[1]), int(p[2]), float(p[3]))
            else:
                hdr[p[0]] = ' '.join(p[1:])
    return hdr, refs, delete


# ---------------------------------------------------------------------------------------------------------- macros
def interpolate(macro_cfg, name, value):
    comps = []
    for part in macro_cfg[name]['parts']:
        lo, hi = part['lowest'], part['highest']
        if lo < value < hi:
            t = (value - lo) / (hi - lo)
            if part['low']:
                comps.append((part['low'], 1.0 - t))
            if part['high']:
                comps.append((part['high'], t))
    return comps


def macro_stack(macro_cfg, info, cutoff=0.01):
    """Os alvos macro e seus pesos, como o MPFB."""
    c = {k: interpolate(macro_cfg, k, info[k]) for k in
         ('gender', 'age', 'muscle', 'weight', 'proportions', 'height', 'cupsize', 'firmness')}
    out = []
    for race, rw in info['race'].items():
        if rw > 1e-4:
            for a in c['age']:
                for g in c['gender']:
                    w = rw * g[1] * a[1]
                    if w > cutoff:
                        out.append((f'macrodetails/{race}-{g[0]}-{a[0]}', w))
    for g in c['gender']:
        for a in c['age']:
            for mu in c['muscle']:
                for we in c['weight']:
                    w = g[1] * a[1] * mu[1] * we[1]
                    if w > cutoff:
                        out.append((f'macrodetails/universal-{g[0]}-{a[0]}-{mu[0]}-{we[0]}', w))
                    for h in c['height']:
                        wh = w * h[1]
                        if wh > cutoff:
                            out.append((f'macrodetails/height/{g[0]}-{a[0]}-{mu[0]}-{we[0]}-{h[0]}', wh))
                    for pr in c['proportions']:
                        wp = w * pr[1]
                        if wp > cutoff and a[0] != 'baby':
                            out.append((f'macrodetails/proportions/{g[0]}-{a[0]}-{mu[0]}-{we[0]}-{pr[0]}', wp))
                    if g[0] == 'female':
                        for cu in c['cupsize']:
                            for fi in c['firmness']:
                                wb = w * cu[1] * fi[1]
                                n = f'breast/female-{a[0]}-{mu[0]}-{we[0]}-{cu[0]}-{fi[0]}'
                                if wb > cutoff and 'averagecup-averagefirmness' not in n and a[0] != 'baby':
                                    out.append((n, wb))
    return out


# Alvos de detalhe sorteados (pares -decr/-incr ou -in/-out etc.): o que faz cada rosto ser outro.
DETAIL_GROUPS = ['head', 'nose', 'mouth', 'chin', 'cheek', 'eyebrows', 'eyes', 'ears', 'forehead', 'neck']


def detail_pairs(data):
    """Pares (alvo-, alvo+) de detalhe do rosto: um valor em [-1, 1] escolhe um lado."""
    pairs = []
    for grp in DETAIL_GROUPS:
        d = os.path.join(data, 'targets', grp)
        if not os.path.isdir(d):
            continue
        names = sorted(n[:-10] for n in os.listdir(d) if n.endswith('.target.gz'))
        used = set()
        for n in names:
            for a, b in (('-decr', '-incr'), ('-in', '-out'), ('-down', '-up'), ('-backward', '-forward'),
                         ('-compress', '-uncompress'), ('-concave', '-convex'), ('-less', '-more')):
                if n.endswith(b) and n[:-len(b)] + a in names:
                    base = n[:-len(b)]
                    if base not in used:
                        used.add(base)
                        pairs.append((f'{grp}/{base}{a}', f'{grp}/{base}{b}'))
    return pairs


# ---------------------------------------------------------------------------------------------------------- corpo
class Body:
    def __init__(self, data):
        self.data = data
        self.v0, self.vt, self.faces = load_obj(os.path.join(data, '3dobjs', 'base.obj'))
        self.groups = json.load(open(os.path.join(data, 'mesh_metadata', 'basemesh_vertex_groups.json')))
        self.macro_cfg = json.load(open(os.path.join(data, 'targets', 'macrodetails', 'macro.json')))['macrotargets']
        self.body_verts = set(ranges(self.groups, 'body').tolist())

    def shape(self, info, details):
        v = self.v0.copy()
        for name, w in macro_stack(self.macro_cfg, info) + list(details.items()):
            p = os.path.join(self.data, 'targets', name + '.target.gz')
            if os.path.exists(p) and abs(w) > 1e-4:
                idx, d = load_target(p)
                v[idx] += d * w
        return v

    def joint(self, v, cube):
        return v[ranges(self.groups, cube)].mean(axis=0)


def triangulate(faces, keep=None):
    tris = []
    for vi, ti in faces:
        if keep is not None and not all(x in keep for x in vi):
            continue
        for k in range(1, len(vi) - 1):
            tris.append(((vi[0], vi[k], vi[k + 1]), (ti[0], ti[k], ti[k + 1])))
    return tris


def smooth_normals(pos, tri_v):
    n = np.zeros_like(pos)
    a, b, c = pos[tri_v[:, 0]], pos[tri_v[:, 1]], pos[tri_v[:, 2]]
    fn = np.cross(b - a, c - a)
    for k in range(3):
        np.add.at(n, tri_v[:, k], fn)
    l = np.linalg.norm(n, axis=1, keepdims=True)
    return n / np.maximum(l, 1e-12)


def top4(weight_rows):
    """Lista de dicionários {osso: peso} -> (índices 4, pesos 4) normalizados."""
    J = np.zeros((len(weight_rows), 4), np.uint16)
    W = np.zeros((len(weight_rows), 4), np.float32)
    for i, row in enumerate(weight_rows):
        items = sorted(row.items(), key=lambda kv: -kv[1])[:4]
        s = sum(w for _, w in items)
        if s <= 0:
            items, s = [(0, 1.0)], 1.0
        for k, (j, w) in enumerate(items):
            J[i, k] = j
            W[i, k] = w / s
    return J, W


# ---------------------------------------------------------------------------------------------------------- glTF
class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.doc = {'asset': {'version': '2.0', 'generator': 'legacy tools/humano.py (MakeHuman CC0)'},
                    'buffers': [], 'bufferViews': [], 'accessors': [], 'meshes': [], 'nodes': [], 'materials': [],
                    'textures': [], 'images': [], 'samplers': [{'magFilter': 9729, 'minFilter': 9987}], 'skins': [],
                    'scenes': [{'nodes': []}], 'scene': 0}
        self.tex_index = {}

    def view(self, arr, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        self.bin += arr.tobytes()
        bv = {'buffer': 0, 'byteOffset': off, 'byteLength': arr.nbytes}
        if target:
            bv['target'] = target
        self.doc['bufferViews'].append(bv)
        return len(self.doc['bufferViews']) - 1

    def accessor(self, arr, gltf_type, comp, target=None, minmax=False, normalized=False):
        a = {'bufferView': self.view(np.ascontiguousarray(arr), target), 'componentType': comp,
             'count': int(arr.shape[0]), 'type': gltf_type}
        if normalized:
            a['normalized'] = True
        if minmax:
            a['min'] = [float(x) for x in arr.min(axis=0)]
            a['max'] = [float(x) for x in arr.max(axis=0)]
        self.doc['accessors'].append(a)
        return len(self.doc['accessors']) - 1

    def texture(self, uri):
        if uri not in self.tex_index:
            self.doc['images'].append({'uri': uri})
            self.doc['textures'].append({'source': len(self.doc['images']) - 1, 'sampler': 0})
            self.tex_index[uri] = len(self.doc['textures']) - 1
        return self.tex_index[uri]

    def material(self, m):
        self.doc['materials'].append(m)
        return len(self.doc['materials']) - 1

    def write(self, path):
        base = os.path.splitext(os.path.basename(path))[0]
        self.doc['buffers'] = [{'uri': base + '.bin', 'byteLength': len(self.bin)}]
        with open(os.path.join(os.path.dirname(path) or '.', base + '.bin'), 'wb') as f:
            f.write(self.bin)
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(self.doc, f, separators=(',', ':'))


# ---------------------------------------------------------------------------------------------------------- pessoa
def choose(rng, opts):
    return opts[rng.randrange(len(opts))]


def build(args):
    data, assets = args.mpfb, args.assets
    rng = random.Random(args.semente)
    perfil = json.load(open(args.perfil)) if args.perfil else {}
    body = Body(data)

    # --- forma
    male = rng.random() < 0.5
    info = {'gender': 1.0 if male else 0.0, 'age': rng.uniform(0.5, 0.85) if rng.random() < 0.8 else rng.uniform(0.4, 1.0),
            'muscle': min(max(rng.gauss(0.5, 0.15), 0.1), 0.95), 'weight': min(max(rng.gauss(0.5, 0.17), 0.1), 0.95),
            'proportions': rng.uniform(0.5, 1.0), 'height': min(max(rng.gauss(0.5, 0.18), 0.05), 0.95),
            'cupsize': rng.uniform(0.3, 0.8), 'firmness': rng.uniform(0.3, 0.8)}
    r = [rng.random() ** 2 for _ in range(3)]
    s = sum(r)
    info['race'] = dict(zip(('african', 'asian', 'caucasian'), (x / s for x in r)))
    info.update(perfil.get('macros', {}))
    male = info['gender'] >= 0.5
    details = {}
    for neg, pos in detail_pairs(data):
        if rng.random() < 0.6:
            x = max(-1.0, min(1.0, rng.gauss(0.0, 0.35)))
            details[pos if x > 0 else neg] = abs(x)
    details.update(perfil.get('detalhes', {}))
    v = body.shape(info, details)

    # --- esqueleto (game_engine)
    rig = json.load(open(os.path.join(data, 'rigs', 'standard', 'rig.game_engine.json')))
    weights = json.load(open(os.path.join(data, 'rigs', 'standard', 'weights.game_engine.json')))['weights']
    names = list(rig)
    order, seen = [], set()

    def visit(n):
        if n in seen:
            return
        p = rig[n]['parent']
        if p:
            visit(p)
        seen.add(n)
        order.append(n)
    for n in names:
        visit(n)
    bone_index = {n: i for i, n in enumerate(order)}
    heads = {n: body.joint(v, rig[n]['head']['cube_name']) for n in order}

    vw = [dict() for _ in range(len(v))]
    for bname, lst in weights.items():
        if bname in bone_index:
            j = bone_index[bname]
            for vi, w in lst:
                vw[vi][j] = vw[vi].get(j, 0.0) + w

    # --- chão e escala
    floor = v[list(body.body_verts)][:, 1].min()
    to_m = lambda p: (p - np.array([0.0, floor, 0.0])) * UNIT

    g = Gltf()
    texdir = os.path.join(os.path.dirname(args.saida) or '.', 'tex')
    os.makedirs(texdir, exist_ok=True)

    def tex_uri(src):
        if not src or not os.path.exists(src):
            return None
        name = os.path.basename(src)
        if not args.tex_max:
            dst = os.path.join(texdir, name)
            if not os.path.exists(dst):
                shutil.copyfile(src, dst)
            return 'tex/' + name
        # Multidão: textura reduzida (no máximo tex_max px); sem transparência vira JPEG (~10x menor).
        from PIL import Image
        im = Image.open(src)
        alpha = im.mode in ('RGBA', 'LA') and im.getchannel('A').getextrema()[0] < 250
        name = os.path.splitext(name)[0] + ('.png' if alpha else '.jpg')
        dst = os.path.join(texdir, name)
        if not os.path.exists(dst):
            im = im.convert('RGBA' if alpha else 'RGB')
            if max(im.size) > args.tex_max:
                k = args.tex_max / max(im.size)
                im = im.resize((max(1, int(im.width * k)), max(1, int(im.height * k))), Image.LANCZOS)
            im.save(dst, quality=88) if not alpha else im.save(dst)
        return 'tex/' + name

    prims = []

    def add_prim(pos_dm, uv, tris, vweights, mat):
        """Triângulos de uma peça: vértice glTF = par (posição, UV)."""
        if not tris:
            return
        key, vidx, tidx, out_tris = {}, [], [], []
        for tv, tt in tris:
            t = []
            for a, b in zip(tv, tt):
                k = (a, b)
                if k not in key:
                    key[k] = len(vidx)
                    vidx.append(a)
                    tidx.append(b)
                t.append(key[k])
            out_tris.append(t)
        vidx, tidx = np.array(vidx), np.array(tidx)
        tri = np.array(out_tris, np.uint32)
        P = to_m(pos_dm)[vidx].astype(np.float32)
        # Normais suaves pela posição (as costuras de UV não quebram a normal).
        all_tri_pos = np.array([tv for tv, _ in tris], np.int64)
        N = smooth_normals(to_m(pos_dm), all_tri_pos)[vidx].astype(np.float32)
        T = np.zeros((len(vidx), 2), np.float32)
        ok = tidx >= 0
        T[ok] = uv[tidx[ok]]
        T[:, 1] = 1.0 - T[:, 1]
        J, W = top4([vweights[i] for i in vidx])
        prims.append({'attributes': {'POSITION': g.accessor(P, 'VEC3', 5126, 34962, minmax=True),
                                     'NORMAL': g.accessor(N, 'VEC3', 5126, 34962),
                                     'TEXCOORD_0': g.accessor(T, 'VEC2', 5126, 34962),
                                     'JOINTS_0': g.accessor(J, 'VEC4', 5123, 34962),
                                     'WEIGHTS_0': g.accessor(W, 'VEC4', 5126, 34962)},
                      'indices': g.accessor(tri.reshape(-1), 'SCALAR', 5125, 34963), 'material': mat})

    def mat_from(mhmat_path, name, alpha=False, extras=None, rough=0.6, clearcoat=None):
        mm = parse_mhmat(mhmat_path)
        folder = os.path.dirname(mhmat_path)
        m = {'name': name, 'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': rough}}
        d = tex_uri(os.path.join(folder, mm['diffuseTexture'])) if 'diffuseTexture' in mm else None
        if d:
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': g.texture(d)}
        n = tex_uri(os.path.join(folder, mm['normalmapTexture'])) if 'normalmapTexture' in mm else None
        if n:
            m['normalTexture'] = {'index': g.texture(n)}
        if alpha:
            m['alphaMode'] = 'MASK'
            m['alphaCutoff'] = 0.35
            m['doubleSided'] = True
        if extras:
            m['extras'] = extras
        if clearcoat:
            m['extensions'] = {'KHR_materials_clearcoat': {'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': clearcoat}}
            g.doc.setdefault('extensionsUsed', []).append('KHR_materials_clearcoat')
        return g.material(m)

    # --- acessórios (ajustados à forma) e o corpo escondido sob a roupa
    def fit(mhclo):
        hdr, refs, delete = parse_mhclo(mhclo)
        scale = np.ones(3)
        for k, ax in (('x_scale', 0), ('y_scale', 1), ('z_scale', 2)):
            if k in hdr:
                a, b, ref = hdr[k]
                scale[ax] = abs(v[a][ax] - v[b][ax]) / ref if ref else 1.0
        ids = np.array([r[0] for r in refs])
        ws = np.array([r[1] for r in refs])
        off = np.array([r[2] for r in refs])
        pos = (v[ids] * ws[:, :, None]).sum(axis=1) + off * scale
        pw = []
        for (i3, w3, _) in refs:
            d = {}
            for vi, wi in zip(i3, w3):
                if wi:
                    for j, wj in vw[vi].items():
                        d[j] = d.get(j, 0.0) + wi * wj
            pw.append(d)
        return hdr, pos, pw, set(delete)

    pieces = []
    hidden = set()
    assets_dir = lambda *p: os.path.join(assets, *p)

    def piece(kind, name, alpha=False, extras=None, rough=0.6, clearcoat=None, mat_override=None):
        folder = assets_dir(kind, name)
        cl = [f for f in os.listdir(folder) if f.endswith('.mhclo')] if os.path.isdir(folder) else []
        if not cl:
            return
        hdr, pos, pw, dele = fit(os.path.join(folder, cl[0]))
        pv, puv, pf = load_obj(os.path.join(folder, hdr.get('obj_file', name + '.obj')))
        mpath = mat_override or os.path.join(folder, hdr.get('material', name + '.mhmat'))
        mat = mat_from(mpath, f'{kind}:{name}', alpha, extras, rough, clearcoat)
        hidden.update(dele)
        pieces.append((pos, puv, triangulate(pf), pw, mat))

    skins = sorted(os.listdir(assets_dir('skins')))
    skin_pref = ('young' if info['age'] < 0.75 else 'old') + '_' + (
        'african' if info['race']['african'] > 0.45 else 'asian' if info['race']['asian'] > 0.45 else 'caucasian') + \
        '_' + ('male' if male else 'female')
    cand = [s for s in skins if s.startswith(skin_pref)] or [s for s in skins if ('_male' if male else '_female') in s]
    skin = perfil.get('pele', choose(rng, cand))
    skin_folder = assets_dir('skins', skin)
    skin_mat = mat_from(os.path.join(skin_folder, skin + '.mhmat'), 'pele:' + skin, rough=0.45,
                        extras={'legacy_pele': [0.004, 0.0015, 0.0007]})

    eye_mats = sorted(f for f in os.listdir(assets_dir('eyes', 'materials')) if f.endswith('.mhmat'))
    eye = perfil.get('olhos')
    eye_mat = os.path.join(assets_dir('eyes', 'materials'), (eye + '.mhmat') if eye else choose(rng, eye_mats))
    piece('eyes', 'high-poly', rough=0.1, clearcoat=0.02, mat_override=eye_mat)
    piece('eyebrows', perfil.get('sobrancelha', choose(rng, sorted(os.listdir(assets_dir('eyebrows'))))), alpha=True,
          rough=0.7)
    piece('eyelashes', perfil.get('cilios', choose(rng, sorted(os.listdir(assets_dir('eyelashes'))))), alpha=True,
          rough=0.7)
    hairs = sorted(os.listdir(assets_dir('hair')))
    male_hair = [h for h in hairs if h.startswith('short') or h == 'afro01']
    hair = perfil.get('cabelo', choose(rng, male_hair if male else hairs))
    piece('hair', hair, alpha=True, rough=0.5)
    clothes = sorted(os.listdir(assets_dir('clothes')))
    suits = [c for c in clothes if c.startswith('male_' if male else 'female_')]
    shoes = [c for c in clothes if c.startswith('shoes')]
    for c in perfil.get('roupa', [choose(rng, suits), choose(rng, shoes)]):
        piece('clothes', c, rough=0.75)

    # --- corpo (sem os vértices de ajuda e sem o que a roupa cobre)
    keep = body.body_verts - hidden
    add_prim(v, body.vt, triangulate(body.faces, keep), vw, skin_mat)
    for pos, puv, tris, pw, mat in pieces:
        add_prim(pos, puv, tris, pw, mat)

    # --- nós do esqueleto e a pele (skin)
    first = len(g.doc['nodes'])
    for n in order:
        p = rig[n]['parent']
        rel = to_m(heads[n]) - (to_m(heads[p]) if p else 0.0)
        g.doc['nodes'].append({'name': n, 'translation': [float(x) for x in rel]})
    for n in order:
        kids = [first + bone_index[c] for c in order if rig[c]['parent'] == n]
        if kids:
            g.doc['nodes'][first + bone_index[n]]['children'] = kids
    ibm = np.zeros((len(order), 16), np.float32)
    for i, n in enumerate(order):
        m = np.eye(4, dtype=np.float32)
        m[:3, 3] = -to_m(heads[n])
        ibm[i] = m.T.reshape(-1)  # coluna por coluna
    g.doc['skins'].append({'joints': [first + i for i in range(len(order))],
                           'inverseBindMatrices': g.accessor(ibm, 'MAT4', 5126),
                           'skeleton': first + bone_index[order[0]]})
    g.doc['meshes'].append({'name': 'pessoa', 'primitives': prims})
    g.doc['nodes'].append({'name': 'pessoa', 'mesh': 0, 'skin': 0})
    g.doc['scenes'][0]['nodes'] = [first + bone_index[order[0]], len(g.doc['nodes']) - 1]
    g.doc['extras'] = {'legacy_pessoa': {'semente': args.semente, 'macros': info, 'pele': skin, 'cabelo': hair,
                                         'altura_m': float(to_m(v[list(keep)])[:, 1].max())}}
    g.write(args.saida)
    return g.doc['extras']['legacy_pessoa']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mpfb', required=True, help='pasta data do MPFB2 (src/mpfb/data)')
    ap.add_argument('--assets', required=True, help='pasta do makehuman_system_assets (clothes, hair, skins...)')
    ap.add_argument('--saida', required=True)
    ap.add_argument('--semente', type=int, default=1)
    ap.add_argument('--perfil')
    ap.add_argument('--tex-max', type=int, default=0, help='reduz as texturas a no máximo N px (multidão)')
    a = ap.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(a.saida)), exist_ok=True)
    info = build(a)
    print(json.dumps(info, ensure_ascii=False))


if __name__ == '__main__':
    main()
