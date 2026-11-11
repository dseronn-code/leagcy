# eFootball (mobile, Classic): jogabilidade "formiga a formiga" — relatório v2

Data: 2026-11-11. Versão atual do jogo: v6.1.0 ("eFootball 2027"). As notas v4.x (2025), v5.x (2026) e v6.0 (2027) foram lidas na íntegra no site da Konami.

## 0. Como esta pesquisa foi feita e confiabilidade

- **Acesso:** a ferramenta WebFetch continuou falhando (erro de DNS) para todos os domínios. Mas `curl` pelo proxy da sessão funcionou, então as páginas foram baixadas e lidas **na íntegra** em HTML. Cópias brutas: `/tmp/claude-0/-home-user-leagcy/8e3314e2-3925-5d4e-abea-f1e544d074da/scratchpad/pages/`.
- **Lido com sucesso:** konami.com (manual de controles mobile, página "new_controls", notas v4.0–v4.4, v5.0–v5.4, v6.0, v6.1), dds.konami.com (manual PES 2019), efootballexplained.com (todas as 29 páginas de atributos, 15 skills, 10 playing styles, página de mitos com 454 itens, changelog), pesmastery.com (tutoriais de drible, passe, chute, defesa e goleiro), fifplay.com, gamingonphone.com, efootballlab.com, gamemarket.gg, footymeta.com, ef-labo.com.
- **Não legível:**
  - efootballedge.com e efootball.cz são aplicações JavaScript. O HTML vem vazio ("Loading article…"), então o conteúdo não foi lido.
  - efootbase.com devolveu 403.
  - A imagem da legenda do controle no site da Konami (MobileController_en.png) redireciona para um host bloqueado. A numeração dos botões foi deduzida cruzando com o fifplay (ver 7.2).
- **Fonte principal de números: efootballexplained.com.** É engenharia reversa do código do jogo, "Checked on game version 11.1.1 (2026-10)", e não é afiliada à Konami. Ela dá fórmulas e valores internos que a Konami não publica. Os números de lá são marcados **[EE]**. As notas oficiais são marcadas **[KONAMI]**. Guias de fãs são marcados **[GUIA]** (baixa confiabilidade para números).
- **[ESTIMATIVA]** marca proposta minha para implementação. Não vem de fonte.
- **Unidade de tempo:** segundo o EE, a partida do eFootball atualiza **27 vezes por segundo** (1 tick = 0,037 s). Todos os valores em "ticks" abaixo são ticks de 27 Hz. Para o seu motor de 30 Hz, converta pelo tempo em segundos, não pela contagem de ticks: ticks30 = round(segundos × 30). Fonte: https://efootballexplained.com/stats/

---

## 1. CONDUÇÃO E CONTATO CORPO-BOLA (o ponto da reclamação do usuário)

### 1.1 Modelo geral: a bola é tocada em eventos discretos
O jogo não "cola" a bola no pé. O condutor dá **toques** em instantes discretos. Entre um toque e outro, a bola rola livre e o jogador corre atrás dela. Isso aparece em várias fontes:
- **Dois critérios para tocar de novo [EE]:** há "o timing de cada toque" e "o momento em que a bola conta como perto o bastante para ser tocada de novo". Nenhum dos dois usa Dribbling. Fonte: https://efootballexplained.com/stats/drib/
- **Distância entre toques [EE]:** Dribbling não define a distância que a bola vai à frente a cada toque. Mas, como Dribbling aumenta a velocidade com bola, a bola anda mais entre os toques. Ou seja, a distância bola–pé é consequência de velocidade × intervalo, e não um parâmetro fixo.
- **Bola exposta entre toques [GUIA]:** "quando o atacante corre, a bola vai mais longe à frente a cada toque, e há um momento visível em que ele não está em contato com ela". É a janela do desarme. Fonte: https://gamemarket.gg/news/efootball/efootball-defending-guide-pressure-match-up-and-tackling
- **Defensor ataca a bola, não o corpo [EE]:** o desarme de longo alcance só usa uma versão extra quando a bola está a ≥1,2 m do condutor (Aggression 40), 0,2 m (Aggression 99) ou cerca de 0 (110). O alcance de início desse desarme vai de 2 m (Aggression ≤60) a 5 m (Aggression 95). Fonte: https://efootballexplained.com/stats/aggr/
- **Bugs que a Konami corrigiu, todos sobre a relação corpo-bola [KONAMI]:**
  - v4.4: "jogador não consegue tocar a bola ao tentar driblar" e "jogador se separa da bola a uma distância maior que o esperado".
  - v5.2: "tocar a bola antes do timing correto fazia ela ser chutada longe demais".
  - v5.4: "reduzir casos em que a bola bate sem querer no corpo ou nas pernas"; "colisão jogador-bola era um pouco maior que o visual"; "a bola atravessava o jogador ou fazia contato repetido com o corpo".
  - v6.0: "durante uma finta com vários toques, a bola fica no lugar errado e o jogador erra totalmente ao tentar tocá-la de novo"; "o condutor está prestes a passar da bola (overrun) mas continua se movendo como se encarasse o adversário — agora essas animações são evitadas ou canceladas".
  - Fontes: https://www.konami.com/efootball/en/page/v4/versioninfo_v4-40 , https://www.konami.com/efootball/en/page/v5/versioninfo_v5-20 , https://www.konami.com/efootball/en/page/v5/versioninfo_v5-40 , https://www.konami.com/efootball/en/page/v6/versioninfo_v6-00

**Conclusão para implementar:** o condutor precisa **perseguir a bola** e redirecioná-la só nos toques. O joystick define a direção do **próximo** toque, não a da bola em si. O raio de virada "natural" sai disso.

### 1.2 Intervalo entre toques (Tight Possession) — números [EE]
Fonte: https://efootballexplained.com/stats/tp/

| Situação | Tight Possession 40 | 60 | 80 | 90 | 99 | 120 (booster) |
|---|---|---|---|---|---|---|
| Intervalo **mínimo** entre dois toques | 0,23 s | 0,229 | 0,200 | 0,185 | 0,180 | 0,167 |
| Condução lenta **em linha reta** (abaixo de 20 km/h ≈ 5,56 m/s), intervalo fixo, qualquer stat | 0,5 s | 0,5 | 0,5 | 0,5 | 0,5 | — |
| Viradas de 45–67° | 0,49 s | — | — | — | 0,40 s | — |

- **Ritmo nas viradas:** acelera a partir de **22,5°** de mudança de direção. Para ≥45°, em qualquer velocidade, o multiplicador de ritmo vai de **×1,02 (TP 40) a ×1,20 (TP 99)**, e ×1,24 a 120.
- **Velocidade das animações de toque:** algumas tocam mais rápido com a média de Acceleration e TP: **95% da velocidade normal na média 40, 104% na 80, 115% na 99.** Boosters não contam aqui.
- **Fadiga:** com a reserva de sprint ≤20%, essas animações usam TP e Acceleration até 20 pontos mais baixos (1 ponto por % abaixo de 20).
- **Tight Possession não muda a velocidade máxima.**
- **Ganho por ponto:** −0,3 ms (40–59), −1,5 ms (60–89), −0,6 ms (90–120).
- **Ball Control não entra na condução normal** (velocidade, timing e planejamento de toques). Só entra nas fintas e no domínio. Fonte: https://efootballexplained.com/stats/bc/

### 1.3 Velocidade com bola (Speed × Dribbling) [EE]
- **Fórmula:** velocidade máxima com bola = **26,5 + 6 × √(score_Speed × score_Dribbling) − 1,8 km/h**. Num "estado de drible" específico, o desconto é só 0,75 km/h, o que dá cerca de +1 km/h. O controle que ativa esse estado não é conhecido. Fontes: https://efootballexplained.com/stats/speed/ , https://efootballexplained.com/stats/drib/
- **Sem bola:** 26,5 + 6 × score_Speed km/h. Isso dá 27,0 (Speed 40), 27,6 (60), 30,5 (80), 31,9 (90), 32,5 (99), 33,8 (120).
- **O "score" (minha reconstrução a partir das tabelas do EE):** é uma curva 0..1 com duas dobras:
  - 40 → 0,083; 60 → 0,183; 80 → 0,667; 90 → 0,90; 99 → 1,0.
  - Inclinações de cerca de 0,0045/ponto abaixo de 60, 0,0242/ponto de 60 a 89 e 0,0102/ponto de 90 em diante.
  - A mesma curva de duas dobras (60 e 90) vale para Speed, Acceleration, Dribbling, Tight Possession e os 4 atributos de precisão. Fonte: https://efootballexplained.com/stats/
- **Exemplos com bola:**

  | Speed / Dribbling | Velocidade máxima com bola | Em m/s |
  |---|---|---|
  | 90 / 70 | 28,4 km/h | 7,9 m/s |
  | 80 / 80 | 28,7 km/h | 7,97 m/s |
  | 90 / 90 | 30,15 km/h | — |
  | 99 / 99 | 30,7 km/h | 8,53 m/s |

  O mais fraco dos dois atributos limita.
- **Aceleração com bola:**
  - Usa só Acceleration (igual a sem bola).
  - O planejamento dos toques calcula um "pick-up" com √(score_Acc × score_Drb), mas o efeito real é desconhecido [EE].
  - **[KONAMI v5.0]:** "ajustado para que, em baixa velocidade, os jogadores acelerem rápido e façam movimentos ágeis; em velocidade alta, acelerem gradualmente". E "Dribbling e Acceleration afetam o quão rápido o jogador acelera conduzindo".
- **[KONAMI v6.0]:**
  - "Dash Dribble rápido demais e difícil de defender: velocidade máxima levemente reduzida."
  - "Na condução lenta, mudança na forma de chutar a bola e na velocidade das animações para que o jogador se mova um pouco mais rápido."
  - "Mais fácil usar o pé bom durante o drible."
  - "Ao virar durante o drible, os jogadores andavam em velocidades diferentes conforme a animação: ficou mais estável."
  - "Na condução lenta, o corpo virava demais para a direção do movimento: corrigida a direção para a qual o corpo aponta."
- **Altura:** jogadores com ≤165 cm fazem fintas até cerca de 5% mais rápido que jogadores com ≥195 cm. Os domínios deles também são mais rápidos. Altura e peso não entram em velocidade nem aceleração [EE].

### 1.4 Reserva de sprint com bola ("pernas cansadas") [EE]
Fonte: https://efootballexplained.com/stats/stamina/ , https://efootballexplained.com/skills/double-touch/
- **Como funciona:** o portador tem uma reserva curta, separada da stamina.
  - **Correr com bola acima de 22 km/h (6,1 m/s) esvazia a reserva cheia em cerca de 3 s** (2,8 s).
  - Ela chega a 80% após cerca de 0,6 s de corrida rápida.
  - **Recarga:** cerca de 7 s sem bola, ou cerca de 12 s conduzindo devagar.
  - Jogador cansado tem a reserva menor (encolhe com a stamina restante, mínimo 1/5).
- **Efeitos:**
  - Reserva ≤80%: **fintas com Dribbling −25.** Reserva vazia: −50.
  - Reserva <20%: Finishing, Low Pass, Lofted Pass e Kicking Power podem perder até 5 pontos, e os toques ficam mais lentos.
  - Reserva <80%: o tanque principal drena mais rápido (cerca de +20% com ela vazia).
- **Na prática:** dar Dash o tempo todo com bola é penalizado em segundos. Isso empurra para o ritmo "lento → explosão" (que é o que a Konami descreve no Sharp Touch).

### 1.5 Comandos de condução (Classic mobile e controle)
- **Condução normal:** joystick. **Dash Dribble:** joystick + segurar Dash.
- **Sharp Touch:**
  - Toque forte seguido de arrancada. No mobile Classic: "Flick while pressing Dash" (dar o flick no joystick enquanto Dash está pressionado). No controle: apertar R2 até o fundo e soltar dirigindo o stick esquerdo.
  - [KONAMI v4.0] Velocidade aumentada no Sharp Touch para frente durante o Dash Dribble.
  - [KONAMI v5.4] Viradas mais bruscas, fim da bola "flutuando", empurrão de bola consistente com ângulo e velocidade, e removidos os movimentos de "circular o adversário".
  - Fontes: https://www.konami.com/efootball/en/page/new_controls , https://www.fifplay.com/efootball-2027-controls/
- **Quick Stop:** soltar o stick e segurar Dash (controle). No mobile, toque duplo no lado esquerdo da tela. Fonte: https://www.konami.com/efootball/en/page/mobile_controller , https://www.konami.com/efootball/en/page/new_controls
- **Finesse Dribble (v4.0):**
  - Toques finos mantendo o corpo voltado para o gol. No controle: L2 + stick (⑩+⑧).
  - Com Smart Assist, perto da linha lateral ele evita deixar a bola sair. [KONAMI]
  - **Não achei o gesto do mobile Classic.**
  - [KONAMI v6.0] Apertar Dash + Finesse juntos ao dominar = domínio seguido de arrancada.
- **Shield (proteção):** removido na v4.0. O jogador protege automaticamente conforme a situação, e Physical Contact pesa. Antes era "double tap left side while pressing Dash" (2022). Fonte: https://www.konami.com/efootball/en/page/v4/versioninfo_v4-00
- **Virada rápida no Dash:** [KONAMI v4.0] melhor resposta em viradas de 180° durante o Dash Dribble. [KONAMI v5.4] novas animações para mudança de direção mais rápida em Dash e Sharp Touch.
- **Taxa de rotação:** [EE] "a taxa de rotação é fixa e igual para todos os jogadores". Acceleration só muda o quão rápido ele volta a ganhar velocidade depois da mudança de direção. A frenagem muda no máximo cerca de 5%, e animações de virada de ≥89°, partida e parada ficam cerca de 1,5% mais rápidas (40→99). **O valor da taxa não foi publicado.** Fonte: https://efootballexplained.com/stats/accel/

### 1.6 Fintas (feints)
- **Tipos de comando (v5.4):**
  - **Easy:** comandos simples.
  - **Smart:** o padrão. A finta é escolhida pelo jogo conforme a situação, as skills e a direção.
  - **Advanced:** cada input dá uma finta específica.
  - Menu: [Extras] > [Game Settings] > [Play Settings] > [Feint Command Type]. Fonte: https://www.konami.com/efootball/en/page/v5/versioninfo_v5-40
- **Gestos no mobile Classic** [GUIA, gamingonphone, set/2026]. As direções são relativas ao movimento do jogador:

  | Finta | Gesto |
  |---|---|
  | Marseille | deslizar Dash para frente e depois 90° para o lado (variantes: lado→frente, frente→diagonal oposta 135°) |
  | Scissors | girar em volta do Dash em arco para frente ou para trás |
  | Double Touch | tocar Dash e dar flick lateral 90° (outra fonte: flick na direção em que o corpo aponta) |
  | Draw and Open (L Feint) | deslizar Dash para trás e depois para o lado |
  | Draw and Close / Scotch | deslizar Dash para trás e depois para frente |
  | Flip Flap | tocar Dash e dar flick diagonal 45° |
  | Sombrero / Rainbow | tocar Dash 3 vezes conduzindo |
  | Sombrero no domínio | tocar Dash antes de dominar e deslizar |

  Fontes: https://gamingonphone.com/guides/efootball-a-guide-to-perform-feints-and-sombrero/ , https://gamingonphone.com/guides/efootball-2024-a-complete-dribbling-guide-to-learn-the-fundamentals-and-skill-moves
- **Smart Assist + finta:** inclinar o stick direito (no controle) para o lado da evasão. Fonte: https://www.konami.com/efootball/en/page/overview
- **Números das fintas [EE]:**
  - **Velocidade de execução com Dribbling** (jogador de 180 cm): 102% (40), 117% (99), 122% (120).
  - **Skill da finta:** o dono conta Dribbling +20. Quem não tem a skill numa finta "básica" (Marseille, double touch, chop, flip flap, corte básico, sombrero) conta −20. A diferença entre dono e não-dono é de cerca de 10% de velocidade. A tesoura simples não tem penalidade.
  - **Janela de encaixe do toque da finta na bola:** 1 a 3 ticks (0,04–0,11 s), crescendo com Ball Control.
  - **Bola rápida:** se a bola está acima de 36 km/h no toque da finta, o jogo pode tratar o toque de outro jogo. A 50 km/h a chance é de 86% com BC 40 e 59% com BC 99. Uma bola que sobe ou desce a mais de 7,5 km/h sempre dispara isso.
  - **Dribbling muito baixo:** Dribbling 40 adiciona uma espera de 0,05 s no planejamento do drible (0,10 s num modo). Em 99 não há espera.
  - **Animações restritas:** três animações especiais exigem Dribbling de carta ≥80. Algumas de domínio e truque exigem BC de carta ≥79, 84 ou 85.
  - **Sole Control:** a "sole double touch" só sai com o adversário a ≤4 m, dentro de 30° à frente.
  - **Acceleration Burst:** Sharp Touch especial com o stick a 15,5–120° da direção do corpo, só em velocidade lenta (até cerca de 16 km/h com a skill, cerca de 11 km/h sem). Abaixo de 20 km/h, toques 0,006 s mais cedo e aceleração ×1,025.
  - Fontes: https://efootballexplained.com/stats/drib/ , https://efootballexplained.com/stats/bc/ , https://efootballexplained.com/skills/sole-control/ , https://efootballexplained.com/skills/acceleration-burst/
- **[KONAMI v5.4]:**
  - Marseille não pode ser feita com virada muito fechada em alta velocidade.
  - Ajustados o ângulo de saída e a distância da bola no Flip Flap e no Chop Turn.
  - Corrigida a bola chutada longe demais sem querer nas fintas.
  - Sombrero não é mais automático.

### 1.7 Contato físico, proteção e equilíbrio [EE]
Fontes: https://efootballexplained.com/stats/pc/ , https://efootballexplained.com/stats/balance/
- **Empurrão no esbarrão:**
  - Determinístico, **sem sorteio**. É dividido por: diferença de Physical Contact, diferença de velocidade e ângulo.
  - Parcela que você recebe (mesma velocidade, sem bola), pela sua diferença de PC: −30 → 80%; −10 → 60%; 0 → 50%; +15 → 35%; +30 → 20%. Isso dá cerca de 1% por ponto, e a diferença satura em ±30.
  - Com bola, a divisão é por razão. Exemplo: 80 contra 70 → você recebe 47%.
  - Quem corre a ≥13,5 km/h contra alguém a ≤5 km/h recebe até +35 pontos percentuais do empurrão.
- **Reação pesada (stagger maior):** acontece quando a diferença é ≤−10. Contato leve nunca derruba. Quedas vêm de colisões rápidas e fortes ou de Power Tackle de lado ou por trás.
- **Carga de ombro contínua sobre o portador:** ele aguenta **2,0 s** (40 pontos mais fraco), **3,0 s** (igual) e **4,0 s** (40 pontos mais forte).
- **Condutor em sprint (11–25 km/h) "aguenta" a carga e segue correndo** se a vantagem de PC for:
  - ≥+30 com a carga pela frente (<67,5°);
  - ≥+15 pelo lado (67,5–90°);
  - ≥0 por trás (≥90°).

  Além disso, Balance define o ângulo mínimo de onde a carga precisa vir: 135° (Balance 40), 112° (80), 56° (99).
- **Chutar com adversário em cima (chute ≥120 km/h):**
  - Perde 17% da velocidade com o atacante 20+ pontos mais fraco, 8,5% com os dois iguais e 0 com o atacante 20+ pontos mais forte.
  - A perda some a 75 km/h.
  - A mesma diferença dita quanto a pressão estraga a precisão.
- **Distância de rasteira:** o pé do desarme derruba você a 0,30 m (seu PC 40), 0,20 m (99) ou 0,16 m (120).
- **Balance:**
  - Tempo extra preso no stagger: até 5 ticks (0,19 s). Diminui em degraus em 75/83/89/95.
  - Sair de um empurra-empurra ombro a ombro: +2 ticks (Balance 40), +1 (41–80), 0 (81+).
  - Chance de o condutor **pular o desvio automático** (hurdle/side-step) contra um carrinho a ≤4 m: 42% (Balance 60), 12% (70), 0% (82).
  - Contra desarme em pé por trás (>135°), ele só tem o movimento evasivo se Dribbling + Balance ≥150.
- **Magnetic Feet:** +2 em Dribbling, TP, PC e Balance para cada adversário a ≤5 m (máximo +8). https://efootballexplained.com/skills/magnetic-feet/
- **[KONAMI v5.0]:**
  - "Quando o comando aponta para longe do defensor, ele tem menos chance de tentar roubar."
  - Novas animações para o adversário que chega pelo lado, "disputando sem grudar no oponente".
  - Jogadores com Balance alto passam mais fácil.
- **[KONAMI v5.4]:**
  - Condutores tropeçam ou caem menos em colisões.
  - Contato corporal desacelera menos o condutor.
  - Animações de disputa que impedem tocar a bola disparam menos logo antes do toque.
- **[KONAMI v6.0]:** animações de "face-off" quando o condutor encosta no adversário, para reduzir empurrões que parecem antinaturais e deixar claro quem ganha.

### 1.8 IA de condução (quando a CPU conduz) [EE + KONAMI]
- **Pontuação das opções:** a IA monta uma lista curta de opções de drible com uma pontuação em %. Exemplo de Mazing Run: Double Touch +20, Chop +25, corte básico +17,5, virada ≥135° +20, opção "correr" +20.
  - A pontuação sobe de 70 a 90 nos atributos que cada opção usa.
  - A "tendência de driblar" sobe com Dribbling (até +0,30) e com Speed e Acceleration (até +0,15 cada), de 70 a 90.
  - Fonte: https://efootballexplained.com/playing-styles/mazing-run/
- **Cruzamento:** chance base de a CPU cruzar = 10% com Lofted Pass ≤60, subindo até 60% em 90. Fonte: https://efootballexplained.com/stats/lop/
- **[KONAMI v4.0]:** reduzida a tendência da IA de segurar a bola demais no ataque.
- **[KONAMI v6.0]:** corrigido o atacante da IA que parava com a bola e não voltava a se mover.

---

## 2. PASSE

### 2.1 Comandos Classic mobile [KONAMI new_controls, 2022, ainda válido nos nomes]
| Botão | Toque | Flick ↑/↓ | Flick ← | Flick → |
|---|---|---|---|---|
| **Pass** | passe rasteiro | passe alto / cruzamento | Stunning Low Pass (ou Stunning Low Cross na zona de cruzamento) | Stunning Lofted Pass (ou Stunning Cross) |
| **Through** | through rasteiro | Chipped Through | Stunning Through | Stunning Chipped Through |
- **Pass-and-run (Cross Over):** depois do passe, "pull and flick on the left side" (puxar e dar flick no lado esquerdo).
- **Kick Cancel (v5.0, mobile):** toque duplo no lado esquerdo depois de comandar passe ou chute.
- **Controle (equivalentes):** 1-2 Pass = L1 + passe; Pass-and-run = R1 + passe; Kick Feint = chute → passe antes do contato; Kick Cancel = R1 + R2 antes do contato.
- Fontes: https://www.konami.com/efootball/en/page/new_controls , https://www.konami.com/efootball/en/page/mobile_controller , https://www.konami.com/efootball/en/page/v5/versioninfo_v5-00

### 2.2 Stunning Kicks — definição oficial [KONAMI]
- **Stunning Low:** rápido e no pé. "A bola tende a flutuar em distâncias longas."
- **Stunning Lofted:** mais baixo e mais rápido que o lofted normal.
- **Stunning Through:** mais rápido e termina mais perto do pé do recebedor.
- **Stunning Chipped Through:** mais rápido, trajetória um pouco mais baixa e mais perto do pé.
- **Stunning Cross:** mais baixo, mais rápido e com mais curva.
- **Stunning Low Cross:** mais rápido e com mais efeito. Quica logo à frente do recebedor.
- **Custo:** "a bola demora mais a sair do pé ao chutar parado ou conduzindo devagar" (passada maior).
- **[EE] Preparação do Stunning Shot:** 0,33 s (9 ticks) com Finishing <84 e 0,30 s com ≥84. Com Snap Strike: 0,26 s / 0,22 s (degrau em 79). https://efootballexplained.com/skills/snap-strike/

### 2.3 Velocidade, precisão e assistência [EE]
Fonte: https://efootballexplained.com/stats/lp/ , https://efootballexplained.com/stats/lop/ , https://efootballexplained.com/stats/kp/
- **Passe assistido:**
  - O jogo **planeja a velocidade com que a bola deve chegar no recebedor** e depois escala essa velocidade por Low Pass: ×0,825 (40), ×0,94 (80), ×0,98 (90), ×1,0 (99), ×1,04 (120).
  - Com assistência mínima (passe totalmente manual), quem decide é a barra de força.
  - No manual, os passes do jogador controlado **nunca saem mal batidos (mis-hit).**
- **Erro do passe rasteiro:**
  - Todo passe tem erro, e acima de uma velocidade-limite entra erro extra. O limite sobe de 40 para 50 km/h com stat alto, e o erro extra cai para 1/5.
  - Erro médio num passe duro de **70 km/h** (parado, pé bom, sem pressão): 3,00° (LP 60), 1,93° (70), 1,08° (80), 0,68° (90), 0,59° (99).
  - Num passe de 30 km/h: 1,06° (40) e 0,49° (99).
  - Low Pass também corta o erro por pressão e por passar em corrida.
- **Erro do passe longo** (80 km/h): 4,94° (LP 60), 2,31° (80), 1,84° (85), 1,50° (90), 1,31° (99).
- **Altura máxima planejada do lofted:** cerca de 7,5 m (Lofted Pass 40), 5,5 m (99), 5,0 m (120). Ajustada de −2 a +1,8 m por distância e ângulo.
- **Pé fraco:**

  | Weak Foot Accuracy (nível 1→4) | Pontos perdidos |
  |---|---|
  | Maioria dos chutes e passes | −15 / −11 / −7 / −3 |
  | Chute de peito do pé (laces) | −25 / −17 / −10 / −3 |
  | Ball Control no domínio | −20 / −13 / −6 / 0 |

  https://efootballexplained.com/stats/wfa/
- **Velocidade da animação de chute:** todo chute (menos cabeçada e acrobático) toca mais rápido com o stat de precisão daquele chute: ×0,95 (40), ×1,10 (99), ×1,135 (120). [KONAMI v4.4] confirma para Low Pass, Lofted Pass e Finishing.
- **Animações especiais:**
  - Low Pass de carta ≥85 libera uma animação para passes de 60–157° em relação à direção do corpo (com companheiro naquela direção).
  - Passes de trivela ficam desligados com LP <70.
  - Lofted Pass ≥85 libera um lançamento especial (>24 m, ≤25° à frente).
  - Weighted Pass só vale acima de 45 m.
- **Kicking Power quase não afeta passes.** Só limita a velocidade máxima de algumas animações de lançamento longo.

### 2.4 Passe de primeira (one-touch)
- **Penalidade por velocidade de chegada:** cada passe, cruzamento ou afastamento de primeira perde precisão conforme a velocidade com que a bola chega.
  - A skill One-touch Pass conta essa velocidade 10% mais baixa.
  - O passe rasteiro de primeira **começa a subir do chão** acima de certa velocidade de chegada. Low Pass permite até cerca de +5 km/h nesse limite, e a skill dá +2,5 km/h.
  - **A skill não faz o jogador soltar a bola mais cedo.**
  - Fonte: https://efootballexplained.com/skills/one-touch-pass/
- **[KONAMI v6.0]** Corrigida a queda frequente de precisão em chute de primeira com a bola rápida.

### 2.5 O que o recebedor faz [KONAMI]
- **v6.0 (2027):**
  - "Jogadores ficam mais propensos a receber o passe numa posição que leva em conta os adversários, por exemplo **movendo-se em direção à bola quando há um oponente perto**."
  - "Jogadores se moviam rápido demais ao receber, dificultando o domínio: ajustada a velocidade de movimento ao receber o passe."
  - "Revisado o julgamento do ponto de quique para receber bola quicando onde é mais fácil dominar."
- **v4.4:** "jogadores que se oferecem para receber assumem posições mais adequadas e evitam mudanças bruscas de direção difíceis de perceber".
- **v5.0:** corrigido o apoiador escondido atrás do defensor. Meias recuados sobem para apoiar na construção.
- **v5.4:**
  - Revisada a escolha do jogador de apoio no "Pass Support": o mais próximo do portador continua apoiando.
  - Corrigido o recebedor que se movia de costas para o passador.
- **Atraso de reação ao passe, valendo para o time inteiro [EE]:**
  - Depois de um passe, a IA do time escala alguns jogadores (não o recebedor) para reagir. Eles reagem com atraso base de **0,4 a 0,8 s** conforme a posição, até 80% mais cedo com Defensive Engagement 99.
  - Todo jogador do time que passou ganha até +0,1 s de atraso com Offensive Awareness ≤50 (0 em 99).
  - Defensores ganham até +0,1 s com Defensive Awareness ≤50.
  - Fontes: https://efootballexplained.com/stats/de/ , https://efootballexplained.com/stats/oa/ , https://efootballexplained.com/stats/da/
- **Cursor [KONAMI v5.0]:** depois de um cruzamento, "o cursor agora fica no alvo do passe na maioria dos casos". [v5.4] Corrigida a troca de cursor para um jogador que não era o alvo depois de um passe errado.

### 2.6 Passes que "puxam" corrida (1-2 e pass-and-run)
- **1-2 (PES/eFootball, controle):**
  1. L1 + passe → o passador arranca para frente.
  2. Through antes de o recebedor dominar → devolução de primeira à frente do passador.
  3. Os jogadores voltam aos poucos à posição.
  - Fonte: https://pesmastery.com/efootball-passing-tutorial/
- **Pass-and-run (Cross Over):** o passador faz uma corrida diagonal cruzando com o recebedor. [KONAMI 2022]
- **[KONAMI v4.4]:** no 1-2, o corredor continua em Dash mesmo que vá ficar impedido por pouco.
- **Corrida principal da IA [EE]:**
  - Quando o portador inicia certas ações, a IA manda **1 a 3** jogadores até a linha de impedimento.
  - Elegíveis: quem está a ≤18 m da bola no sentido do campo (≤23 m se estiver nivelado lateralmente, a ≤12 m; ≤24 m para Goal Poacher).
  - Esse limite cresce com o Lofted Pass do portador (carta 75–95, até +18 m).
  - **Espera antes de um companheiro poder ser escolhido para corrida:** 27 ticks (1,0 s) com OA 40, 17 (60), 8 (80), 4 (90), 0 a partir de 97.
  - Corridas de apoio: 9 ticks (0,33 s) com OA 40, 0 a partir de 93.
  - Fontes: https://efootballexplained.com/playing-styles/goal-poacher/ , https://efootballexplained.com/stats/oa/

---

## 3. RECEPÇÃO, DOMÍNIO E BOLA SOLTA

### 3.1 Domínio [EE + KONAMI]
Fonte: https://efootballexplained.com/stats/bc/
- **Limite de domínio limpo** (BC 40 → 99):

  | Tipo de bola | BC 40 | BC 99 |
  |---|---|---|
  | Passe rasteiro de companheiro | 70 km/h | 80 km/h |
  | Passe alto de companheiro | 50 km/h | 60 km/h |
  | Bola vinda do adversário ou quicando, no chão | 50 km/h | 60 km/h |
  | Bola vinda do adversário ou quicando, no ar | 45 km/h | 50 km/h |

  - O limite é maior se houver tempo de reação.
  - Acima do limite e sem tempo, sai um "reflex trap" (domínio de reflexo).
- **Empurrão do primeiro toque ("heavy touch"):** é a velocidade extra que a bola mantém, afastando-se dele. Caso de referência: passe a 60 km/h relativo, jogador a 20 km/h.

  | Ball Control | 60 | 80 | 90 | 99 |
  |---|---|---|---|---|
  | Velocidade média extra | 8,0 km/h | 4,0 km/h | 1,9 km/h | 0,0 km/h |

  Cada toque é aleatório, de 0 a 2× esse valor.
- **Erros que restam mesmo com BC 99:**
  - Erro de direção aleatório de **±5°** (±10° no ar, mais em bolas rápidas).
  - Ao correr em cima da bola, mantém **10%** da velocidade extra da bola (BC 99) ou 45% (BC 40).
- **Recuperação depois do toque** (em domínios longos), até poder agir: 2 ticks (BC 78), 1 tick (95), 0 (111).
- **Bola vinda do adversário (interceptação):** o empurrão do primeiro toque usa **Tackling** no lugar de Ball Control. Ball Control continua definindo o limite de velocidade.
- **Direção do domínio [KONAMI v4.0]:** "se não houver direção, o jogador domina **na direção para a qual está virado**, e não automaticamente de frente para o gol". Isso facilita manter o corpo entre a bola e o defensor. Com direção no stick, domínio orientado.
- **Comandos de domínio:**
  - "Face the Opponent's Goal": soltar o stick e segurar Dash ao ir dominar. No mobile, toque duplo no lado esquerdo ao dominar.
  - Trap Feint: só no tipo Advanced.
  - Sombrero no domínio.
  - Fontes: https://www.konami.com/efootball/en/page/mobile_controller , https://www.konami.com/efootball/en/page/new_controls
- **[KONAMI v5.0]:**
  - Corrigida a bola que batia no corpo logo após o domínio.
  - Corrigido o domínio no ar que demorava a dar controle.
  - Adicionados domínios para o jogador não ficar lento depois.
  - O jogador se posiciona para dominar a bola aérea com a perna.
- **[KONAMI v5.4]:**
  - Domínios parados ou em baixa velocidade refletem melhor o input.
  - Domínios em Dash.
  - Domínio com Sharp Touch.
  - Menos empurrão indesejado de bola no domínio, que deixava a condução seguinte lenta.

### 3.2 Bola solta e disputa
- **[KONAMI v6.0]:**
  - Corrigido "o mesmo jogador continua a perseguir o cruzamento mesmo depois de a bola passar por um companheiro": o perseguidor é redeterminado.
  - Corrigido "jogador que não alcança a bola é escolhido como recebedor de um lofted pass e falha, ou o cursor troca em pouco tempo".
  - "O defensor controlado era puxado para um lançamento que o adversário vai alcançar primeiro": agora pode ser movido livremente.
- **[KONAMI v5.2]:** depois de um through ball, o defensor controlado que não perseguia a bola e seguia o input agora persegue a bola "conforme a situação". Corrigido também o cursor que ficava no autor do carrinho mesmo quando ele não era o mais rápido até a bola solta.
- **[KONAMI v5.0]:**
  - "Jogadores ganham a bola mas não conseguem tomar posse porque, indo para a bola, ficam presos no adversário": corrigido.
  - Defensores na linha não sobem para o ataque quando a bola está "quicando sem dono".
- **[KONAMI v5.4]:**
  - Corrigido o defensor que tenta dominar e erra a bola quando o adversário está prestes a tocá-la.
  - Ao perder a bola num empurra-empurra, o jogador desarmado **tem tempo de recuperação maior**, sem reação imediata.
  - [v5.0] Não é possível dar carrinho ou carga de ombro logo depois de perder a posse.
- **Super Cancel / Kick Cancel:** cancela o comando de chute. Historicamente (PES) também cancela a corrida automática até a bola. Console: R1 + R2. Mobile: toque duplo no lado esquerdo (v5.0). Fonte: https://www.konami.com/efootball/en/page/v5/versioninfo_v5-00
- **Bloqueios [EE]:** a bola bloqueada mantém 50–100% do ritmo. Tackling baixo aumenta essa fração em até +15 pontos (com 40). https://efootballexplained.com/stats/tack/

---

## 4. DEFESA

### 4.1 Match-up
- **Definição oficial:** "o defensor **agacha** e enfrenta o portador dando **passos precisos** dentro de uma faixa apropriada, aumentando a chance de bloquear passe ou chute". O defensor se coloca entre o atacante e o gol.
  - **Com Smart Assist:** "o defensor se move automaticamente para a frente do adversário. Para ajustar, dê direção".
  - **Mobile Classic:** deslizar o botão **Pressure** para a esquerda ou a direita. Controle: L2 + stick.
  - **Touch & Flick:** segurar o lado direito enquanto se move = "Dash and Match-up".
  - Fontes: https://www.konami.com/efootball/en/page/overview , https://www.konami.com/efootball/en/page/new_controls
- **Histórico de ajustes [KONAMI]:**
  - **v5.0:**
    - Ampliada a faixa de movimento entre o portador e o gol.
    - Movimento mais suave conforme a direção, "dentro de uma faixa corrigível".
    - Com Defensive Awareness alto, segue melhor.
    - Ao mandar ir até um portador distante, o defensor vai com **velocidade levemente reduzida** para conseguir acompanhar.
    - Melhor desarme automático no Match-up: posiciona, pressiona para roubar "quando chega a hora" ou dá carrinho.
  - **v5.4:**
    - Reagia cedo demais a viradas do drible: agora o defensor precisa prever.
    - Pode recuar (backpedal) conforme o drible.
    - De perto, com o input na direção do adversário, a velocidade aumenta.
    - Mais bloqueios de chute em Match-up.
  - **v6.0:**
    - "Revisado o movimento no Match-up: **em vez de parar automaticamente por um momento na frente do portador quando não há mais input de direção**, ficou mais fácil parar o jogador conforme o input."
    - Defensores da IA em Match-up reagiam rápido demais ao drible: reação revista, com diferença por Defensive Awareness.
    - Mais fácil bloquear logo após o chute.
    - Novo "precise step" para defensores.
- **Distância exata:** **não publicada.**
- **Uso [GUIA]:** defender é um "sistema de atraso". Match-up com liberação no momento certo. O defensor em Match-up "morde" o fake shot (kick feint) e demora a se recuperar. Fonte: https://pesmastery.com/efootball-defending-tutorial/

### 4.2 Pressure, desarme automático e carrinho
- **Pressure:**
  - Segurar = correr até o portador e tentar roubar. Mobile Classic: botão Pressure. Controle: X/A.
  - **[KONAMI v5.0]: "jogadores raramente tentam desarme em pé automaticamente quando a bola está longe, a menos que o comando Pressure seja dado".** Para roubar agressivamente use Pressure; para atrasar, Match-up.
  - [v5.4] Corrigida a velocidade quando o pressionador está colado por trás e ficava para trás.
  - [v5.4] O toque ao ganhar a bola com Pressure era grande demais.
- **Desarme em pé [EE]:**
  - **Depois que a bola é jogada, o desarme fica segurado brevemente.** Em 99 essa espera é 0,1 s menor que em 40.
  - Um timing de desarme: 0,41 s → 0,37 s (Tackling 65) → 0,33 s (85).
  - O alcance cresce com o quadrado da faixa 40–99 e encolhe correndo: some aos poucos acima de 22 km/h e fica em 1/5 a 30 km/h.
  - O erro aleatório de contato cai linearmente: total em 40, metade em 80, zero em 120.
  - **Não existe duelo Tackling × atributos do driblador.** O resultado é geométrico (alcance, ângulo, timing) mais o código de contato.
  - Fonte: https://efootballexplained.com/stats/tack/
- **Ângulos e skills [EE]:**
  - Desarmes de pé de longo alcance podem mirar com ângulo de 0° (Tackling 40) a 11,25° (99).
  - Long-reach Tackle dá +3,0 de alcance.
  - Power Tackle exige orientações dentro de 112,5°.
  - Fonte: https://efootballexplained.com/myths/
- **Carrinho [EE]:**
  - Valor de alcance (unidade desconhecida): 2,0 (Tackling 40), 2,9 (80), 4,0 (99).
  - A parte fixa de 2,0 encolhe acima de 22 km/h (0,55 a 30 km/h, zero a 33 km/h).
  - Velocidade do carrinho por degraus: ×0,93 (<60), ×0,97 (60), ×1,00 (70), ×1,05 (85), ×1,10 (100).
  - [KONAMI v5.4] Carrinho "fácil demais": distância e velocidade revistas, e o carrinho por trás dá mais cartão.
  - [KONAMI v5.0] Carrinho pode ser feito sem o adversário ter a bola. Ao ganhar com carrinho, o jogador levanta já com a bola.
- **Carga de ombro:** mobile Classic = toque duplo no lado esquerdo. Pode ser falta por trás. [KONAMI 2022]. A IA leva em conta o PC relativo antes de decidir a carga (v4.0).
- **Defensor batido [KONAMI v5.4]:** "quando o defensor é batido no drible, viradas pesadas ocorrem mais quanto mais rápido ele vinha. **O risco de dar Dash até a bola é maior.**"
- **Faltas [EE]:** o juiz não lê atributos nem skills. Usa ângulo, velocidade e tipo de desarme, mais regras fixas de situação. https://efootballexplained.com/stats/aggr/

### 4.3 Call for Pressure (segundo homem)
- **Comando:** mobile Classic = deslizar o botão Pressure **para cima**. Controle: R1. Não existe no Co-op.
- **Como funciona:**
  - Mostra uma seta dupla no pé do companheiro chamado.
  - Os outros se posicionam conforme a distância da bola: bloqueiam linhas de passe ou sobem a linha. [KONAMI 2022]
  - [EE] Espera antes de o segundo pressionador poder ser enviado depois de um chute ou passe: 0,59 s (DE 40), 0,44 s (99). The Destroyer: 0,52 s → 0,33 s.
  - [KONAMI v5.0/5.4] Cancela se for usado por muito tempo com os pressionadores longe da bola.
- Fontes: https://www.konami.com/efootball/en/page/new_controls , https://efootballexplained.com/stats/de/

### 4.4 IA defensiva (sem o controle do usuário) [EE]
- **Marcação:**
  - O marcador da CPU atualiza a posição ao lado do homem a cada 0,44 s (DA ≤60), 0,37 / 0,30 / 0,22 s (61/71/81), 0,15 s (91), 0,11 s (96).
  - Em jogo corrido esse intervalo é **esticado até 2×** conforme as posições do homem e da bola.
  - Fonte: https://efootballexplained.com/stats/da/
- **Re-mira e fintas:**
  - O defensor da CPU re-mira o movimento a cada 0,41 s (DA 40), 0,19 s (80), 0,037 s (99).
  - Durante uma finta, ele mira um ponto **2 m à frente no movimento do driblador** na primeira parte do movimento.
- **Shape:** com DA ≤50 a CPU mantém o lugar no shape a 75% da velocidade. A partir de 75, velocidade cheia.
- **Engajamento (Defensive Engagement):**
  - Espera para mandar um defensor "sombrear" um adversário: 1,85 s (DE 40), 0,89 s (60), 0,41 s (85), 0,22 s (99).
  - Espera para cortar linha de passe: 0,74 s (≤50) até 0 (99).
  - **Volta correndo à posição** quando ela está a mais de **19 − 0,1 × DE metros** (15 m com DE 40, 9,1 m com 99), mais até 5 m com adversário a <6 m e DE ≤55.
  - Fonte: https://efootballexplained.com/stats/de/
- **Aggression (CPU):** troca o modo de marcação só dentro de uma distância: 10 m (Aggression 50), 4 m (99), 1 m (120). Também alimenta a rapidez da pressão.
- **[KONAMI v4.4]:** defensores da IA aplicam só pressão moderada logo após um passe e ficam um pouco mais longe ao se aproximar do portador.
- **[KONAMI v5.0]:**
  - IA com Aggression alto chega mais perto do portador.
  - DE alto chega mais rápido na frente dele.
  - Atacantes descem quando o time está encurralado.
- **[KONAMI v5.4]:**
  - Linha de meio-campo e defesa levemente mais próximas.
  - Formação defensiva mais estreita, concentrada no centro.
- **[KONAMI v6.0]:**
  - Cortes de linha menos frequentes e menos perfeitos.
  - Pressão da IA logo após o passe menos imediata.
  - Marcação dentro da área mais próxima.
  - Linhas não tão coladas na marcação individual.
  - Atacante recua perto do próprio gol.
  - A reação ao passe ficou um pouco mais lenta no próprio campo.
- **Velocidade da IA [KONAMI v5.0]:** "jogadores controlados pela IA corriam em velocidade máxima mais que no futebol real; agora se movem **com mais frequência a 70% da velocidade**". Fonte: https://www.konami.com/efootball/en/page/v5/versioninfo_v5-00

### 4.5 Troca de jogador (cursor)
- **Opções:** Assisted / Semi-Auto (ou "Semi-Assist") / Manual.
  - Botão Switch = troca para o jogador mais perto da bola (escolha da IA).
  - Controle: stick direito = "o cursor vai para o jogador mais próximo na direção escolhida".
  - "Next Player Indicator" mostra quem será o próximo. O Call for Pressure se aplica ao jogador marcado por ele (manual PES 2019).
  - Fontes: https://www.konami.com/efootball/en/page/mobile_controller , https://dds.konami.com/games/manual/pes2019/PS4/en/screen.html
- **[KONAMI v5.0]:** na troca automática, o cursor tende a ir para o jogador "numa posição em que é mais fácil desafiar o adversário".
- **[KONAMI v6.0]:**
  - Corrigida a troca estranha em pouco tempo durante um passe.
  - Semi-Assist troca mais cedo para o defensor em cruzamentos rasteiros.

---

## 5. CHUTE

### 5.1 Comandos
- **Mobile Classic:** Shoot = segurar para encher a barra. Flick esquerda ou direita no Shoot = **Stunning Shot**.
- **Controle:** Controlled Shot = R1 + chute. Chip = L1 + chute. Kick Feint = chute → passe antes do contato.
- **Mobile:** fake shot = chutar e dar flick no stick para outra direção antes do contato [GUIA].
- **Dynamic Volley (v6.0):** Stunning Shot numa bola no ar, de qualquer altura. Com Smart Assist pode sair sozinho.
- **Barra de força:** amarela no normal, roxa no Stunning e azul-clara no Controlled Shot (v4.0). [KONAMI v5.1] Corrigida a diferença entre o nível mostrado na barra e o nível real.
- Fontes: https://www.konami.com/efootball/en/page/new_controls , https://www.konami.com/efootball/en/page/v4/versioninfo_v4-00 , https://pesmastery.com/efootball-shooting-tutorial/

### 5.2 Precisão [EE]
Fonte: https://efootballexplained.com/stats/fin/
- **Erro máximo:** cada chute tem um erro máximo definido pela **barra de força** e pela forma de contato. Finishing reduz quanto desse máximo o chute costuma usar.
- **Chute limpo** (90% de força, pé bom, parado, sem pressão, não de primeira):
  - Erro máximo de **8,2°** para todos.
  - Erro médio: 3,4° (Fin 60), 2,6° (80), 2,5° (90), 2,4° (99), 2,2° (120).
  - A 16 m, 1° ≈ 0,28 m.
- **Contato ruim** (50% de força): erro médio de 16,4° (Fin 60), 10,8° (90), 10,0° (99).
- **Mis-hit em contato ruim:** 90% (Fin 40), 60% (99).
- **Penalidade de chute de primeira:** começa a 15 km/h de velocidade relativa (Fin 40) até 30 km/h (Fin 99) e fica plena 35 km/h acima disso. First-time Shot conta a bola 10% mais lenta.
- **Score de precisão:** +2,7 por ponto de 60 a 89 e +1,1 a partir de 90.
- **Pênalti no mobile:** o passo que adiciona erro de mira está **desligado**.

### 5.3 Velocidade [EE]
Fonte: https://efootballexplained.com/stats/kp/
- **Velocidade = Kicking Power + fração da barra.** É uma interpolação linear entre tabelas de KP 40 e KP 99, sem custo de precisão.
- **Chute normal com força total de ≥25 m:** 95 km/h (KP 40), 104 (70), 113 (99), 119 (120).
- **De 5 m:** 81,7 → 85 km/h. **De 15 m:** 89 → 99 km/h.
- **Stunning com força total:** 88–108 km/h (KP 40) e 90–120 km/h (KP 99), conforme a distância.
- **Teto por animação:** 100 km/h (KP 40), 120 (90), 125 (99), 130 (120). No chute com força total só se aplica 30% desse corte.
- **Ganho por ponto (≥25 m):** normal +0,30 km/h, Stunning +0,21, Controlled +0,13.
- **Goleiro segura até** 92 km/h (GK Catching 40) ou 110 km/h (99). Ver 6.3.

### 5.4 Barra: tempo de enchimento e faixas
- **Tempo de enchimento:** **continua não encontrado** em nenhuma fonte lida (nem EE, nem Konami).
- **Faixas por skill (Stunning):** Dipping cerca de 1/4, Knuckle cerca de 1/2, Rising cerca de 3/4 [GUIA pesmastery]. ef-labo: Dipping 20–50%, Knuckle 50–65%, Rising 65–95%. https://ef-labo.com/efootball/player-skills/
- **Força recomendada:** Controlled em 1x1 com 1/4–1/2 da barra. Da entrada da área, 1/2–3/4 [GUIA].
- **[KONAMI v5.0]** Com Smart Assist, o nível da barra no Stunning Shot é afetado pelas skills do jogador.

---

## 6. IA SEM BOLA, LINHA E GOLEIRO

### 6.1 Ataque sem bola [KONAMI + EE]
- **Transição defesa→ataque:** depois de ganhar a bola, cada jogador muda de modo em 0,7–1,4 s conforme a posição. Com OA ≥85 esse tempo encurta (−0,12 s em 99). [EE oa]
- **[KONAMI v5.0]:**
  - Jogadores a 70% da velocidade em vez de 100% conforme a situação.
  - Corredores que vêm de trás da linha desaceleram conforme a situação.
  - Laterais e meias mais altos na construção no Possession Game.
  - Zagueiro Build Up se oferece.
- **[KONAMI v6.0]:**
  - Atacantes corriam em velocidade total logo após a troca para o ataque: velocidade e momento de partida revistos.
  - Corridas depois de se livrar do marcador.
  - Mais diagonais dos pontas.
  - Corridas perto da linha evitam o impedimento.
  - Apoiadores entre os defensores.
  - O meia ofensivo não recua até a linha dos volantes.
  - Overlap do lateral sem "laçar" o portador.
- **[KONAMI v5.4]:** desaceleração perto da linha de impedimento revista (antes freavam e quebravam a corrida).

### 6.2 Linha defensiva e impedimento
- **Offside Trap:** opção do Game Plan. No console antigo também havia o atalho de toque duplo para frente no D-pad.
- **[EE] High Line Master:** o ponto de marcação dele desce 1 m menos (5 m em vez de 6 m). **Não existe** movimento especial de armadilha de impedimento.
- **[KONAMI v6.0]:** linha sobe melhor quando o adversário está numa zona de cruzamento. Marcação contra quem corre nas costas revista.
- Fontes: https://efootballexplained.com/myths/ , https://www.konami.com/efootball/en/page/v6/versioninfo_v6-00

### 6.3 Goleiro [EE]
- **Reação básica:** 0,27 s (GK Awareness 40), 0,252 (60), 0,225 (90), 0,20 (99).
  - Pode **começar a se mover até 0,22 s antes do chute** (degraus em 49, 58, 67, 75, 84 e 93, de 1 tick cada).
  - Erro lateral aleatório de posicionamento: até 0,5 m (40), zero (99).
- **Saída do gol:**
  - Se a bola chega em até cerca de 3,3 s, ele espera antes de decidir: 0,48 s (40), 0,30 (80), 0,22 (99).
  - **Margens de saída por estilo** (ele só sai se chega antes do atacante com essa folga):

    | Estilo | Margem | Largura máxima | Antes do companheiro (folga) | Outras regras |
    |---|---|---|---|---|
    | Padrão | 0,65 s | 14,6 m | 1,2 s | posição até 15 m do gol |
    | Attacking GK | 0,45 s | 20,1 m | 0,8 s | — |
    | Defensive GK | 0,85 s | largura da pequena área | — | 1,5 s para sair mais largo |
    | Sweeper GK | — | — | — | até 42 m com a bola longe |
    | Qualquer um, com atacante cara a cara | 0,45 s | — | — | Sweeper sai sempre que for o primeiro |

  - Fontes: https://efootballexplained.com/stats/gka/ , https://efootballexplained.com/myths/
- **Reflexo:** reação mínima a bola a ≤8 m: 0,10 s (Reflexes 40), 0,063 s (99). Acima de 20 m usa Reach, e entre 8 e 20 m mistura os dois. https://efootballexplained.com/stats/gkr/
- **Alcance:** esticada máxima do mergulho ×0,94 (Reach 40), ×1,00 (63), ×1,12 (99). https://efootballexplained.com/stats/gkreach/
- **Pegada:** segura até 92 km/h (Catching 40), 98 (60), 104 (80), 110 (99). Nenhuma pegada é possível nos primeiros 0,37 s depois do chute (40) ou 0,22 s (99). https://efootballexplained.com/stats/gkc/
- **[KONAMI v5.4]:** em 1x1, o goleiro "um passo à frente" fecha mais. Ele se reposiciona mais rápido em passes laterais.
- **"Bring Out the GK" (sair do gol):** existe no mobile Virtual Pad. O gesto foi alterado na v4.0 para evitar acionar sem querer, mas **o gesto exato não foi encontrado**. Controle: segurar Through (△/Y).

---

## 7. MANUAL MOBILE: LISTA DE COMANDOS

### 7.1 Classic (Virtual Pad) — lista consolidada
**Ataque:**

| Comando | Gesto |
|---|---|
| Mover / conduzir | joystick (fixo ou "Moveable") |
| Dash Dribble | segurar Dash |
| Sharp Touch | flick no joystick com Dash pressionado (2022) |
| Quick Stop / de frente para o gol no domínio | toque duplo no lado esquerdo |
| Passe rasteiro | tocar/segurar Pass |
| Passe alto / cruzamento | flick ↑/↓ em Pass |
| Stunning Low Pass / Low Cross | flick ← em Pass |
| Stunning Lofted / Cross | flick → em Pass |
| Through | tocar Through |
| Chipped Through | flick ↑/↓ em Through |
| Stunning Through | flick ← em Through |
| Stunning Chipped | flick → em Through |
| Chute | Shoot |
| Stunning Shot / Dynamic Volley | flick ←/→ em Shoot |
| Pass-and-run | depois do passe, puxar e dar flick no lado esquerdo |
| Kick Cancel | toque duplo no lado esquerdo depois do comando de chute ou passe (v5.0) |
| Fintas | Dash + gestos do stick (ver 1.6) |

**Defesa:**

| Comando | Gesto |
|---|---|
| Pressure | segurar Pressure |
| Match-up | deslizar Pressure ←/→ |
| Call for Pressure | deslizar Pressure ↑ |
| Carga de ombro | toque duplo no lado esquerdo |
| Carrinho | Tackle |
| Troca de cursor | Switch |
| Dash | segurar Dash |

- **Configurações:**
  - Smart Assist (ligado por padrão para usuários novos).
  - Cursor Change: Assisted / Semi-Auto / Manual.
  - Feint Command Type: Easy / Smart / Advanced.
  - Pass Search Guide.
  - Defending Control Type: Standard / Pressure.
  - Movement Input Range, Directional Stick Type e Flexible Auto Control.
  - Button Opacity, Next Player Indicator e Radar.
  - Fonte das configurações: https://gamingonphone.com/guides/efootball-guide-to-the-best-gameplay-and-game-plan-settings-for-a-smooth-experience/ [GUIA, set/2026]
- Fontes dos comandos: https://www.konami.com/efootball/en/page/new_controls , https://www.konami.com/efootball/en/page/v5/versioninfo_v5-00
- **Não encontrados:** os gestos atuais de Finesse Dribble, Controlled Shot, Chip Shot, cruzamento rasteiro explícito e Bring Out the GK no mobile Classic.

### 7.2 Controle físico no mobile (manual oficial "mobile_controller")
- **Numeração dos botões:** deduzida cruzando com https://www.fifplay.com/efootball-2027-controls/ :
  - ① O / B, ② X / A, ③ ▢ / X, ④ △ / Y
  - ⑤ R1, ⑥ R2, ⑦ L1, ⑧ L2
  - ⑨ stick direito, ⑩ stick esquerdo, ⑪ Options
- **Ataque:**

  | Comando | Entrada |
  |---|---|
  | Conduzir | ⑩ |
  | Dash Dribble | ⑩ + ⑥ |
  | Sharp Touch | ⑩ + ⑥ ×2 |
  | Quick Stop | soltar ⑩ e segurar ⑥ |
  | Finesse Dribble | ⑩ + ⑧ |
  | Passe rasteiro | ② |
  | Passe alto / cruzamento | ① |
  | Through | ④ |
  | Chipped Through | ⑦ + ④ |
  | Stunning | ⑥ até o fundo + qualquer chute |
  | Controlled Shot | ⑤ + ③ |
  | Chip | ⑦ + ③ |
  | Kick Feint | ③ → ② |
  | Kick Cancel | qualquer chute → ⑤ + ⑥ |
  | 1-2 | ⑦ + ② |
  | Pass-and-run | ⑤ + ② ou ④ |

- **Defesa:**

  | Comando | Entrada |
  |---|---|
  | Match-up | ⑩ + ⑧ |
  | Pressure | ② |
  | Carga de ombro | ③ (com o adversário driblando) |
  | Carrinho | ① |
  | Afastar | ③ (sem posse adversária, no próprio campo) |
  | Cursor | ⑦ |
  | Cursor manual | ⑨ (vai ao mais próximo na direção) |
  | Call for Pressure | ⑤ |
  | Goleiro vai na bola | segurar ④ |

- **Fintas do tipo Advanced:**

  | Finta | Entrada (com o jogo para →) |
  |---|---|
  | Marseille | ⑩↓/↘ segurando ⑨→ |
  | Double Touch | segurar ⑨↓, depois ⑩→ |
  | Scissors | rotação de ⑨ e depois ⑩. O número de pedaladas depende do timing |
  | Rainbow Flick | ⑨ ×2 |
  | Chop | ⑩↗/↘ segurando ⑦ |
  | L Feint, Scotch | exigem a skill |

- **Onde funciona:** só contra a IA (VS AI, exceto divisões baixas) e no Friend Match.
- Fonte: https://www.konami.com/efootball/en/page/mobile_controller

---

## 8. RITMO DO JOGO

- **Simulação:** 27 Hz [EE].
- **Velocidades de referência [EE]:**

  | Movimento | km/h | m/s |
  |---|---|---|
  | Andar (igual para todos) | 5 | 1,39 |
  | Faixa de stamina: recarrega (parado/andando) | até 8 | 2,2 |
  | Faixa de stamina: corrida leve, custa pouco | até 13,5 | 3,75 |
  | Faixa de stamina: custa cerca de 75% do sprint | 13,5–19 | 3,75–5,3 |
  | Sprint | 27–32,5 | 7,5–9,0 |
  | Com bola (máximo) | 25,8–30,7 | 7,2–8,5 |

  - A "corrida normal" é igual para todos (o valor não foi publicado).
  - Fontes: https://efootballexplained.com/stats/speed/ , https://efootballexplained.com/stats/stamina/
- **Aceleração [EE]:** três fases.
  1. Empurrão forte até 15 km/h (4,17 m/s).
  2. Empurrão menor até 22,5 km/h (6,25 m/s).
  3. Subida lenta até a máxima.
  - Acceleration de 40 para 99 deixa a fase 1 34% mais forte e as fases 2 e 3 53% mais fortes.
  - De parado até a máxima (Speed 80): 3,20 s (Acc 60), 2,56 s (80), 2,31 s (90), 2,22 s (99).
  - Distância nos primeiros 3 s (Speed 80): 18,4 m (Acc 60), 20,2 m (90), 20,4 m (99). Com os dois stats iguais: 19,7 m (80/80), 20,7 m (99/99).
  - 40 m partindo do parado (Acc 80): 5,80 s (Speed 40), 5,40 s (80), 5,24 s (99).
  - Fonte: https://efootballexplained.com/stats/accel/
- **Cansaço:**
  - Abaixo de 70% de stamina, Speed e Acceleration caem até ×0,90 (com 10% restante).
  - Dribbling, BC, Finishing, LP, LoP, PC, Balance, KP e Jump caem até ×0,80.
  - Para 1% do relógio em sprint, o jogador perde 5,4% do tanque (Stamina 40), 2,2% (80) e 1,5% (99).
  - O tanque fica igual com a bola fora de jogo.
  - [EE stamina]
- **Duração da partida:** o tempo de jogo é configurável (Match Time) em amistosos e contra a IA (v4.0), mas **não achei o valor padrão do PvP.** Um guia de 2024 recomendava não passar de 6 min no Friend Match por causa de um bug. Fontes: https://www.konami.com/efootball/en/page/v4/versioninfo_v4-00 , https://gamingonphone.com/guides/efootball-friend-match-mode-guide-tips
- **Fluxo:**
  - **Bola parada contínua:** existem "seamless set pieces" (lateral e reposição sem corte) e "Quick Restart" (qualquer comando de chute enquanto os jogadores se preparam).
  - [KONAMI v6.0] Lateral curto acelerado. Jogadores caídos perto do reinício levantam mais rápido.
  - [KONAMI v5.4] Contra a cera, fica mais difícil gastar tempo trocando passes altos na mesma zona.
  - [KONAMI v5.0] O goleiro não pode pegar a bola para gastar tempo (recuos após tiro de meta etc.).
  - Fontes: https://www.konami.com/efootball/en/page/mobile_controller , https://www.konami.com/efootball/en/page/v5/versioninfo_v5-40
- **Faltas:** o árbitro decide por ângulo, velocidade e tipo de desarme [EE]. A Konami endureceu o carrinho por trás e o "obvious goal-scoring opportunity" na v5.4.

---

# ESPECIFICAÇÃO DE IMPLEMENTAÇÃO (priorizada)

Convenções:
- dt = 1/30 s.
- Velocidades em m/s.
- `score(x)` é a curva 0..1 de duas dobras da seção 1.3: pontos 40→0,083, 60→0,183, 80→0,667, 90→0,90, 99→1,0, com interpolação linear entre eles e extrapolação até 120 com inclinação de 0,0102/ponto.
- **Fonte** = número tirado de fonte (EE ou KONAMI). **[EST]** = valor meu, para calibrar.

## P1. Condução por toques (resolve "conduzem mas ignoram a bola")

1. **A bola nunca é filha do jogador.** Durante a condução, a bola é um corpo físico livre: rola com atrito e quica. O condutor tem um estado `Dribbling` com:
   - `lastTouchTime`
   - `plannedFoot`: pé bom por padrão. Use o pé fraco só se a bola estiver claramente do lado dele, com chance ∝ Weak Foot Usage. Fonte KONAMI v6.0: "mais fácil usar o pé bom".
   - `desiredDir`: vem do joystick.
2. **Condição para um toque** (avaliada todo tick):
   - (a) `t − lastTouchTime ≥ Tmin`; **e**
   - (b) a bola está dentro do alcance do pé planejado. Distância do ponto de contato (0,25 m à frente e 0,12 m ao lado do pé, [EST]) ≤ `Rtouch` = 0,35 m [EST]. Altura da bola < 0,3 m [EST].
   - (c) Se (b) falhar porque a bola está longe, o jogador **persegue o ponto de interceptação da bola**, e não a direção do joystick. Ao perseguir, a direção do corpo é limitada pela taxa de rotação (P3).
3. **Tmin** (fonte EE, convertido para segundos):
   - Base: `Tmin = lerp(0,23 s, 0,18 s, score(TP))`. Fonte: 0,23 em 40 e 0,18 em 99.
   - Se a velocidade do condutor for < 5,56 m/s (20 km/h) **e** a mudança de direção pedida for < 22,5°: `Tmin = 0,50 s`.
   - Se a mudança pedida for ≥ 45°: `Tmin = lerp(0,49, 0,40, score(TP))` em condução lenta. Em qualquer velocidade, divida o ritmo por `turnMul = lerp(1,02, 1,20, score(TP))`. Entre 22,5° e 45°, interpole entre 1,0 e turnMul [EST].
   - Animação do toque: multiplique a velocidade por `lerp(0,95, 1,15, score((Acc+TP)/2))`.
   - Fadiga: reserva < 20% → TP e Acc efetivos −(20 − reserva%) pontos.
4. **Execução do toque** (o que dar à bola), [EST] derivado do modelo da fonte:
   - Direção: `desiredDir` mais ruído uniforme de ±2° [EST]. O BC não entra (fonte EE: Ball Control não afeta a condução normal).
   - Velocidade de saída da bola: `vBall = vCarrierTarget × kPush`, com `kPush` = 1,25 na condução normal, 1,6 no Dash e 2,2 no Sharp Touch [EST].
     - Escolha `kPush` para que, com atrito de rolamento `μ·g ≈ 2,0 m/s²` [EST], a bola fique entre 0,6 e 1,0 m à frente do pé no próximo toque na condução normal, 1,5–3 m no Dash e 4–6 m no Sharp Touch [EST].
     - Isso é coerente com a fonte: a distância cresce com a velocidade, e não com Dribbling.
   - Se o input mudou ≥ 90° em alta velocidade: o toque sai **mais curto** (`kPush × 0,6` [EST]) e o jogador entra numa animação de virada (P3). É isso que dá o "raio de giro com a bola".
   - Nunca permitir o toque se a bola estiver atrás do quadril (ângulo bola–corpo > 100° [EST]). Nesse caso o jogador primeiro gira e se reposiciona. É o "overrun" que a Konami corrigiu na v6.0: o jogador cancela outras animações e vai para a bola.
5. **Velocidade-alvo do condutor:**
   - Sem Dash: `vNormal` = 5,0 m/s [EST]. Fica abaixo do limiar de 20 km/h, de modo que vale o ritmo lento de 0,5 s; o limiar de 22 km/h da reserva de sprint também não é atingido.
   - Com Dash: `vMaxBall = (26,5 + 6·√(score(Spd)·score(Drb)) − 1,8)/3,6`. Fonte EE.
   - Andar: 1,39 m/s. Fonte EE.
   - O condutor ajusta a velocidade para **chegar na bola** no instante do próximo toque permitido: `vNeeded = dist(pé, bola_prevista(t+Tmin))/Tmin`, limitada por vMax e pela aceleração (P2). Assim ele nunca "atravessa" a bola nem a deixa para trás.
6. **Reserva de sprint com bola** (fonte EE):
   - `reserve ∈ [0,1]`. Se a velocidade for > 6,11 m/s (22 km/h), drena 1/2,8 por segundo.
   - Sem bola, recarrega 1/7 por segundo. Com bola e devagar, 1/12 por segundo.
   - Capacidade máxima = max(0,2, stamina restante) [EST: a fonte só diz "encolhe com a stamina, mínimo 1/5"].
   - Efeitos: reserva ≤ 0,8 → Dribbling −25 nas fintas; reserva = 0 → −50; reserva < 0,2 → kicking stats −5 e toques mais lentos.
7. **Sharp Touch:**
   - Comando: flick no joystick com Dash pressionado.
   - Um único toque com `kPush` alto na direção do flick, seguido de aceleração máxima.
   - Bloqueia novos toques até o jogador alcançar a bola (condição 2b). Isso cria a janela de risco natural para o defensor (P5).
8. **Quick Stop:** toque duplo no lado esquerdo. Toque de sola: a bola fica com velocidade ≈ 0 junto ao pé, e o jogador desacelera a 9 m/s² [EST].
9. **Proteção automática** (sem botão, fonte KONAMI v4.0):
   - Se houver adversário a < 1,5 m [EST] e o condutor estiver lento (< 3 m/s [EST]), o próximo toque mantém a bola do lado oposto ao adversário: bola posicionada a 0,5 m [EST], no lado do pé mais longe dele.
   - O corpo fica entre a bola e o adversário.
   - Os empurrões usam P4.
10. **Smart Assist** (opcional, fonte KONAMI overview):
    - Desvia `desiredDir` em até 30° [EST] para longe do adversário que se aproxima.
    - A menos de 2 m [EST] da linha lateral, reduz a velocidade e vira para não sair.

## P2. Locomoção (todos os jogadores)

1. **Velocidade máxima sem bola:** `(26,5 + 6·score(Speed))/3,6`. Fonte EE.
2. **Aceleração em 3 fases** [EST ajustado aos números EE para Spd 80 / Acc 80: 2,56 s até a máxima e cerca de 19,7 m em 3 s]:

   | Fase | Faixa de velocidade | a (Acc 80) |
   |---|---|---|
   | 1 | 0 → 4,17 m/s | 10 m/s² |
   | 2 | 4,17 → 6,25 m/s | 5 m/s² |
   | 3 | 6,25 → vmax | 1,3 m/s² |

   - Escalas por Acceleration (fonte EE): de 40 para 99, a fase 1 fica ×1,34 e as fases 2 e 3 ficam ×1,53. Normalize para que Acc 80 dê os valores acima.
   - Calibre contra a tabela da fonte: até a máxima em 3,20 / 2,56 / 2,31 / 2,22 s (Acc 60/80/90/99).
3. **Frenagem:** 9 m/s² [EST]. Varia no máximo ±5% com Acceleration (fonte EE).
4. **Taxa de rotação fixa para todos** (fonte EE; o valor é meu):
   - 720°/s parado, 360°/s a 5 m/s e 180°/s a 8 m/s [EST].
   - Virada > 89° acima de 6 m/s: animação de virada que custa 0,25–0,35 s com desaceleração forçada [EST].
   - Animações de partida, parada e virada ficam 1,5% mais rápidas com Acc 99 e cerca de 3% mais rápidas com 165 cm em vez de 195 cm (fonte EE).
5. **IA sem bola:** usa 70% da vmax como velocidade de cruzeiro "na maioria das situações" (fonte KONAMI v5.0). Sprint só em corrida principal, recuperação e pressão.
6. **Fadiga:** abaixo de 70% de stamina, multiplique Speed e Acc por lerp(1, 0,9) e os stats técnicos por lerp(1, 0,8) até 10% (fonte EE).

## P3. Recepção e domínio

1. **Movimento do recebedor:**
   - Ao sair o passe, o recebedor vai ao ponto de interceptação mais cedo alcançável.
   - **Se houver adversário a < 3 m [EST] dele ou da linha, ele anda em direção à bola** (fonte KONAMI v6.0).
   - A velocidade ao receber é reduzida para 70% [EST] (fonte KONAMI v6.0: "se moviam rápido demais ao receber").
   - Ele para e se orienta 0,2 s [EST] antes da chegada da bola.
2. **Atraso de reação de quem não é o recebedor:** base de 0,4–0,8 s conforme a posição (fonte EE). Só o recebedor e o cursor reagem de imediato.
3. **Domínio:**
   - Limite limpo: `vLim = lerp(70, 80, BC_lin) km/h` no rasteiro de companheiro, onde BC_lin = (BC − 40)/59, linear. Outros tipos de bola na seção 3.1. Fonte EE.
   - Acima do limite e com pouco tempo: "reflex trap", com a bola espirrando 1–3 m [EST].
   - **Direção:** com o stick, a direção do stick. Sem stick, a direção do corpo, não o gol (fonte KONAMI v4.0). Ruído de ±5° no chão e ±10° no ar (fonte EE, com BC 99). Escale para cerca de ±10° com BC 40 [EST].
   - **Empurrão:** a bola recebe a velocidade do jogador mais um extra aleatório de 0 a 2·push(BC). Valores de push: 8 km/h (BC 60), 4 km/h (80), 1,9 km/h (90), 0 (99). Extrapole cerca de 12 km/h em 40 [EST]. Fonte EE.
   - Ao correr em cima da bola, mantém 45% (BC 40) a 10% (BC 99) da velocidade relativa extra (fonte EE).
   - Bola vinda do adversário: o empurrão usa Tackling no lugar de BC (fonte EE).
   - Recuperação após o toque: 2 / 1 / 0 ticks de 27 Hz (BC 78 / 95 / 111), ou seja 0,074 / 0,037 / 0 s.
4. **Bola solta:**
   - A cada 3 ticks [EST], cada time calcula o tempo de chegada (TTA) de cada jogador à bola. O de menor TTA é o "chaser".
   - Troque de chaser se outro companheiro ficar melhor por mais de 0,15 s [EST] (fonte KONAMI v6.0, cruzamento).
   - O jogador controlado só é puxado automaticamente para a bola se o time dele for o primeiro a chegar (fonte KONAMI v6.0). Ele sai desse modo com qualquer input contrário ou com Kick Cancel.

## P4. Contato físico

1. **Esbarrão** (sem aleatoriedade, fonte EE):
   - Parcela que A recebe = clamp(0,5 − 0,01·(PC_A − PC_B), 0,2, 0,8).
   - Ajuste por velocidade: +até 0,35 para quem corre ≥ 3,75 m/s contra alguém parado (≤ 1,4 m/s).
   - Com bola, a fonte diz que a divisão é "por razão" (80 contra 70 → 47%), com fórmula exata não publicada. Use a linear e calibre.
2. **Reação:**
   - Diferença ≤ −10: stagger pesado. Senão, leve ou nenhum.
   - Contato leve nunca derruba. Queda só com velocidade de fechamento > 5 m/s [EST] ou Power Tackle de lado ou por trás.
   - Stagger extra por Balance: 5→0 ticks (degraus em 75/83/89/95).
3. **Condutor em sprint** (3,0–6,9 m/s) segue correndo contra carga se:
   - vantagem de PC ≥ +30 pela frente, +15 pelo lado, ≥0 por trás;
   - e a carga vier de um ângulo ≥ lerp(135°, 56°, Balance 40→99) da linha de corrida.

   Senão, perde a bola ou é desviado.
4. **Carga contínua sobre o portador parado:** ele perde a bola depois de `hold = 3,0 + (PC_A − PC_B)/40` s, entre 2 e 4 s. Fonte EE.
5. **Chute sob pressão:** perda de velocidade de até 17% / 8,5% / 0% (20+ mais fraco / igual / 20+ mais forte), só em chutes de 120 km/h ou mais, sumindo até 75 km/h. Fonte EE.

## P5. Defesa (Match-up, Pressure, desarme)

1. **Match-up** (deslizar Pressure ←/→, mantido enquanto o dedo segura):
   - Alvo = ponto na linha portador→centro do gol, a `dMU` = 1,8 m [EST] do portador. Faixa ajustável pelo stick: ±1,5 m lateral e 0,8–3 m de distância [EST]. "Faixa corrigível" (fonte KONAMI v5.0).
   - Corpo sempre voltado para o portador. Passos curtos, velocidade máxima de 70% da vmax [EST]. Recuo (backpedal) permitido.
   - Se o portador está longe (> 6 m [EST]) e o input aponta para ele: aproxima com velocidade levemente reduzida (fonte KONAMI v5.0).
   - Sem input: **não congela** na frente do portador (fonte KONAMI v6.0). Mantém o alvo dinâmico.
   - Reação ao drible ou finta: atraso de 0,15–0,30 s [EST], diminuindo com Defensive Awareness. O defensor **não pode** reagir antes do toque real na bola (fonte KONAMI v5.4).
   - Na finta, mira 2 m à frente do movimento do driblador na primeira parte (fonte EE).
   - **Desarme automático:** dispara quando a bola está a ≥ `dBall` do pé do condutor (1,2 m com Aggression 40 até 0,2 m com 99, fonte EE) **e** dentro do alcance do desarme.
     - Alcance base de 1,0–1,6 m [EST], crescendo com o quadrado de Tackling e encolhendo acima de 6,1 m/s até 1/5 a 8,3 m/s (fonte EE).
     - Desarmes de longo alcance podem começar de 2 m (Aggression ≤ 60) a 5 m (95) (fonte EE).
     - Depois de um chute ou passe, segure o desarme por 0,1–0,2 s (fonte EE: 0,1 s a menos em 99).
   - **Resultado do desarme:** geométrico. O pé do desarme (cápsula) intersecta a bola antes do próximo toque do condutor? Some um erro de contato aleatório: total em Tackling 40, metade em 80, zero em 120 (fonte EE).
     - Se ganhar, a bola sai com 2–4 m/s [EST] na direção do desarme (fonte KONAMI v6.0: "não chutar longe demais").
   - Bloqueio de chute ou passe: o Match-up aumenta o tamanho da cápsula de bloqueio em 30% [EST].
2. **Pressure** (segurar):
   - Corre até um ponto 0,8 m [EST] à frente do portador, na trajetória dele. Ao pressionar por trás, contorna para ficar na frente (fonte KONAMI v4.0).
   - Só com Pressure a IA tenta desarme em pé a distância (fonte KONAMI v5.0). Sem Pressure e sem Match-up, nenhum desarme automático com a bola longe.
3. **Defensor batido:** se o condutor passou e o defensor estava a > 5 m/s, aplique uma virada pesada de 0,4–0,6 s [EST] (fonte KONAMI v5.4).
4. **Perdeu a bola:** proíba carrinho e carga por 0,5 s [EST] e aplique um tempo de recuperação de 0,3 s [EST] (fonte KONAMI v5.0/v5.4).
5. **Call for Pressure** (deslizar Pressure ↑):
   - O companheiro do Next Player Indicator pressiona após uma espera de 0,59→0,44 s por DE (fonte EE).
   - Cancela se o tempo segurado passar de 4 s [EST] com o pressionador a mais de 12 m da bola [EST].
6. **Troca de cursor:**
   - Assisted / Semi-Auto / Manual.
   - Escolha automática = menor TTA a um ponto 1 m à frente do portador, priorizando quem fica entre a bola e o gol (fonte KONAMI v5.0: "posição mais fácil para desafiar").
   - Histerese de 0,4 s [EST]. Nunca troque durante um desarme. O cursor fica no alvo do passe.

## P6. Passe

1. **Assistência por cone:**
   - Cone de ±35° [EST]. Pontuação = alinhamento × distância × linha livre.
   - Smart Assist pode trocar o alvo e trocar o rasteiro bloqueado por passe alto ou forte (fonte KONAMI).
2. **Velocidade planejada de chegada:** 8–12 m/s no pé do recebedor [EST], escalada por Low Pass ×0,825 (40) até ×1,0 (99) (fonte EE).
   - Velocidade de saída = solução balística com atrito 2,0 m/s² [EST].
   - Mantenha a chegada abaixo do limite de domínio limpo do recebedor: 19,4–22,2 m/s = 70–80 km/h (fonte EE).
3. **Erro angular:**
   - Base: 1,06° (LP 40) a 0,49° (99) num passe de 30 km/h (8,3 m/s).
   - Passe duro: acima de 11–14 m/s (40–50 km/h), some erro extra até 3,0° (LP 60) / 0,59° (99) a 19,4 m/s (fonte EE).
   - Multiplicadores [EST]: pressão ×1,5, passe em corrida ×1,3, de primeira por velocidade de chegada ×(1 + v/20).
4. **Lofted:** altura máxima de 7,5 m (LoP 40) a 5,5 m (99) (fonte EE). Erro de 4,94° (60) a 1,31° (99) a 22 m/s (80 km/h).
5. **Stunning:** velocidade ×1,3 [EST] e preparação +0,15 s [EST] quando o passador está parado ou lento (fonte KONAMI: "a bola demora mais a sair do pé").
6. **Through:** alvo = posição do recebedor + v_recebedor × tempo de voo, limitado antes da área do goleiro. O corredor recebe ordem de corrida de imediato.
7. **1-2 / Pass-and-run:** o passador recebe uma corrida-alvo (frente ou diagonal cruzando). A devolução é de primeira.

## P7. Chute

1. **Barra:** linear, de 0 a 100% em 1,0 s [EST: não há fonte]. Sai ao soltar ou no máximo. Stunning = flick no Shoot, com preparação de 0,33 s (0,30 com Fin ≥ 84) (fonte EE).
2. **Velocidade (fonte EE):**
   - `v = lerp(tab40, tab99, (KP − 40)/59)` × barra, com interpolação linear e sem limite.
   - Referências com força total: 25 m+ → 95 / 113 km/h; 15 m → 89 / 99; 5 m → 81,7 / 85 (KP 40 / 99).
   - Teto por animação: 100–125 km/h.
3. **Erro:**
   - Erro máximo = f(barra), com 8,2° perto de 90% (fonte EE). [EST] 3° em 25% da barra, 5° em 50%, 8,2° em 90%.
   - Fração usada ~ |N(0, σ)|, com σ calibrado para dar média de 3,4° (Fin 60) / 2,4° (99) em 90% de força.
   - Contato ruim: mis-hit de 90% (Fin 40) a 60% (99), com o erro empurrado para o máximo.
   - Primeiro toque: a penalidade começa em 15→30 km/h de velocidade relativa (Fin 40→99).

## P8. Goleiro (valores de fonte EE, usar direto)

- **Reação:** 0,27→0,20 s (GK Awareness). Pode antecipar até 0,22 s.
- **Reflexo a ≤ 8 m:** 0,10→0,063 s. A partir de 20 m usa Reach, misturando entre 8 e 20 m.
- **Mergulho:** esticada máxima ×0,94→×1,12 sobre o alcance base de 2,8 m [EST].
- **Pegada:** se a velocidade do chute ≤ 92→110 km/h (Catching 40→99), e não nos primeiros 0,37→0,22 s depois do chute. Senão, espalma.
- **Saída do gol:** só se `TTA_gk + margem < TTA_atacante`, com margem padrão de 0,65 s (0,45 cara a cara) e largura de até 14,6 m. Espera de decisão de 0,48→0,22 s depois do último toque, quando a bola chega em ≤ 3,3 s.
- **Posicionamento:** bissetriz do ângulo, até 15 m fora do gol. Erro lateral de 0,5 m (40) → 0 (99).

## P9. IA do time

1. **Marcação:** cada marcador CPU reavalia a posição a cada `Tm` = 0,44→0,15 s (DA ≤60 → 91), ×1 a ×2 conforme a distância da bola (fonte EE). Re-mira a cada 0,41→0,037 s.
2. **Transição:** depois de perder a bola, espere o "shadow" (1,85→0,22 s por DE) e volte em sprint se estiver a mais de `19 − 0,1·DE` m da posição (fonte EE).
3. **Corridas:**
   - A espera para uma corrida pode começar 1,0→0 s (OA 40→97) depois de o portador iniciar a ação.
   - De 1 a 3 corredores a ≤ 18 m da bola (24 m Goal Poacher) atacam a linha de impedimento.
   - Desaceleram perto da linha só o necessário para não ficar impedidos (fonte KONAMI v5.4/v6.0).
4. **Transição defesa→ataque:** 0,7–1,4 s por posição (fonte EE oa). Sem disparada imediata de todos (fonte KONAMI v6.0).
5. **Bloco defensivo:** linhas de meio e defesa próximas e formação estreita (fonte KONAMI v5.4). Parâmetros por Team Playstyle, como no relatório v1.

---

## O que continua sem número público

- **Tempo de enchimento da barra de força.**
- **Taxa de rotação fixa:** o EE confirma que ela existe, mas não dá o valor.
- **Distância e faixa exatas do Match-up.**
- **Velocidade da "corrida normal" sem Dash:** igual para todos.
- **Raio do pé e alcance de toque na condução.**
- **Duração padrão da partida PvP.**
- **Gestos atuais no mobile Classic:** Finesse Dribble, Controlled Shot, Chip e Bring Out the GK.
- **Páginas não lidas:** efootballedge.com e efootball.cz são JavaScript e não puderam ser lidas. efootbase.com devolveu 403. A imagem da legenda do controle no site da Konami está num host bloqueado.
