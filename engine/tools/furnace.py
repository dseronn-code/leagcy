#!/usr/bin/env python3
# Teste da fornalha branca: python3 tools/furnace.py imagem.png  (gerada com --scene furnace --exposure 2.2)
import sys, numpy as np
from PIL import Image
def lin(a):
    a = a.astype(np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
for f in sys.argv[1:]:
    img = lin(np.asarray(Image.open(f).convert('RGB')))
    H, W, _ = img.shape
    lum = img @ np.array([0.2126, 0.7152, 0.0722])
    bg = np.median(lum[5:40, :])  # faixa superior: só céu
    hh = 13 * np.tan(np.radians(30)); hw = hh * W / H
    rows = []
    for row, y in ((0, 1.3), (1, -1.3)):
        vals = []
        for i in range(6):
            x = -6.25 + 2.5 * i
            px = int((x / hw * 0.5 + 0.5) * W); py = int((0.5 - y / hh * 0.5) * H)
            # Fundo local: anel logo fora da esfera (mesma vinheta/bloom que o centro).
            rpx = 1.0 / hh * 0.5 * H
            yy, xx = np.mgrid[0:H, 0:W]
            d = np.hypot(xx - px, yy - py)
            ring = lum[(d > 1.15 * rpx) & (d < 1.3 * rpx) & (np.abs(yy - py) < 0.5 * rpx)]
            vals.append(lum[py-8:py+9, px-8:px+9].mean() / np.median(ring))
        rows.append(vals)
    print(f.split('/')[-1], "fundo=%.3f" % bg)
    for name, vals in zip(("metal f0=1 ", "dielétrico "), rows):
        print("  %s rugosidade 0..1: %s" % (name, "  ".join("%.3f" % v for v in vals)))
