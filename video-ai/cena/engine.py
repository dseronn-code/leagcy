"""Motor de geração: carrega o modelo Wan e transforma uma Direcao em MP4.

Dois modos, escolhidos pelo que cabe na GPU (gratuita) disponível:
  - "maxima": Wan 2.2 TI2V-5B, até 1280×704 a 24 fps (melhor realismo).
  - "rapida": Wan 2.1 T2V-1.3B, 832×480 a 16 fps (cabe em ~8 GB de VRAM).
Tudo de código aberto e gratuito; os pesos são baixados do Hugging Face na
primeira execução.
"""

from __future__ import annotations

import gc
import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .director import Direcao, quadros_para


@dataclass(frozen=True)
class Modelo:
    nome: str
    repo: str
    fps: int
    resolucoes: dict[str, tuple[int, int]]  # rótulo -> (largura, altura)
    passos: int
    guidance: float
    flow_shift: float
    vram_ideal_gb: float  # abaixo disso, usa descarregamento para a RAM


MODELOS: dict[str, Modelo] = {
    "maxima": Modelo(
        nome="Máxima (Wan 2.2 · 5B)",
        repo=os.environ.get("CENA_MODELO_MAXIMA", "Wan-AI/Wan2.2-TI2V-5B-Diffusers"),
        fps=24,
        resolucoes={"720p": (1280, 704), "480p": (832, 480)},
        passos=50,
        guidance=5.0,
        flow_shift=5.0,
        vram_ideal_gb=32,
    ),
    "rapida": Modelo(
        nome="Rápida (Wan 2.1 · 1.3B)",
        repo=os.environ.get("CENA_MODELO_RAPIDA", "Wan-AI/Wan2.1-T2V-1.3B-Diffusers"),
        fps=16,
        resolucoes={"480p": (832, 480)},
        passos=40,
        guidance=5.0,
        flow_shift=3.0,
        vram_ideal_gb=16,
    ),
}


@dataclass
class Pedido:
    direcao: Direcao
    modo: str = "maxima"
    resolucao: str = "720p"
    segundos: float = 5.0
    semente: int = -1
    passos: int | None = None
    guidance: float | None = None
    vertical: bool = False


@dataclass
class Resultado:
    arquivo: Path
    semente: int
    segundos_gerando: float
    largura: int
    altura: int
    quadros: int
    fps: int


Progresso = Callable[[int, int], None]


def ram_gb() -> float:
    try:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 1024**3
    except (ValueError, OSError, AttributeError):
        return 0.0


def usar_economia() -> bool:
    """Pouca RAM (Colab grátis tem ~12 GB): nunca deixa o modelo inteiro na memória do sistema."""
    forcado = os.environ.get("CENA_ECONOMIA")
    if forcado is not None:
        return forcado == "1"
    return 0 < ram_gb() < 24


def info_gpu() -> dict[str, Any]:
    """Descreve a GPU (ou a falta dela) para escolher modo e descarregamento."""
    try:
        import torch
    except ImportError:
        return {"cuda": False, "nome": "sem PyTorch", "vram_gb": 0.0}
    if not torch.cuda.is_available():
        return {"cuda": False, "nome": "sem GPU", "vram_gb": 0.0}
    p = torch.cuda.get_device_properties(0)
    return {"cuda": True, "nome": p.name, "vram_gb": p.total_memory / 1024**3}


def modo_recomendado(vram_gb: float) -> str:
    # O 5B roda com descarregamento a partir de ~15 GB (T4 do Kaggle/Colab); abaixo disso, o 1.3B.
    return "maxima" if vram_gb >= 14.5 else "rapida"


class Motor:
    """Mantém um pipeline carregado por vez (troca de modo libera a VRAM do anterior)."""

    def __init__(self, saida: Path | str = "videos", carregar: Callable[[Modelo], Any] | None = None):
        self.saida = Path(saida)
        self.saida.mkdir(parents=True, exist_ok=True)
        self._carregar = carregar or carregar_pipeline
        self._modo: str | None = None
        self._pipe: Any = None

    def pipeline(self, modo: str) -> Any:
        if modo not in MODELOS:
            raise ValueError(f"Modo desconhecido: {modo}")
        if self._modo != modo:
            self.descarregar()
            self._pipe = self._carregar(MODELOS[modo])
            self._modo = modo
        return self._pipe

    def descarregar(self) -> None:
        self._pipe = None
        self._modo = None
        gc.collect()
        try:
            import torch

            if torch.cuda.is_available():
                torch.cuda.empty_cache()
        except ImportError:
            pass

    def gerar(self, pedido: Pedido, progresso: Progresso | None = None) -> Resultado:
        m = MODELOS[pedido.modo]
        resolucao = pedido.resolucao if pedido.resolucao in m.resolucoes else next(iter(m.resolucoes))
        largura, altura = m.resolucoes[resolucao]
        if pedido.vertical:
            largura, altura = altura, largura
        quadros = quadros_para(pedido.segundos, m.fps)
        passos = pedido.passos or m.passos
        semente = pedido.semente if pedido.semente >= 0 else int.from_bytes(os.urandom(4), "big") % 2**31

        pipe = self.pipeline(pedido.modo)
        inicio = time.time()
        quadros_gerados = self._executar(
            pipe,
            prompt=pedido.direcao.prompt,
            negative_prompt=pedido.direcao.negativo,
            width=largura,
            height=altura,
            num_frames=quadros,
            num_inference_steps=passos,
            guidance_scale=pedido.guidance or m.guidance,
            semente=semente,
            progresso=progresso,
            total=passos,
        )
        arquivo = self.saida / f"cena-{time.strftime('%Y%m%d-%H%M%S')}-{semente}.mp4"
        exportar_mp4(quadros_gerados, arquivo, m.fps)
        return Resultado(arquivo, semente, time.time() - inicio, largura, altura, quadros, m.fps)

    def _executar(self, pipe: Any, *, semente: int, progresso: Progresso | None, total: int, **kwargs: Any) -> Any:
        import torch

        gerador = torch.Generator(device="cpu").manual_seed(semente)

        def ao_fim_do_passo(_pipe: Any, passo: int, _t: Any, cb_kwargs: dict) -> dict:
            if progresso:
                progresso(passo + 1, total)
            return cb_kwargs

        saida = pipe(generator=gerador, callback_on_step_end=ao_fim_do_passo, **kwargs)
        return saida.frames[0]


def _dtype() -> Any:
    import torch

    # bf16 é o formato nativo do Wan, mas só GPUs Ampere+ (RTX 30xx, A100...) o fazem em hardware.
    # A T4 gratuita do Kaggle/Colab emularia bf16 muitas vezes mais devagar, então nela usamos fp16.
    # CENA_DTYPE=bf16|fp16 força a escolha.
    escolha = os.environ.get("CENA_DTYPE")
    nativo_bf16 = torch.cuda.get_device_capability(0)[0] >= 8
    return torch.bfloat16 if escolha == "bf16" or (escolha is None and nativo_bf16) else torch.float16


def _exigir_gpu() -> None:
    import torch

    if not torch.cuda.is_available():
        raise RuntimeError(
            "Nenhuma GPU encontrada. No Kaggle: Settings → Accelerator → GPU T4 x2. "
            "No Colab: Ambiente de execução → Alterar tipo → GPU T4."
        )


def carregar_pipeline(m: Modelo) -> Any:
    """Carrega o Wan com as otimizações que fazem caber numa GPU gratuita."""
    import torch
    from diffusers import AutoencoderKLWan, WanPipeline
    from diffusers.schedulers.scheduling_unipc_multistep import UniPCMultistepScheduler

    _exigir_gpu()
    if usar_economia():
        return PipelineEconomico(m)
    dtype = _dtype()
    vae = AutoencoderKLWan.from_pretrained(m.repo, subfolder="vae", torch_dtype=torch.float32)
    pipe = WanPipeline.from_pretrained(m.repo, vae=vae, torch_dtype=dtype)
    pipe.scheduler = UniPCMultistepScheduler.from_config(pipe.scheduler.config, flow_shift=m.flow_shift)

    vram = torch.cuda.get_device_properties(0).total_memory / 1024**3
    if vram >= m.vram_ideal_gb:
        pipe.to("cuda")
    else:
        # Mantém só o componente em uso na GPU (codificador de texto → transformer → VAE).
        pipe.enable_model_cpu_offload()
    # Decodifica o vídeo em blocos: evita estourar a memória no último passo.
    pipe.vae.enable_tiling()

    lora = os.environ.get("CENA_LORA")
    if lora:
        # Opcional: LoRA de personagem/estilo (ou de aceleração) compatível com o modelo carregado.
        pipe.load_lora_weights(lora)
    return pipe


def exportar_mp4(quadros: Any, arquivo: Path, fps: int) -> Path:
    """Grava os quadros (lista de arrays HxWx3 em 0..1 ou 0..255) como MP4 H.264 de alta qualidade."""
    import imageio.v2 as imageio
    import numpy as np

    with imageio.get_writer(str(arquivo), fps=fps, codec="libx264", quality=9, pixelformat="yuv420p", macro_block_size=16) as w:
        for q in quadros:
            a = np.asarray(q)
            if a.dtype != np.uint8:
                a = (np.clip(a, 0, 1) * 255).round().astype(np.uint8)
            w.append_data(a)
    return arquivo


class PipelineEconomico:
    """Wan para máquinas com pouca RAM (ex.: Colab grátis, ~12 GB).

    O codificador de texto (UMT5-XXL, ~11 GB) e o gerador de vídeo não cabem
    juntos na RAM nem na VRAM de uma T4. Então, a cada vídeo:
      1. carrega só o codificador de texto direto na GPU, processa o pedido e o descarta;
      2. carrega (ou reaproveita) o gerador de vídeo direto na GPU, sem passar pela RAM,
         e gera a partir do texto já processado.
    É mais lento por vídeo (recarrega o codificador), mas cabe.
    """

    def __init__(self, m: Modelo):
        self.m = m
        self._pipe: Any = None

    def _codificar(self, prompt: str, negativo: str) -> tuple[Any, Any]:
        import torch
        from diffusers import WanPipeline
        from transformers import AutoTokenizer, UMT5EncoderModel

        dtype = _dtype()
        tokenizer = AutoTokenizer.from_pretrained(self.m.repo, subfolder="tokenizer")
        encoder = UMT5EncoderModel.from_pretrained(self.m.repo, subfolder="text_encoder", dtype=dtype, device_map="cuda")
        # Reaproveita o encode_prompt oficial do pipeline (mesmo pré-processamento e preenchimento).
        so_texto = WanPipeline(tokenizer=tokenizer, text_encoder=encoder, vae=None, scheduler=None, transformer=None)
        with torch.no_grad():
            pos, neg = so_texto.encode_prompt(
                prompt=prompt,
                negative_prompt=negativo,
                do_classifier_free_guidance=True,
                max_sequence_length=512,
                device=torch.device("cuda"),
                dtype=dtype,
            )
        del so_texto, encoder
        gc.collect()
        torch.cuda.empty_cache()
        return pos, neg

    def _gerador(self) -> Any:
        if self._pipe is not None:
            return self._pipe
        import torch
        from diffusers import AutoencoderKLWan, WanPipeline, WanTransformer3DModel
        from diffusers.schedulers.scheduling_unipc_multistep import UniPCMultistepScheduler

        dtype = _dtype()
        transformer = WanTransformer3DModel.from_pretrained(self.m.repo, subfolder="transformer", torch_dtype=dtype, device_map="cuda")
        vae = AutoencoderKLWan.from_pretrained(self.m.repo, subfolder="vae", torch_dtype=torch.float32, device_map="cuda")
        pipe = WanPipeline.from_pretrained(self.m.repo, transformer=transformer, vae=vae, text_encoder=None, tokenizer=None, torch_dtype=dtype)
        pipe.scheduler = UniPCMultistepScheduler.from_config(pipe.scheduler.config, flow_shift=self.m.flow_shift)
        pipe.vae.enable_tiling()
        lora = os.environ.get("CENA_LORA")
        if lora:
            pipe.load_lora_weights(lora)
        self._pipe = pipe
        return pipe

    def _liberar_gerador(self) -> None:
        import torch

        self._pipe = None
        gc.collect()
        torch.cuda.empty_cache()

    def __call__(self, *, prompt: str, negative_prompt: str, **kwargs: Any) -> Any:
        import torch

        vram = torch.cuda.get_device_properties(0).total_memory / 1024**3
        # O 5B (~10 GB) e o codificador de texto (~11 GB) não cabem juntos numa GPU de 15 GB.
        if self.m.vram_ideal_gb > 16 and vram < 26:
            self._liberar_gerador()
        pos, neg = self._codificar(prompt, negative_prompt)
        return self._gerador()(prompt_embeds=pos, negative_prompt_embeds=neg, **kwargs)
