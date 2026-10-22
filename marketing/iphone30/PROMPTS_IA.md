# iPhone 30 — "Um dia": versão hiper-realista com IA de vídeo

Use com geradores de vídeo realista: **Google Veo 3**, **OpenAI Sora 2**, **Kling 2.x**, **Runway Gen-4** ou **Luma Ray**.
Os prompts estão em inglês porque esses modelos respondem melhor assim. Cada tomada dura de 4 a 8 s, em 16:9 e 24 fps.

## Como ficar indistinguível de filmagem real

1. **Mesmo personagem em todas as tomadas.** Cole o bloco `PERSONAGEM` no início de *todo* prompt. Se a ferramenta aceitar imagem de referência, como "character reference" ou "ingredients", gere primeiro uma foto do personagem e use-a em todas as tomadas.
2. **Linguagem de câmera real.** Lente, foco, luz e granulação de filme são o que mais "vende" realismo; os prompts já trazem isso.
3. **Não deixe a IA escrever na tela do celular.** Os modelos distorcem textos e interfaces. Peça a tela do celular **verde chapada** (já está nos prompts) e substitua na edição pelas telas prontas deste repositório. As animações de interface do `film.html` (alarme, café, bateria, satélite, tradução, carga, cinema noturno) podem ser exportadas só como tela para fazer esse encaixe.
4. **Gere de 3 a 4 versões de cada tomada** e fique com a melhor. Mãos e dedos são onde a IA mais erra: descarte as tomadas com dedos estranhos.
5. **Sem logo da maçã nem marcas reais.** Além do risco legal, logos gerados por IA costumam sair deformados e entregam que é falso.

## Blocos fixos (copie em todos os prompts)

```
PERSONAGEM: a Brazilian man in his early 30s, warm light-brown skin, short dark curly hair, short well-groomed beard, friendly face, slim build.
ESTILO: shot on a cinema camera, 35mm anamorphic lens, shallow depth of field, natural light, subtle film grain, Apple-style premium commercial, realistic skin texture, no text, no logos.
CELULAR: a modern smartphone with a thin brushed-titanium frame and edge-to-edge screen, back with no logo; when the screen is visible it shows a flat solid green screen for compositing.
```

## Tomadas (sincronizadas com `out/trilha.wav`)

| # | Tempo | Prompt (depois dos blocos fixos) |
|---|---|---|
| 1 | 0:00–0:03 | `Blue pre-dawn bedroom. Extreme close-up top-down of the smartphone lying on a wooden nightstand, screen lighting up as an alarm. A hand reaches in slowly and taps the screen once. Static camera, soft blue ambient light, the screen glow lighting the wood.` |
| 2 | 0:03–0:06 | `The man sits up in bed and stretches his arms high, smiling sleepily, as sheer curtains are opened and warm golden morning sun floods the bright minimalist bedroom. Medium-wide shot, slow push-in, backlit, dust particles in the sunbeam.` |
| 3 | 0:06–0:10 | `Slow motion, 120fps look: the smartphone slides off the edge of a wooden nightstand and falls onto a hardwood floor, bounces once and lands flat, screen intact. Low angle at floor level, morning sun, shallow focus on the phone.` |
| 4 | 0:10–0:16 | `Bright Scandinavian kitchen, morning. The man in a gray t-shirt holds the smartphone up, pointing its camera at an empty glass coffee jar on the counter, then taps the screen and smiles. Over-the-shoulder shot, soft window light, steam from a kettle in the background.` |
| 5 | 0:16–0:22 | `Rainy city street in São Paulo, morning commute. The man walks under a black umbrella, raindrops bouncing, glances at the smartphone in his hand and smiles. Tracking shot at walking pace, wet reflections, cool blue-gray tones with warm shop lights.` |
| 6 | 0:22–0:28 | `Inside a moving subway car in a dark tunnel, tunnel lights streaking past the windows. The man types a message on the smartphone and sends it, a small satisfied smile. Close-up, handheld subtle movement, warm passing lights flickering across his face.` |
| 7 | 0:28–0:36 | `Bright modern office with plants. The smartphone stands on a desk stand in a video call; the man talks and laughs naturally, nodding as he listens. Medium close-up, soft daylight, shallow depth of field, colleagues blurred in background.` |
| 8 | 0:36–0:42 | `Close-up of a hand plugging a white USB-C cable into the smartphone lying on a dark walnut desk, afternoon light. Macro detail of the connector, then a slow rack focus to the screen.` |
| 9 | 0:42–0:50 | `Dark school auditorium at night, a 7-year-old girl in a pink dress dances on a warmly lit stage. In the foreground, the man holds up the smartphone recording her, his face lit softly by the screen, proud and emotional. Over-the-shoulder, very low light, cinematic.` |
| 10 | 0:50–0:55 | `Dark bedroom at night, blue moonlight. The man places the smartphone face up on the wooden nightstand; the screen dims. He turns off the bedside lamp. Top-down static shot, then darkness.` |
| 11 | 0:55–1:00 | Fundo preto com a assinatura "iPhone 30 · Feito para o seu dia.": use o final já renderizado do `film.html` (sem IA). |

## Montagem

1. Importe as tomadas no **CapCut**, **DaVinci Resolve** (gratuito) ou **Premiere**, na ordem da tabela.
2. Coloque `out/trilha.wav` como áudio. Os cortes já caem nos tempos da tabela: a música entra exatamente aos 3 s, quando ele levanta.
3. Substitua as telas verdes pelas telas do `film.html` (chroma key + "corner pin"/rastreamento).
4. Correção de cor leve e uniforme em todas as tomadas, para parecerem da mesma filmagem.
5. Mantenha a cartela final com a ressalva.

## Atenção antes de publicar

Um comercial realista de um "iPhone 30" pode ser compartilhado como se fosse um anúncio oficial vazado. Para evitar problemas:
- **Mantenha a ressalva** "Filme-conceito de fã, não afiliado à Apple" na cartela final.
- **Marque como conteúdo gerado por IA**: YouTube, TikTok e Instagram exigem essa marcação em vídeos realistas feitos com IA, e podem remover o vídeo ou punir a conta se não estiver marcado.
- Não use o logo da maçã nem o nome "Apple" como se fosse a autora do filme.
