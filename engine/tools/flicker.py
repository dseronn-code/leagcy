#!/usr/bin/env python3
# Uso: python3 tools/flicker.py "pasta/f_*.png" nome  (sequência gerada com --save-every 1, câmera parada)
# Métrica de cintilação temporal em uma sequência de PNGs (câmera parada).
import sys, glob, numpy as np
from PIL import Image
files = sorted(glob.glob(sys.argv[1]))[-16:]
def lin(f):
    a = np.asarray(Image.open(f).convert('RGB')).astype(np.float32) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32)
L = np.stack([lin(f) for f in files])          # (frames, h, w)
mean = L.mean(0); std = L.std(0)
cv = std / (mean + 0.01)                       # variação relativa por pixel
med = np.median(L, 0)
sparkle = (L > 2.0 * med + 0.02).any(0)        # pixel que "pisca" acima do dobro da mediana
print(f"{sys.argv[2]:28s} frames={len(files)} cintilação média={cv.mean():.4f} p99={np.percentile(cv,99):.4f} "
      f"brilhinhos={100*sparkle.mean():.2f}% dos pixels  brilho médio={mean.mean():.3f}")
