"""Gera as marcas dos produtos (SVG, PNG 512, .ico do Windows, ícone da janela e a imagem da tela de carregamento).

Mesma família da marca Legacy: quadrado escuro arredondado, forma geométrica em degradê.
  legacy         — o "L" com o pixel (a marca da empresa e do launcher)
  legacy_engine  — cubo em três faces (o mundo 3D: o motor)
  times_square   — silhueta de prédios com o telão aceso (o pixel da Legacy virou o letreiro)
Rodar: python3 plataforma/marca/gerar_marcas.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

AQUI = os.path.dirname(os.path.abspath(__file__))
CENAS = os.path.join(AQUI, '..', '..', 'engine', 'cenas', 'marca')
FUNDO = '#0f1116'
TELA = (0x0b, 0x0c, 0x10)
QUENTE = ('#ffb547', '#ff5e7a')
NEON = ('#ff5e7a', '#8a5cff')

def encolher(p, d=1.3):
    """Afasta cada vértice do centro da face (frestas entre as faces do cubo)."""
    cx = sum(x for x, _ in p) / len(p)
    cy = sum(y for _, y in p) / len(p)
    out = []
    for x, y in p:
        vx, vy = x - cx, y - cy
        n = (vx * vx + vy * vy) ** 0.5
        out.append((round(x - vx / n * d, 2), round(y - vy / n * d, 2)))
    return out


# formas no quadro 64×64: (tipo, pontos, degradê)
MARCAS = {
    'legacy': {
        'nome': 'Legacy', 'sub': '',
        'formas': [('poly', [(16, 12), (26, 12), (26, 42), (48, 42), (48, 52), (16, 52)], QUENTE),
                   ('rect', (38, 12, 48, 22, 2), QUENTE)],
    },
    'legacy_engine': {
        'nome': 'Legacy Engine', 'sub': 'Carregando',
        'formas': [('poly', encolher([(32, 10), (51, 21), (32, 32), (13, 21)]), ('#ffc46a', '#ffa04f')),
                   ('poly', encolher([(13, 21), (32, 32), (32, 54), (13, 43)]), ('#ff9a52', '#ff6f6c')),
                   ('poly', encolher([(32, 32), (51, 21), (51, 43), (32, 54)]), ('#ff6f74', '#d9466c'))],
    },
    'times_square': {
        'nome': 'Times Square', 'sub': 'Carregando a cidade',
        'formas': [('poly', [(11, 53), (11, 33), (15, 33), (15, 28), (23, 28), (23, 53)], NEON),
                   ('poly', [(26, 53), (26, 21), (29, 21), (29, 16), (31, 16), (31, 8), (32.5, 8), (32.5, 16),
                             (34.5, 16), (34.5, 21), (37.5, 21), (37.5, 53)], NEON),
                   ('poly', [(40.5, 53), (40.5, 25), (52, 25), (52, 53)], NEON),
                   ('rect', (37, 31, 55, 40, 1.5), QUENTE)],
    },
}


def svg(m):
    defs, corpo = [], []
    grads = {}
    for tipo, p, g in m['formas']:
        if g not in grads:
            gid = f'g{len(grads)}'
            grads[g] = gid
            defs.append(f'<linearGradient id="{gid}" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{g[0]}"/>'
                        f'<stop offset="1" stop-color="{g[1]}"/></linearGradient>')
        f = f'url(#{grads[g]})'
        if tipo == 'poly':
            corpo.append(f'<path d="M{" L".join(f"{x:g} {y:g}" for x, y in p)}z" fill="{f}"/>')
        else:
            x0, y0, x1, y1, r = p
            corpo.append(f'<rect x="{x0:g}" y="{y0:g}" width="{x1 - x0:g}" height="{y1 - y0:g}" rx="{r:g}" fill="{f}"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">'
            f'<defs>{"".join(defs)}</defs><rect width="64" height="64" rx="15" fill="{FUNDO}"/>{"".join(corpo)}</svg>\n')


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def degrade(n, g):
    a, b = hexrgb(g[0]), hexrgb(g[1])
    im = Image.new('RGBA', (n, n))
    px = im.load()
    for y in range(n):
        for x in range(n):
            t = (x + y) / (2 * (n - 1))
            px[x, y] = tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3)) + (255,)
    return im


def png(m, n, fundo=True):
    s = 4 * n  # superamostragem para bordas lisas
    k = s / 64
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    if fundo:
        ImageDraw.Draw(im).rounded_rectangle((0, 0, s - 1, s - 1), 15 * k, fill=FUNDO)
    cache = {}
    for tipo, p, g in m['formas']:
        mask = Image.new('L', (s, s), 0)
        d = ImageDraw.Draw(mask)
        if tipo == 'poly':
            d.polygon([(x * k, y * k) for x, y in p], fill=255)
        else:
            x0, y0, x1, y1, r = p
            d.rounded_rectangle((x0 * k, y0 * k, x1 * k - 1, y1 * k - 1), r * k, fill=255)
        if g not in cache:
            cache[g] = degrade(s, g)
        im.paste(cache[g], (0, 0), mask)
    return im.resize((n, n), Image.LANCZOS)


def fonte(tam, peso='Bold'):
    for f in (f'/usr/share/fonts/opentype/inter/InterDisplay-{peso}.otf', f'/usr/share/fonts/opentype/inter/Inter-{peso}.otf',
              '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'):
        if os.path.exists(f):
            return ImageFont.truetype(f, tam)
    return ImageFont.load_default()


def tela(m):
    """Imagem da tela de carregamento: marca e nome sobre o fundo da tela (o motor copia sem mistura de alfa)."""
    W, H = 1200, 600
    im = Image.new('RGB', (W, H), TELA)
    icone = png(m, 260)
    im.paste(icone, ((W - 260) // 2, 70), icone)
    d = ImageDraw.Draw(im)
    f = fonte(78)
    w = d.textlength(m['nome'], font=f)
    d.text(((W - w) / 2, 370), m['nome'], font=f, fill=(0xf2, 0xf3, 0xf7))
    if m['sub']:
        f2 = fonte(30, 'Medium')
        w2 = d.textlength(m['sub'], font=f2)
        d.text(((W - w2) / 2, 478), m['sub'], font=f2, fill=(0x8b, 0x90, 0xa0))
    return im


def main():
    os.makedirs(CENAS, exist_ok=True)
    for chave, m in MARCAS.items():
        open(os.path.join(AQUI, f'{chave}.svg'), 'w').write(svg(m))
        png(m, 512).save(os.path.join(AQUI, f'{chave}-512.png'))
        png(m, 256).save(os.path.join(AQUI, f'{chave}.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
        png(m, 256).save(os.path.join(CENAS, f'{chave}_icone.png'))
        tela(m).save(os.path.join(CENAS, f'{chave}_carregamento.png'), optimize=True)
        print('ok', chave)


if __name__ == '__main__':
    main()
