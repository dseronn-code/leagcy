"""Gera um vídeo direto pelo terminal, sem interface.

    python -m cena.cli "um cachorro correndo na praia ao amanhecer" --estilo drone --segundos 4
"""

import argparse

from .director import ESTILOS, MOVIMENTOS, dirigir
from .engine import MODELOS, Motor, Pedido, info_gpu, modo_recomendado


def main() -> None:
    ap = argparse.ArgumentParser(description="Cena — gera um vídeo realista a partir de um texto")
    ap.add_argument("pedido")
    ap.add_argument("--estilo", choices=list(ESTILOS), default="cinema")
    ap.add_argument("--movimento", choices=list(MOVIMENTOS), default="auto")
    ap.add_argument("--modo", choices=list(MODELOS), default=None)
    ap.add_argument("--resolucao", default="480p")
    ap.add_argument("--segundos", type=float, default=4)
    ap.add_argument("--semente", type=int, default=-1)
    ap.add_argument("--vertical", action="store_true")
    ap.add_argument("--saida", default="videos")
    a = ap.parse_args()

    gpu = info_gpu()
    modo = a.modo or modo_recomendado(gpu["vram_gb"])
    print(f"GPU: {gpu['nome']} ({gpu['vram_gb']:.0f} GB) · modo: {MODELOS[modo].nome}")
    r = Motor(a.saida).gerar(
        Pedido(dirigir(a.pedido, a.estilo, a.movimento), modo=modo, resolucao=a.resolucao, segundos=a.segundos, semente=a.semente, vertical=a.vertical),
        progresso=lambda p, t: print(f"\rpasso {p}/{t}", end="", flush=True),
    )
    print(f"\n{r.arquivo}  ({r.segundos_gerando / 60:.1f} min, semente {r.semente})")


if __name__ == "__main__":
    main()
