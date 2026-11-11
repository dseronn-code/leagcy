#!/usr/bin/env python3
"""Converte captura de movimento BVH (100STYLE, CMU...) em glTF com esqueleto e animação. Uso (de engine/):

    python3 -I tools/mocap.py entrada.bvh saida.gltf [--nome andar] [--escala 0.01] [--de 0] [--ate -1] [--passo 1]
                              [--no-lugar]

O glTF sai sem malha: só os ossos (nós) e uma animação com rotação de cada osso e translação da raiz. O motor copia
essa animação para outro esqueleto pelo retargeting ("retarget" no "visual" do personagem: diferença de rotação no
espaço do modelo, pose de referência corrigida sozinha). --no-lugar tira o deslocamento horizontal da raiz (a
locomoção anda pela física do personagem, não pela animação).

BVH: canais de rotação em graus na ordem escrita (ex.: Zrotation Yrotation Xrotation = R = Rz·Ry·Rx), posição da raiz
nas unidades do arquivo (centímetros no 100STYLE e no CMU: --escala 0.01 para metros).
"""
import argparse
import json
import math
import os

import numpy as np


def parse_bvh(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        tok = f.read().split()
    i = 0
    joints = []  # dicionários: name, parent, offset, channels
    stack = []

    def expect(word):
        nonlocal i
        assert tok[i] == word, (tok[i], word)
        i += 1
    expect('HIERARCHY')
    while tok[i] != 'MOTION':
        t = tok[i]
        if t in ('ROOT', 'JOINT'):
            name = tok[i + 1]
            i += 2
            expect('{')
            j = {'name': name, 'parent': stack[-1] if stack else -1, 'offset': None, 'channels': []}
            joints.append(j)
            stack.append(len(joints) - 1)
        elif t == 'End':
            i += 2
            expect('{')
            expect('OFFSET')
            i += 3
            expect('}')
        elif t == 'OFFSET':
            joints[stack[-1]]['offset'] = [float(x) for x in tok[i + 1:i + 4]]
            i += 4
        elif t == 'CHANNELS':
            n = int(tok[i + 1])
            joints[stack[-1]]['channels'] = tok[i + 2:i + 2 + n]
            i += 2 + n
        elif t == '}':
            stack.pop()
            i += 1
        else:
            i += 1
    expect('MOTION')
    expect('Frames:')
    frames = int(tok[i])
    i += 1
    expect('Frame')
    expect('Time:')
    dt = float(tok[i])
    i += 1
    nch = sum(len(j['channels']) for j in joints)
    data = np.array(tok[i:i + frames * nch], np.float64).reshape(frames, nch)
    return joints, data, dt


def axis_quat(axis, deg):
    a = math.radians(deg) / 2
    s = math.sin(a)
    v = {'X': (s, 0, 0), 'Y': (0, s, 0), 'Z': (0, 0, s)}[axis]
    return np.array([v[0], v[1], v[2], math.cos(a)])


def qmul(a, b):  # (x, y, z, w)
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz])


def convert(args):
    joints, data, dt = parse_bvh(args.entrada)
    end = data.shape[0] if args.ate < 0 else min(args.ate, data.shape[0])
    frames = range(args.de, end, args.passo)
    times = np.array([(k - args.de) * dt for k in frames], np.float32)
    rot = np.zeros((len(joints), len(times), 4), np.float32)
    root_t = np.zeros((len(times), 3), np.float32)
    col = 0
    for ji, j in enumerate(joints):
        chans = j['channels']
        for fi, k in enumerate(frames):
            q = np.array([0.0, 0.0, 0.0, 1.0])
            pos = np.zeros(3)
            for c, name in enumerate(chans):
                v = data[k, col + c]
                if name.endswith('rotation'):
                    q = qmul(q, axis_quat(name[0].upper(), v))  # ordem do arquivo: R = R1·R2·R3
                else:
                    pos['XYZ'.index(name[0].upper())] = v
            q /= np.linalg.norm(q)
            if fi and np.dot(q, rot[ji, fi - 1]) < 0:  # mantém o hemisfério (interpolação sem giro)
                q = -q
            rot[ji, fi] = q
            if ji == 0 and any(n.endswith('position') for n in chans):
                root_t[fi] = pos * args.escala
        col += len(chans)
    if args.no_lugar:  # deslocamento horizontal fora: a raiz fica sobre a origem (altura e balanço continuam)
        root_t[:, 0] -= root_t[:, 0]
        root_t[:, 2] -= root_t[:, 2]

    bin_ = bytearray()
    doc = {'asset': {'version': '2.0', 'generator': 'legacy tools/mocap.py'}, 'buffers': [], 'bufferViews': [],
           'accessors': [], 'nodes': [], 'animations': [], 'scenes': [{'nodes': [0]}], 'scene': 0}

    def acc(arr, typ, minmax=False):
        while len(bin_) % 4:
            bin_.append(0)
        arr = np.ascontiguousarray(arr, np.float32)
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(bin_), 'byteLength': arr.nbytes})
        bin_.extend(arr.tobytes())
        a = {'bufferView': len(doc['bufferViews']) - 1, 'componentType': 5126, 'count': int(arr.shape[0]), 'type': typ}
        if minmax:
            a['min'] = [float(arr.min())]
            a['max'] = [float(arr.max())]
        doc['accessors'].append(a)
        return len(doc['accessors']) - 1

    for ji, j in enumerate(joints):
        off = np.array(j['offset'] or [0, 0, 0]) * args.escala
        n = {'name': j['name'], 'translation': [float(x) for x in off]}
        kids = [k for k, c in enumerate(joints) if c['parent'] == ji]
        if kids:
            n['children'] = kids
        doc['nodes'].append(n)
    t_acc = acc(times, 'SCALAR', True)
    samplers, channels = [], []
    for ji in range(len(joints)):
        samplers.append({'input': t_acc, 'output': acc(rot[ji], 'VEC4'), 'interpolation': 'LINEAR'})
        channels.append({'sampler': len(samplers) - 1, 'target': {'node': ji, 'path': 'rotation'}})
    root_off = np.array(joints[0]['offset'] or [0, 0, 0]) * args.escala
    samplers.append({'input': t_acc, 'output': acc(root_t + root_off, 'VEC3'), 'interpolation': 'LINEAR'})
    channels.append({'sampler': len(samplers) - 1, 'target': {'node': 0, 'path': 'translation'}})
    doc['animations'].append({'name': args.nome, 'samplers': samplers, 'channels': channels})
    base = os.path.splitext(os.path.basename(args.saida))[0]
    doc['buffers'] = [{'uri': base + '.bin', 'byteLength': len(bin_)}]
    with open(os.path.join(os.path.dirname(os.path.abspath(args.saida)), base + '.bin'), 'wb') as f:
        f.write(bin_)
    with open(args.saida, 'w') as f:
        json.dump(doc, f, separators=(',', ':'))
    print(f'{args.saida}: {len(joints)} ossos, {len(times)} quadros, {times[-1]:.2f} s')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('entrada')
    ap.add_argument('saida')
    ap.add_argument('--nome', default='anim')
    ap.add_argument('--escala', type=float, default=0.01)
    ap.add_argument('--de', type=int, default=0)
    ap.add_argument('--ate', type=int, default=-1)
    ap.add_argument('--passo', type=int, default=1)
    ap.add_argument('--no-lugar', action='store_true')
    convert(ap.parse_args())


if __name__ == '__main__':
    main()
