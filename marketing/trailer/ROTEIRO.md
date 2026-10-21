# Sonora — "A Interrupção" (filme de lançamento, 48 s)

**Formato:** 16:9, 1920×1080, 30 fps · trilha original a 120 BPM (1 compasso = 2 s), gerada por código.
**Referências de linguagem:** comerciais da Apple que nunca citam o rival e deixam o público decodificar a metáfora (o "your store" contra a App Store, os retratos do "your phone", a série *Get a Mac*), os teasers do HomePod feitos só de tipografia animada pela música, e o contraste silêncio → drop dos grandes filmes de produto.

## A cutucada (e o limite dela)
O alvo é **a categoria, não uma marca**: ninguém é nomeado, não aparece logo, cor ou interface de ninguém. Só se cutucam coisas **verdadeiras** que todo mundo já viveu:
1. **A música para no meio para entrar um anúncio.** A cena reproduz a experiência com uma "pílula" cinza genérica: *"Sua música volta em 0:30"*.
2. **O resumo do ano que só chega em dezembro.** *"Tem gente que espera o ano inteiro pra saber o que ouviu."*

Isso segue a regra do CONAR para comparação implícita: nada de enganoso e nada que só sirva para denegrir.
**O que o filme não promete:** "todas as músicas do mundo" ou "melhor que X". O Sonora toca do seu acervo e de fontes legais, então o texto fala de *como* você ouve, não de *quanto catálogo* existe.

## Decupagem

| Tempo | Imagem | Texto na tela | Som |
|---|---|---|---|
| 0:00–0:06 | Preto. Partículas de luz respirando no ritmo de um arpejo de piano. | "Você conhece esse momento." / "A música crescendo." / "O refrão chegando…" | Piano em lá menor + pad subindo num *riser*. |
| **0:06** | **CORTE SECO.** Tudo congela e perde a cor. | Pílula cinza genérica: "Anúncio · Sua música volta em 0:30", com contagem. | **Silêncio total.** Só o tique do relógio a cada segundo. |
| 0:09–0:12 | A pílula racha e se desfaz em pó. | "A gente também conhece." → "Por isso tiramos." | Pad volta de trás para frente, crescendo até o drop. |
| **0:12** | Explosão de onda sonora; nasce o logotipo. | **SONORA** | **DROP**: bumbo, baixo, acordes. |
| 0:14–0:20 | Tipografia gigante batendo no bumbo. | "Zero anúncios." / "Zero interrupções." / "Zero." | Groove completo. |
| 0:20–0:26 | Uma onda "em escadinha" (baixa resolução) vira uma curva perfeita; os números sobem. | "16-bit → 24-bit" · "44,1 kHz → 192 kHz" · "Ouça o que o estúdio ouviu." | Entra um arpejo brilhante agudo. |
| 0:26–0:32 | Uma folha de calendário passa até dezembro e é arrancada; aparecem contadores e barras ao vivo. | "Tem gente que espera o ano inteiro pra saber o que ouviu." → "Aqui é agora." · "12.480 min · 42 dias seguidos" | Respiro: some o bumbo e o filtro fecha; o bumbo volta no fim. |
| 0:32–0:38 | Um campo de link genérico recebe um "colar"; as faixas descem em cascata para a lista. | "Cola o link da sua playlist." / "Ela vem. E continua vindo." | Groove volta, com um *sweep* na transição. |
| 0:38–0:42 | Duas ondas se emendam sem nenhum vão. | "Sem silêncio entre as faixas." | A virada emenda direto no acorde final. |
| **0:42–0:48** | Preto; logotipo, slogan e assinatura. | **Sonora** · "Ouça do jeito que foi gravado." · "Sem anúncios. Nunca." | Acorde final aberto com cauda longa; impacto grave no logo. |

## Arquivos
- `music.py`: compõe e mixa a trilha (numpy, sem samples nem áudio de terceiros).
- `film.html`: o filme inteiro como função do tempo `render(t)`, em canvas.
- `render.mjs`: o Chromium renderiza quadro a quadro, e o ffmpeg junta imagem e trilha em MP4 (H.264 + AAC).

Para renderizar: `python3 music.py && node render.mjs`, que gera `out/sonora-trailer.mp4`.
