#!/usr/bin/env python3
"""Guarda-chuvas abertos para a multidão na chuva (glTF, um por cor). Uso (de engine/):

    python3 tools/guarda_chuva.py [--saida cenas/times_square/guarda_chuva]

Copa de 8 gomos (raio 0,5 m) com as varetas marcadas: cada gomo é uma superfície que cai em curva do topo à borda e
afunda entre as varetas (o tecido esticado), com as pontas das varetas um pouco para fora; haste fina, ponteira e
cabo em gancho (J). Origem na mão: o cabo fica em y = 0 e a copa ~0,8 m acima.
"""
import argparse
import math
import os

import numpy as np

from carros import Mesh, write_gltf

CORES = {'preto': (0.03, 0.03, 0.035), 'azul': (0.04, 0.09, 0.3), 'vermelho': (0.45, 0.03, 0.03),
         'verde': (0.05, 0.18, 0.1), 'bege': (0.55, 0.45, 0.32), 'xadrez': (0.25, 0.08, 0.06)}


def vareta(m, p0, p1, r, seg=6):
    """Tubo fino de p0 a p1 (prisma de `seg` lados)."""
    p0, p1 = np.asarray(p0, float), np.asarray(p1, float)
    d = p1 - p0
    d /= np.linalg.norm(d)
    u = np.cross(d, (0, 1, 0) if abs(d[1]) < 0.9 else (1, 0, 0))
    u /= np.linalg.norm(u)
    v = np.cross(d, u)
    for k in range(seg):
        a0, a1 = 2 * math.pi * k / seg, 2 * math.pi * (k + 1) / seg
        o0 = r * (math.cos(a0) * u + math.sin(a0) * v)
        o1 = r * (math.cos(a1) * u + math.sin(a1) * v)
        m.quad(p0 + o0, p0 + o1, p1 + o1, p1 + o0, center=(p0 + p1) / 2)


def guarda_chuva():
    copa, haste, ponta = Mesh(), Mesh(), Mesh()
    R, topo, queda, gomos = 0.5, 0.82, 0.26, 8
    na, nr = 8, 8  # amostras por gomo (ângulo) e no raio

    def pt(a, f):
        """Ponto da copa: ângulo a, fração do raio f. O tecido afunda entre as varetas (seno do gomo)."""
        g = (a * gomos / (2 * math.pi)) % 1.0
        afunda = math.sin(math.pi * g) * 0.06 * f ** 1.5  # entre varetas o tecido fica mais baixo e mais para dentro
        r = R * f * (1.0 - 0.05 * math.sin(math.pi * g) * f)
        y = topo - queda * f ** 1.7 - afunda
        return np.array([r * math.cos(a), y, r * math.sin(a)])

    A = [2 * math.pi * k / (gomos * na) for k in range(gomos * na + 1)]
    F = [k / nr for k in range(nr + 1)]
    G = np.array([[pt(a, f) for f in F] for a in A])
    ta = np.gradient(G, axis=0)
    tf = np.gradient(G, axis=1)
    N = np.cross(tf, ta)
    N[np.sum(N * np.array([0, 1, 0]), -1) < 0] *= -1
    N[:, 0] = (0, 1, 0)
    N /= np.maximum(np.linalg.norm(N, axis=-1, keepdims=True), 1e-9)
    for i in range(len(A) - 1):
        for j in range(nr):
            q = [G[i, j], G[i + 1, j], G[i + 1, j + 1], G[i, j + 1]]
            nq = [N[i, j], N[i + 1, j], N[i + 1, j + 1], N[i, j + 1]]
            copa.quad_n(q, nq)
            # lado de baixo (o tecido visto de dentro), um pouco abaixo
            copa.quad_n([v - (0, 0.004, 0) for v in q[::-1]], [-n for n in nq[::-1]])
    for k in range(gomos):  # pontas das varetas e as varetas por baixo
        a = 2 * math.pi * k / gomos
        p = pt(a, 1.0)
        ponta.box((p[0] * 1.03, p[1] - 0.01, p[2] * 1.03), (0.008, 0.012, 0.008))
        fs = np.linspace(0.04, 1.0, 7)
        for f0, f1 in zip(fs[:-1], fs[1:]):  # vareta seguindo a curva da copa, 1 cm abaixo do tecido
            vareta(haste, pt(a, f0) - (0, 0.01, 0), pt(a, f1) - (0, 0.01, 0), 0.0035)
        # haste de apoio: do meio da vareta até a haste central (o "runner")
        vareta(haste, pt(a, 0.45) - (0, 0.012, 0), np.array([0.0, topo - 0.32, 0.0]), 0.003)
    # haste vertical, ponteira no topo, cabo em J
    seg = 10
    for k in range(seg):
        a0, a1 = 2 * math.pi * k / seg, 2 * math.pi * (k + 1) / seg
        r = 0.009
        haste.quad((r * math.cos(a0), 0.0, r * math.sin(a0)), (r * math.cos(a1), 0.0, r * math.sin(a1)),
                   (r * math.cos(a1), topo, r * math.sin(a1)), (r * math.cos(a0), topo, r * math.sin(a0)),
                   center=(0, topo / 2, 0))
    ponta.box((0, topo + 0.05, 0), (0.008, 0.05, 0.008))
    for k in range(10):  # gancho: meia-volta de raio 5 cm abaixo da mão
        t0, t1 = math.pi * k / 10, math.pi * (k + 1) / 10
        c0 = np.array([0.05 - 0.05 * math.cos(t0), -0.06 - 0.05 * math.sin(t0), 0])
        c1 = np.array([0.05 - 0.05 * math.cos(t1), -0.06 - 0.05 * math.sin(t1), 0])
        vareta(haste, c0, c1, 0.014, seg=8)
    vareta(haste, (0, -0.06, 0), (0, 0.06, 0), 0.016, seg=10)  # empunhadura
    return dict(corpo=copa, escuro=haste, cromado=ponta)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--saida', default=os.path.join('cenas', 'times_square', 'guarda_chuva'))
    a = ap.parse_args()
    os.makedirs(a.saida, exist_ok=True)
    partes = guarda_chuva()
    for nome, cor in CORES.items():
        write_gltf(os.path.join(a.saida, f'{nome}.gltf'), partes, cor, verniz=False)
    print('guarda-chuvas:', list(CORES))


if __name__ == '__main__':
    main()
