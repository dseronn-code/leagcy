"""O "diretor": transforma um pedido simples num prompt de cinema realista.

Modelos de vídeo respondem muito melhor a linguagem de câmera (lente, luz,
movimento) do que a descrições soltas. Aqui ficam os presets de estilo e o
prompt negativo que afasta o "visual de IA" (pele de plástico, mãos erradas,
cara de 3D).
"""

from dataclasses import dataclass

REALISMO = (
    "photorealistic live-action footage, real people, natural skin texture with visible pores, "
    "anatomically correct hands, physically accurate lighting and shadows, realistic motion, "
    "shallow depth of field, subtle film grain, natural color grading"
)

ESTILOS: dict[str, str] = {
    "cinema": "cinematic shot on a cinema camera, 35mm anamorphic lens, soft key light, gentle camera movement",
    "comercial": "premium commercial, clean composition, soft natural window light, slow dolly-in, polished but natural",
    "documentario": "documentary style, handheld camera, available light, candid and unposed moment",
    "celular": "shot on a smartphone, vertical-friendly framing, handheld, everyday real life, natural daylight",
    "noturno": "night scene, practical lights, neon and street lamps, high dynamic range, cinematic low light",
    "drone": "aerial drone shot, smooth forward motion, golden hour, wide landscape, realistic atmosphere",
}

MOVIMENTOS: dict[str, str] = {
    "auto": "",
    "parada": "static locked-off camera",
    "aproximando": "slow push-in towards the subject",
    "afastando": "slow pull-back revealing the scene",
    "acompanhando": "tracking shot following the subject",
    "orbitando": "slow orbit around the subject",
    "panoramica": "slow pan from left to right",
}

# Base: o prompt negativo recomendado pelos autores do Wan, mais o que denuncia vídeo de IA.
NEGATIVO = (
    "bright tones, overexposed, static, blurred details, subtitles, style, works, paintings, images, "
    "overall gray, worst quality, low quality, JPEG compression residue, ugly, incomplete, extra fingers, "
    "poorly drawn hands, poorly drawn faces, deformed, disfigured, misshapen limbs, fused fingers, "
    "still picture, messy background, three legs, many people in the background, walking backwards, "
    "cartoon, anime, 3d render, cgi, illustration, plastic skin, waxy skin, doll-like face, "
    "text, watermark, logo, morphing, flickering, distorted face"
)


@dataclass(frozen=True)
class Direcao:
    prompt: str
    negativo: str


def dirigir(pedido: str, estilo: str = "cinema", movimento: str = "auto", extra_negativo: str = "") -> Direcao:
    """Monta o prompt final. O pedido do usuário vem primeiro: é o que mais pesa para o modelo."""
    pedido = " ".join(pedido.split())
    if not pedido:
        raise ValueError("Descreva a cena que você quer ver.")
    if estilo not in ESTILOS:
        raise ValueError(f"Estilo desconhecido: {estilo}. Opções: {', '.join(ESTILOS)}")
    if movimento not in MOVIMENTOS:
        raise ValueError(f"Movimento desconhecido: {movimento}. Opções: {', '.join(MOVIMENTOS)}")
    partes = [pedido.rstrip("."), ESTILOS[estilo], MOVIMENTOS[movimento], REALISMO]
    negativo = NEGATIVO + (f", {extra_negativo.strip()}" if extra_negativo.strip() else "")
    return Direcao(prompt=". ".join(p for p in partes if p) + ".", negativo=negativo)


def quadros_para(segundos: float, fps: int) -> int:
    """O Wan exige número de quadros no formato 4k+1 (ex.: 49, 81, 121)."""
    k = max(1, round(segundos * fps / 4))
    return 4 * k + 1
