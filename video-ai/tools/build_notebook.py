"""Gera notebooks/cena.ipynb, autocontido, a partir do código em cena/.

O notebook roda de graça no Kaggle (GPU T4) ou no Google Colab. Como o código
vai embutido, não é preciso clonar o repositório nem ter conta em lugar nenhum
além do próprio Kaggle/Colab.

    python tools/build_notebook.py
"""

import json
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[1]
ARQUIVOS = ["__init__.py", "director.py", "engine.py", "app.py", "cli.py"]


def md(texto):
    return {"cell_type": "markdown", "metadata": {}, "source": texto.strip().splitlines(keepends=True)}


def code(texto):
    return {"cell_type": "code", "metadata": {}, "execution_count": None, "outputs": [], "source": texto.strip().splitlines(keepends=True)}


celulas = [
    md("""
# Cena: IA de vídeo realista, 100% gratuita

Modelo **Wan** (código aberto, Apache 2.0) rodando em GPU gratuita.

**Google Colab (só precisa de uma conta Google, sem telefone):**
1. Menu **Ambiente de execução → Alterar o tipo de ambiente de execução → GPU T4 → Salvar**.
2. Menu **Ambiente de execução → Executar tudo**.
3. Se aparecer um aviso de que o notebook não é do Google, clique em **Executar mesmo assim**.

A Cena detecta a pouca RAM do Colab grátis e entra sozinha no **modo econômico**.

**Kaggle** (exige verificação de telefone): *Accelerator* **GPU T4 x2**, *Internet* **On**, *Run all*.

A primeira execução baixa ~20–30 GB de pesos do modelo, o que leva alguns minutos.
Na T4 gratuita, cada vídeo leva **de ~5 a ~30 min**, conforme duração, resolução e qualidade.
"""),
    code("""
!nvidia-smi --query-gpu=name,memory.total --format=csv
import os
# No Kaggle, /kaggle/working tem só 20 GB: o cache do modelo vai para /tmp, que é maior.
os.environ.setdefault("HF_HOME", "/tmp/hf" if os.path.exists("/kaggle") else os.path.expanduser("~/.cache/huggingface"))
"""),
    code("%pip install -q -U diffusers transformers accelerate ftfy sentencepiece imageio imageio-ffmpeg gradio"),
    code("!mkdir -p cena"),
]
for nome in ARQUIVOS:
    fonte = (RAIZ / "cena" / nome).read_text()
    celulas.append(code(f"%%writefile cena/{nome}\n{fonte}"))

celulas += [
    md("""
## Opção 1: interface completa (recomendado)
Rode a célula abaixo e abra o link **`https://….gradio.live`** que aparecer. Ele funciona até no celular.
Deixe a célula rodando enquanto usa; o link vale por 72 h.
"""),
    code("""
from cena.app import criar_app
criar_app().launch(share=True, debug=True)
"""),
    md("""
## Opção 2: gerar direto aqui no notebook
Pare a célula de cima antes, para liberar a GPU.
"""),
    code("""
from IPython.display import Video
from cena.director import dirigir
from cena.engine import Motor, Pedido, info_gpu, modo_recomendado

pedido = "Um homem de 30 anos acorda com o despertador do celular, se espreguiça e sorri enquanto o sol entra pela janela"
gpu = info_gpu()
motor = Motor("videos")
r = motor.gerar(
    Pedido(dirigir(pedido, "comercial", "aproximando"), modo=modo_recomendado(gpu["vram_gb"]), resolucao="480p", segundos=4),
    progresso=lambda p, t: print(f"\\rpasso {p}/{t}", end=""),
)
print(f"\\n{r.arquivo} · {r.segundos_gerando/60:.1f} min · semente {r.semente}")
Video(str(r.arquivo), embed=True, width=832)
"""),
]

for i, c in enumerate(celulas):
    c["id"] = f"cena-{i:02d}"

nb = {
    "cells": celulas,
    "metadata": {
        "kernelspec": {"display_name": "Python 3", "language": "python", "name": "python3"},
        "language_info": {"name": "python"},
        "accelerator": "GPU",
        "colab": {"gpuType": "T4", "provenance": []},
        "kaggle": {"accelerator": "nvidiaTeslaT4", "isInternetEnabled": True, "isGpuEnabled": True},
    },
    "nbformat": 4,
    "nbformat_minor": 5,
}
saida = RAIZ / "notebooks" / "cena.ipynb"
saida.write_text(json.dumps(nb, ensure_ascii=False, indent=1))
print(f"{saida} ({len(celulas)} células)")
