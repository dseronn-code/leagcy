#!/usr/bin/env python3
"""Gera carros estilizados (glTF, um arquivo por modelo) para o trânsito da Times Square. Uso (de engine/):

    python3 tools/carros.py [--saida cenas/times_square/carros]

Cada carro sai do perfil lateral (silhueta) extrudado na largura, com as quinas chanfradas na planta: carroceria
pintada (verniz, KHR_materials_clearcoat), vidros escuros com reflexo, rodas (pneu e calota), faróis e lanternas
emissivos (luzes de malha à noite), placa. Modelos: sedã, SUV, van, hatch e os táxis (sedã e SUV amarelos com o
luminoso no teto). Medidas reais (m): sedã 4,8 x 1,85 x 1,45; SUV 4,9 x 1,95 x 1,75; van 5,3 x 2,0 x 2,1.
Frente para +z, chão em y = 0. Formas genéricas: nenhum carro de marca.
"""
import argparse
import json
import math
import os

import numpy as np

# Perfil lateral (z, y) de trás para a frente, por cima: a linha do teto/capô/porta-malas; embaixo é reto na altura
# do assoalho. A janela é a parte do perfil acima da linha de cintura (belt).
MODELS = {
    'seda': dict(L=4.8, W=1.85, H=1.45, belt=0.92, floor=0.28, wheel=0.33,
                 top=[(-2.4, 0.95), (-2.3, 1.02), (-1.55, 1.05), (-1.05, 1.42), (0.35, 1.45), (0.95, 1.05),
                      (2.25, 0.92), (2.4, 0.72)]),
    'suv': dict(L=4.9, W=1.95, H=1.78, belt=1.08, floor=0.38, wheel=0.37,
                top=[(-2.45, 1.05), (-2.42, 1.70), (-1.9, 1.76), (0.55, 1.78), (1.25, 1.18), (2.35, 1.08),
                     (2.45, 0.85)]),
    'van': dict(L=5.3, W=2.0, H=2.1, belt=1.2, floor=0.35, wheel=0.35,
                top=[(-2.65, 1.1), (-2.62, 2.05), (1.2, 2.1), (1.9, 1.35), (2.6, 1.15), (2.65, 0.9)]),
    'hatch': dict(L=4.1, W=1.78, H=1.48, belt=0.92, floor=0.27, wheel=0.31,
                  top=[(-2.05, 0.95), (-2.0, 1.40), (-1.6, 1.46), (0.25, 1.48), (0.9, 1.05), (1.95, 0.92),
                       (2.05, 0.72)]),
}


class Mesh:
    def __init__(self):
        self.p, self.n, self.idx = [], [], []

    def tri(self, a, b, c, out=None, center=None):
        """Triângulo com a frente para fora: `out` (direção) ou para longe de `center` (peças fechadas)."""
        a, b, c = (np.asarray(x, np.float64) for x in (a, b, c))
        n = np.cross(b - a, c - a)
        ln = np.linalg.norm(n)
        if ln < 1e-12:
            return
        n /= ln
        ref = np.asarray(out, np.float64) if out is not None else (
            (a + b + c) / 3 - np.asarray(center, np.float64) if center is not None else None)
        if ref is not None and np.dot(n, ref) < 0:
            b, c, n = c, b, -n
        base = len(self.p)
        self.p += [a, b, c]
        self.n += [n, n, n]
        self.idx += [base, base + 1, base + 2]

    def quad(self, a, b, c, d, out=None, center=None):
        self.tri(a, b, c, out, center)
        self.tri(a, c, d, out, center)

    def cylinder_x(self, cx, cy, cz, r, w, seg=20):
        """Cilindro com o eixo em x (roda), centrado em (cx, cy, cz), largura w."""
        x0, x1 = cx - w / 2, cx + w / 2
        for i in range(seg):
            a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
            p0 = (cy + r * math.cos(a0), cz + r * math.sin(a0))
            p1 = (cy + r * math.cos(a1), cz + r * math.sin(a1))
            ctr = (cx, cy, cz)
            self.quad((x0, p0[0], p0[1]), (x0, p1[0], p1[1]), (x1, p1[0], p1[1]), (x1, p0[0], p0[1]), center=ctr)
            self.tri((x1, cy, cz), (x1, p0[0], p0[1]), (x1, p1[0], p1[1]), out=(1, 0, 0))
            self.tri((x0, cy, cz), (x0, p1[0], p1[1]), (x0, p0[0], p0[1]), out=(-1, 0, 0))

    def box(self, c, h):
        cx, cy, cz = c
        hx, hy, hz = h
        V = [(cx + sx * hx, cy + sy * hy, cz + sz * hz) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
        I = lambda x, y, z: V[4 * x + 2 * y + z]
        for q in ((I(0, 0, 1), I(1, 0, 1), I(1, 1, 1), I(0, 1, 1)), (I(1, 0, 0), I(0, 0, 0), I(0, 1, 0), I(1, 1, 0)),
                  (I(1, 0, 1), I(1, 0, 0), I(1, 1, 0), I(1, 1, 1)), (I(0, 0, 0), I(0, 0, 1), I(0, 1, 1), I(0, 1, 0)),
                  (I(0, 1, 1), I(1, 1, 1), I(1, 1, 0), I(0, 1, 0)), (I(0, 0, 0), I(1, 0, 0), I(1, 0, 1), I(0, 0, 1))):
            self.quad(*q, center=c)


def profile(m, nseg=48):
    """Contorno de cima reamostrado em z (de trás para a frente) e altura do assoalho."""
    zs, ys = zip(*m['top'])
    z = np.linspace(zs[0], zs[-1], nseg)
    y = np.interp(z, zs, ys)
    return z, y


def build_car(m, taxi=False):
    body, glass, dark, chrome, head, tail, sign, plate = (Mesh() for _ in range(8))
    z, ytop = profile(m)
    W, floor, belt = m['W'], m['floor'], m['belt']
    hw = W / 2
    # Planta com as quinas chanfradas: a largura afina perto dos para-choques e no teto (tumblehome).
    def half_width(zz, yy):
        k = 1.0 - 0.10 * max(0.0, (abs(zz) - (m['L'] / 2 - 0.45)) / 0.45)
        t = 1.0 - 0.12 * max(0.0, (yy - belt) / max(m['H'] - belt, 0.1))
        return hw * k * t
    rows = [floor, (floor + belt) / 2, belt]  # cortes na lateral: assoalho, meio, cintura
    for i in range(len(z) - 1):
        z0, z1 = z[i], z[i + 1]
        y0t, y1t = ytop[i], ytop[i + 1]
        # lateral (de assoalho até o contorno), em faixas; acima da cintura é vidro
        for side in (-1, 1):
            for a, b in ((floor, min(belt, y0t)), (belt, None)):
                if b is None:  # vidro: da cintura ao teto
                    if y0t <= belt + 0.02 and y1t <= belt + 0.02:
                        continue
                    ya0, yb0, ya1, yb1 = belt, y0t, belt, y1t
                    tgt = glass
                else:
                    ya0, yb0, ya1, yb1 = floor, min(belt, y0t), floor, min(belt, y1t)
                    tgt = body
                P = lambda zz, yy: (side * half_width(zz, yy), yy, zz)
                q = [P(z0, ya0), P(z1, ya1), P(z1, yb1), P(z0, yb0)]
                tgt.quad(*q, out=(side, 0, 0))
        # teto/capô (de um lado ao outro): vidro nos trechos inclinados do para-brisa/vigia, carroceria no resto
        slope = abs(y1t - y0t) / max(z1 - z0, 1e-6)
        is_glass = slope > 0.45 and min(y0t, y1t) > belt
        tgt = glass if is_glass else body
        a = (-half_width(z0, y0t), y0t, z0)
        b = (half_width(z0, y0t), y0t, z0)
        c = (half_width(z1, y1t), y1t, z1)
        d = (-half_width(z1, y1t), y1t, z1)
        tgt.quad(a, d, c, b, out=(0, 1, 0))
        # assoalho
        dark.quad((-hw * 0.9, floor, z0), (hw * 0.9, floor, z0), (hw * 0.9, floor, z1), (-hw * 0.9, floor, z1),
                  out=(0, -1, 0))
    # traseira e frente (tampas)
    for zz, yy, s in ((z[0], ytop[0], -1), (z[-1], ytop[-1], 1)):
        w0, w1 = half_width(zz, floor), half_width(zz, yy)
        q = [(-w0, floor, zz), (w0, floor, zz), (w1, yy, zz), (-w1, yy, zz)]
        body.quad(*q, out=(0, 0, s))
    # para-choques escuros, grade, faróis, lanternas, placas
    zf, zr = z[-1], z[0]
    dark.box((0, floor + 0.12, zf + 0.04), (hw * 0.95, 0.1, 0.05))
    dark.box((0, floor + 0.12, zr - 0.04), (hw * 0.95, 0.1, 0.05))
    dark.box((0, floor + 0.32, zf + 0.01), (hw * 0.45, 0.07, 0.02))  # grade
    for s in (-1, 1):
        head.box((s * hw * 0.68, floor + 0.36, zf + 0.015), (0.2, 0.06, 0.02))
        tail.box((s * hw * 0.72, floor + 0.45, zr - 0.015), (0.18, 0.06, 0.02))
    plate.box((0, floor + 0.2, zf + 0.1), (0.26, 0.06, 0.005))
    plate.box((0, floor + 0.3, zr - 0.1), (0.26, 0.06, 0.005))
    # retrovisores
    for s in (-1, 1):
        body.box((s * (hw + 0.08), belt + 0.08, z[-1] * 0.15 + 0.6), (0.08, 0.06, 0.1))
    # rodas
    r = m['wheel']
    for zw in (m['L'] / 2 - 0.78, -m['L'] / 2 + 0.85):
        for s in (-1, 1):
            dark.cylinder_x(s * (hw - 0.1), r, zw, r, 0.24)
            chrome.cylinder_x(s * (hw - 0.0), r, zw, r * 0.55, 0.02)
    if taxi:  # luminoso no teto
        ytop_max = max(ytop)
        sign.box((0, ytop_max + 0.12, -0.2), (0.32, 0.1, 0.12))
    return dict(corpo=body, vidro=glass, escuro=dark, cromado=chrome, farol=head, lanterna=tail, luminoso=sign,
                placa=plate)


def write_gltf(path, parts, paint):
    bin_ = bytearray()
    doc = {'asset': {'version': '2.0', 'generator': 'legacy tools/carros.py'}, 'buffers': [], 'bufferViews': [],
           'accessors': [], 'meshes': [], 'nodes': [], 'materials': [], 'scenes': [{'nodes': []}], 'scene': 0,
           'extensionsUsed': ['KHR_materials_clearcoat', 'KHR_materials_emissive_strength']}

    def acc(arr, typ, comp, target, minmax=False):
        while len(bin_) % 4:
            bin_.append(0)
        arr = np.ascontiguousarray(arr)
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(bin_), 'byteLength': arr.nbytes, 'target': target})
        bin_.extend(arr.tobytes())
        a = {'bufferView': len(doc['bufferViews']) - 1, 'componentType': comp, 'count': int(arr.shape[0]), 'type': typ}
        if minmax:
            a['min'] = [float(x) for x in arr.min(0)]
            a['max'] = [float(x) for x in arr.max(0)]
        doc['accessors'].append(a)
        return len(doc['accessors']) - 1

    mats = {
        'corpo': {'name': 'pintura', 'pbrMetallicRoughness': {'baseColorFactor': list(paint) + [1], 'metallicFactor': 0.3,
                                                             'roughnessFactor': 0.35},
                  'extensions': {'KHR_materials_clearcoat': {'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.03}}},
        'vidro': {'name': 'vidro', 'pbrMetallicRoughness': {'baseColorFactor': [0.02, 0.025, 0.03, 1], 'metallicFactor': 0.0,
                                                           'roughnessFactor': 0.04}},
        'escuro': {'name': 'borracha', 'pbrMetallicRoughness': {'baseColorFactor': [0.03, 0.03, 0.03, 1],
                                                               'metallicFactor': 0.0, 'roughnessFactor': 0.8}},
        'cromado': {'name': 'calota', 'pbrMetallicRoughness': {'baseColorFactor': [0.7, 0.7, 0.72, 1], 'metallicFactor': 1.0,
                                                              'roughnessFactor': 0.25}},
        'farol': {'name': 'farol', 'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1]},
                  'emissiveFactor': [1.0, 0.95, 0.85], 'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 60.0}}},
        'lanterna': {'name': 'lanterna', 'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.02, 0.02, 1]},
                     'emissiveFactor': [1.0, 0.04, 0.02], 'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 12.0}}},
        'luminoso': {'name': 'luminoso_taxi', 'pbrMetallicRoughness': {'baseColorFactor': [1, 0.95, 0.8, 1]},
                     'emissiveFactor': [1.0, 0.9, 0.6], 'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 8.0}}},
        'placa': {'name': 'placa', 'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.85, 0.6, 1], 'roughnessFactor': 0.5}},
    }
    prims = []
    for name, mesh in parts.items():
        if not mesh.idx:
            continue
        doc['materials'].append(mats[name])
        P = np.array(mesh.p, np.float32)
        N = np.array(mesh.n, np.float32)
        I = np.array(mesh.idx, np.uint32)
        prims.append({'attributes': {'POSITION': acc(P, 'VEC3', 5126, 34962, True), 'NORMAL': acc(N, 'VEC3', 5126, 34962)},
                      'indices': acc(I, 'SCALAR', 5125, 34963), 'material': len(doc['materials']) - 1})
    doc['meshes'].append({'name': 'carro', 'primitives': prims})
    doc['nodes'].append({'name': 'carro', 'mesh': 0})
    doc['scenes'][0]['nodes'] = [0]
    base = os.path.splitext(os.path.basename(path))[0]
    doc['buffers'] = [{'uri': base + '.bin', 'byteLength': len(bin_)}]
    with open(os.path.join(os.path.dirname(path), base + '.bin'), 'wb') as f:
        f.write(bin_)
    with open(path, 'w') as f:
        json.dump(doc, f, separators=(',', ':'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--saida', default=os.path.join('cenas', 'times_square', 'carros'))
    a = ap.parse_args()
    os.makedirs(a.saida, exist_ok=True)
    yellow = (1.0, 0.72, 0.05)
    made = []
    for name, m in MODELS.items():
        write_gltf(os.path.join(a.saida, f'{name}.gltf'), build_car(m), (0.5, 0.5, 0.52))
        made.append(name)
    for name in ('seda', 'suv'):
        write_gltf(os.path.join(a.saida, f'taxi_{name}.gltf'), build_car(MODELS[name], taxi=True), yellow)
        made.append('taxi_' + name)
    print('carros:', made)


if __name__ == '__main__':
    main()
