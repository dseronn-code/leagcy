"""Interface web da Cena (Gradio). Roda no Kaggle/Colab gratuito ou no seu PC.

    python -m cena.app            # local: http://127.0.0.1:7860
    python -m cena.app --share    # link público gratuito (útil no Kaggle/Colab)
"""

from __future__ import annotations

import argparse
import threading

from .director import ESTILOS, MOVIMENTOS, dirigir
from .engine import MODELOS, Motor, Pedido, info_gpu, modo_recomendado, usar_economia

EXEMPLOS = [
    ["Um homem de 30 anos acorda com o despertador do celular, se espreguiça e sorri enquanto o sol entra pela janela", "comercial", "aproximando"],
    ["Uma senhora rindo enquanto prepara café numa cozinha antiga do interior de Minas, vapor subindo da chaleira", "documentario", "parada"],
    ["Motociclista atravessando a Avenida Paulista à noite na chuva, reflexos de neon no asfalto molhado", "noturno", "acompanhando"],
    ["Ondas quebrando em falésias ao pôr do sol no litoral nordestino", "drone", "auto"],
]

ESTILO_ROTULOS = {"cinema": "Cinema", "comercial": "Comercial", "documentario": "Documentário", "celular": "Filmado no celular", "noturno": "Noturno", "drone": "Drone"}
MOV_ROTULOS = {"auto": "Automático", "parada": "Câmera parada", "aproximando": "Aproximando", "afastando": "Afastando", "acompanhando": "Acompanhando", "orbitando": "Orbitando", "panoramica": "Panorâmica"}


def criar_app(motor: Motor | None = None):
    import gradio as gr

    motor = motor or Motor()
    trava = threading.Lock()  # uma geração por vez: a GPU gratuita não aguenta duas
    gpu = info_gpu()
    # Com pouca RAM (Colab grátis) a Máxima recarrega o modelo a cada vídeo: começa pela Rápida.
    modo_padrao = modo_recomendado(gpu["vram_gb"]) if gpu["cuda"] and not usar_economia() else "rapida"
    res_padrao = "720p" if gpu["vram_gb"] >= 20 else "480p"

    def gerar(pedido, estilo, movimento, modo, resolucao, segundos, vertical, semente, passos, evitar, progress=gr.Progress()):
        direcao = dirigir(pedido, estilo, movimento, evitar or "")
        progress(0, desc="Carregando o modelo (na primeira vez baixa ~20 GB; demora)…")
        with trava:
            r = motor.gerar(
                Pedido(direcao, modo=modo, resolucao=resolucao, segundos=float(segundos), semente=int(semente), passos=int(passos) or None, vertical=bool(vertical)),
                progresso=lambda p, total: progress(p / total, desc=f"Gerando… passo {p}/{total}"),
            )
        minutos = r.segundos_gerando / 60
        info = (
            f"**Pronto em {minutos:.1f} min** · {r.largura}×{r.altura} · {r.quadros} quadros a {r.fps} fps · semente `{r.semente}`  \n"
            f"Gostou do movimento mas quer variar detalhes? Reuse a semente e mude o texto.\n\n"
            f"<details><summary>Prompt enviado ao modelo</summary>\n\n{direcao.prompt}\n\n</details>"
        )
        return str(r.arquivo), info

    with gr.Blocks(title="Cena — IA de vídeo realista") as demo:
        gr.Markdown(
            "# Cena\n**IA de vídeo realista, 100% gratuita e de código aberto** (modelo Wan, Apache 2.0).  \n"
            f"GPU: **{gpu['nome']}** ({gpu['vram_gb']:.0f} GB). Em GPU gratuita cada vídeo leva de alguns minutos a meia hora — vale a pena esperar."
        )
        with gr.Row():
            with gr.Column(scale=3):
                pedido = gr.Textbox(label="O que deve acontecer na cena?", lines=4, placeholder="Quem, onde, fazendo o quê, com que luz… Quanto mais concreto, mais real.")
                with gr.Row():
                    estilo = gr.Dropdown([(ESTILO_ROTULOS[k], k) for k in ESTILOS], value="cinema", label="Estilo")
                    movimento = gr.Dropdown([(MOV_ROTULOS[k], k) for k in MOVIMENTOS], value="auto", label="Câmera")
                with gr.Row():
                    modo = gr.Dropdown([(m.nome, k) for k, m in MODELOS.items()], value=modo_padrao, label="Qualidade")
                    resolucao = gr.Dropdown(["720p", "480p"], value=res_padrao, label="Resolução")
                    segundos = gr.Slider(2, 5, value=4, step=1, label="Duração (s)")
                    vertical = gr.Checkbox(label="Vertical (Reels/TikTok)", value=False)
                with gr.Accordion("Avançado", open=False):
                    semente = gr.Number(value=-1, precision=0, label="Semente (-1 = aleatória)")
                    passos = gr.Slider(0, 60, value=0, step=1, label="Passos (0 = padrão do modelo; menos = mais rápido, menos detalhe)")
                    evitar = gr.Textbox(label="Evitar também (opcional)", placeholder="ex.: chapéu, multidão")
                botao = gr.Button("Gerar vídeo", variant="primary")
                gr.Examples(EXEMPLOS, inputs=[pedido, estilo, movimento])
            with gr.Column(scale=4):
                video = gr.Video(label="Resultado", autoplay=True)
                info = gr.Markdown()
        botao.click(gerar, [pedido, estilo, movimento, modo, resolucao, segundos, vertical, semente, passos, evitar], [video, info])
    demo.queue(max_size=8)
    return demo


def main() -> None:
    ap = argparse.ArgumentParser(description="Cena — IA de vídeo realista")
    ap.add_argument("--share", action="store_true", help="cria um link público gratuito (Kaggle/Colab)")
    ap.add_argument("--porta", type=int, default=7860)
    ap.add_argument("--saida", default="videos")
    a = ap.parse_args()
    criar_app(Motor(a.saida)).launch(share=a.share, server_name="0.0.0.0", server_port=a.porta)


if __name__ == "__main__":
    main()
