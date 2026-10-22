# JBDS — "Raio de 5 km" (comercial-paródia, 52 s)

> **Peça de humor sobre um produto fictício.** Nenhuma caixa de som real faz isso, e o filme termina com essa ressalva na tela.

**Formato:** 16:9, 1920×1080, 30 fps · trilha original de trailer épico (*braams*, batidas, silêncio), gerada por código.
**Tom:** trailer de blockbuster com aviso legal de remédio. O absurdo é dito com seriedade total, porque a piada está no contraste.

## O produto (briefing + invenções)
| Do briefing | Invenção para o filme |
|---|---|
| O áudio mais insano e o mais alto já feito | Grave registrado por sismógrafo (2,1 na escala Richter) |
| Só pode ser usada dentro de sala com isolamento acústico | Agudo que trinca taças a 300 m |
| Ninguém pode estar num raio de 5 km | Botão de volume que vai até ∞ |
| Quem estiver por perto e sem o fone fica surdo | Radar de pessoas embutido, que varre os 5 km antes de ligar |
| O fone que vem junto abaixa a caixa e aumenta a conversa | Fone **JBDS Pods**: "Abaixa a caixa. Aumenta a conversa." |
| | Termos de uso lidos em 6 s ("não use perto de aeroportos, baleias ou do seu vizinho") |
| | Pesa 1,2 tonelada, com alça inclusa; precisa de autorização da companhia de energia |
| | O próprio filme "ensurdece" o espectador: depois do volume máximo só sobra um zumbido |

## Decupagem

| Tempo | Imagem | Texto | Som |
|---|---|---|---|
| 0:00–0:05 | Preto. Uma silhueta em contraluz, quase invisível. | "Algumas coisas não foram feitas para todo mundo." | Drone grave, batimento cardíaco. |
| **0:05** | Recorte de luz vermelha revela o contorno da caixa. | "Esta é uma delas." | **BRAAAM** nº 1. |
| **0:08** | Produto inteiro, cones pulsando; a tela treme. | **JBDS** | **BRAAAM** nº 2 + impacto. |
| 0:12–0:14 | Linha de sismógrafo enlouquece. | "Grave registrado por sismógrafos." · *2,1 Richter* | Tambor + sub. |
| 0:14–0:16 | Taça de cristal trinca. | "Agudo que trinca cristal a 300 m." | Tambor + estalo agudo. |
| 0:16–0:18 | Botão de volume gira: 10, 11, 12… ∞. | "Volume: vai até ∞." | Tambor + *riser*. |
| 0:18–0:24 | Ficha de "requisitos" com carimbos. | "Para usar a JBDS legalmente, você precisa de: ① uma sala com isolamento acústico ② ninguém num raio de 5 km." | Ostinato tenso. |
| 0:24–0:30 | Radar varre o mapa e encontra alguém. | "Pessoas detectadas: 1 · Sr. Antônio, 4,2 km · sem fone" → "Ele vai ficar surdo." → "A não ser que…" | Bipes de radar, pausa dramática. |
| **0:30–0:36** | Revelação do fone; dois medidores: caixa ↓, conversa ↑. | **JBDS Pods** · "Abaixa a caixa. Aumenta a conversa." | Volta a batida. |
| 0:36–0:42 | Letras miúdas rolando rápido demais para ler. | "Termos de uso" (lista absurda). | Ostinato acelera. |
| 0:42–0:44 | Contagem 3, 2, 1 e a mão gira o botão até o fim. | "3 · 2 · 1" | Bipes da contagem. |
| **0:44** | Branco total, a tela racha e treme. | — | **O impacto mais forte do filme** e depois quase nada: só um zumbido agudo e abafado. *Você* ficou surdo. |
| 0:46–0:52 | Assinatura. | **JBDS** · "Ouça uma vez. Lembre pra sempre." · "Fone JBDS Pods incluso. Seus ouvidos, não." · *Produto fictício. Peça de humor.* | *Braam* final distante, cauda longa. |

Para renderizar: `python3 music.py && node render.mjs`, que gera `out/jbds-trailer.mp4`. Use `node render.mjs --stills` para os quadros-chave.
