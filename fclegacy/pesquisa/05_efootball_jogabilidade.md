# eFootball (mobile, temporadas 2025/2026/2027): sistema de jogabilidade para reimplementação

Data da pesquisa: 2026-11-11. Versão atual do jogo nesta data: v6.x ("eFootball 2027", lançada em 12/13-08-2026). A v5.x foi a "eFootball 2026" (14-08-2025), e a v4.x a "eFootball 2025".

## 0. Limites desta pesquisa (leia primeiro)

- **Acesso:** nesta sessão só o motor de busca (WebSearch) funcionou. A leitura direta de páginas (WebFetch) foi bloqueada pela rede da sessão para konami.com, fifplay.com, pesmastery.com, efootballedge.com e efootballexplained.com. Tudo o que está abaixo veio dos trechos e resumos que o buscador devolveu. Nenhuma página foi lida na íntegra. Para leitura completa, esses domínios precisam ser liberados nas configurações de rede da sessão.
- **Nomes dos esquemas de controle:** o nome "Advanced" no mobile do eFootball **não** significa joystick mais botões. Pelas fontes, há dois tipos no mobile:
  - **"Classic"**: joystick virtual mais botões (Pass, Through, Shoot, Dash no ataque; Switch, Pressure, Tackle, Dash na defesa).
  - **"Advanced" / "Touch & Flick"**: sem botões, só toques e flicks na tela.

  O que você descreve (joystick mais botões) é o **Classic**. Este relatório descreve o Classic e cita o Touch & Flick quando relevante. Fontes: https://efootballlab.com/blog/best-efootball-mobile-settings.html , https://inquisitiveuniverse.com/2022/05/30/efootball-mobile-new-controls/
- **Números internos:** a Konami **não publica** números internos como velocidades da bola, tempo de enchimento da barra ou raios de troca. Os únicos números "duros" que achei vêm de engenharia reversa da comunidade (efootballexplained.com, efootbase.com, ef-labo.com) e de notas de patch.
- **Convenção de marcação:** tudo marcado **[ESTIMATIVA]** é proposta minha para implementação, com base em futebol real ou em observação típica de jogos do gênero. Não vem de fonte.

---

## 1. PASSE

### 1.1 Passe rasteiro (Low Pass)
- **Como funciona no eFootball:**
  - **Entrada:** botão Pass. A barra de força aparece nos pés do jogador e enche enquanto o botão é segurado. O chute sai quando você solta o botão **ou** quando a barra chega ao máximo. Isso vem do manual Konami (texto herdado do PES 2019, mesmo sistema). Fonte: https://dds.konami.com/games/manual/pes2019/PC/en/screen.html
  - **Toque curto:** sai um passe curto. Segurar dá mais força e alcance. Guias recomendam força controlada nos passes curtos e segurar mais nos longos. Fonte: https://efootballarena.blog/the-best-way-to-use-manual-controls/
  - **Alvo:** a direção do joystick esquerdo define a direção. Com assistência, o jogo escolhe o companheiro mais adequado naquela direção, e a força é corrigida para o alvo escolhido.
- **Nível de assistência de passe (console/PC):**
  - Vai de 1 a 4, e o padrão é **3**.
  - **Nível 1:** a IA ajuda muito, mas pode errar o recebedor quando há 2 companheiros na mesma direção geral.
  - **Nível 4:** a IA ajuda pouco.
  - Também existe "Manual". Fonte: https://pesmastery.com/efootball-passing-tutorial/
  - No mobile, o que existe é o **Smart Assist** (Extras > Game Settings > Play Settings > Smart Assist). Ele pode trocar o alvo para um jogador livre e converter um passe rasteiro em passe forte ou alto quando o rasteiro "não é ideal". Fontes: https://www.konami.com/efootball/en/page/overview , https://gamingonphone.com/news/efootball-2026-understanding-the-smart-assist-feature-and-the-advantages-of-it
- **Velocidade da bola e atributo:**
  - **Notas v4.4.0 (Konami):** o atributo **Low Pass** passou a afetar mais a velocidade de passes rasteiros, cruzamentos rasteiros e passes em profundidade, **e a velocidade da animação do chute** (valor maior = chute mais rápido). Fonte: https://www.konami.com/efootball/en/page/v4/versioninfo_v4-40
  - **Teste da comunidade (v4.4.0):** a bola fica mais rápida com Low Pass acima de 80 e mais lenta abaixo de 80. Com atributo perto de 70 a imprecisão é grande. Fonte: thread r/eFootball "Zhuhai Amadeusz analysis 4.40 new engine" (espelho https://libreddit.winscloud.net/r/eFootball/comments/1k5aaj7/zhuhai_amadeusz_analysis_440_new_engine )
  - **Pé fraco:** a v6.0.0 melhorou levemente a precisão do pé fraco em passes curtos. Fonte: https://www.konami.com/efootball/en/page/v6/versioninfo_v6-00
- **Números:** nenhuma fonte dá a velocidade da bola em m/s. **[ESTIMATIVA]** Passe rasteiro curto (10–15 m) a 10–14 m/s, rasteiro longo/forte (25–35 m) a 16–22 m/s, Stunning a +15–25%. Aplique atrito de rolamento e aceleração de ~-1,5 a -2,5 m/s² na grama.

### 1.2 Passe de primeira (one-touch)
- **Como funciona:** você aperta Pass antes de a bola chegar, e o recebedor passa direto sem dominar.
- **Skill "One-touch Pass":** o jogo trata a bola que chega como **10% mais lenta**, o que reduz a perda de precisão e o mis-kick. Isso implica que a velocidade da bola que chega penaliza a precisão do passe de primeira. Fonte: https://efootbase.com/wiki/skills/one-touch-pass
- **Skill "Visionary Pass":** +6 no atributo de passe usado em seguida, inclusive em passe de primeira. Fonte: busca sobre efootballexplained/skills (https://efootballexplained.com/skills/)

### 1.3 Passe em profundidade (Through Ball)
- **Entrada e variações:**
  - **Botão Through:** a bola vai para o espaço à frente do recebedor.
  - **Through rasteiro:** toque em Through.
  - **Through alto / Chipped Through:** flick para cima ou para baixo no botão Through (mobile Classic, notas 2022).
  - **Stunning Through Ball:** flick para a esquerda no botão Through. É "uma versão mais rápida do Through Ball normal, que tende a terminar mais perto dos pés do recebedor". Fonte: https://www.konami.com/efootball/en/page/new_controls
- **Atributo:** Lofted Pass governa a velocidade e a altura da trajetória do chipped through (v4.4.0).
- **Uso (guias):**
  - Lance o through no instante em que a corrida começa, não quando o atacante já está na velocidade máxima.
  - Se o atacante está parado ou a linha defensiva está funda, a bola corre para o goleiro.
  - Fonte: https://gamemarket.gg/news/efootball/efootball-attacking-guide-build-up-through-balls-and-finishing
- **[ESTIMATIVA] Cálculo do alvo:** ponto = posição do recebedor + velocidade do recebedor × tempo de voo previsto + avanço extra proporcional à força. Limite o alvo para não passar da linha do goleiro (o "corre para o goleiro" é exatamente isso).

### 1.4 Passe alto / lançamento / cruzamento (Lofted Pass, Cross)
- **Entrada:** flick para cima ou para baixo no botão Pass (mobile Classic). Fonte: https://www.konami.com/efootball/en/page/new_controls
- **Cruzamento:** é o passe alto executado em zona lateral.
- **Cruzamento rasteiro (Low Cross):** toque duplo no botão de cruzamento (console).
- **Cruzamento antecipado (Early Cross):** L1 + qualquer cruzamento (console).
- Fonte (console): https://realsport101.com/article/efootball-2022-controls-tricks-playstation-ps4-ps5-xbox-one-series-x-mobile
- **Atributo:** Lofted Pass determina a velocidade e a altura da trajetória (v4.4.0) e também a precisão dos cruzamentos altos.
- **Força:** Kicking Power influencia o alcance. Pouca força de chute deixa o passe fraco mesmo com a barra cheia (fan guide).

### 1.5 Stunning Pass
- **Console:** apertar R2 até o fundo enquanto carrega o passe rasteiro, alto ou through. A bola viaja mais rápido. Fonte: https://pesmastery.com/efootball-passing-tutorial/
- **Mobile Classic:** gesto de "slide" (deslizar) no botão. **Não achei o mapeamento exato atual.**

### 1.6 O que o recebedor faz
- Nenhuma fonte descreve o algoritmo. A observação comum (e o conselho "mire à frente do companheiro") indica que o recebedor de um passe rasteiro dá alguns passos em direção à bola ou à trajetória para interceptá-la. No through ball, ele mantém a corrida.
- **Correção na v4.4.0:** foram reduzidos os casos em que a bola passava entre as pernas do defensor sem ser cortada. Isso mostra que a interceptação é física, por colisão com o corpo e as pernas, e não um sorteio.
- **[ESTIMATIVA] Comportamento sugerido:**
  1. Ao sair o passe, o recebedor calcula o ponto de interceptação mais cedo alcançável na trajetória e corre para ele.
  2. Se houver muita folga, ele fica parado ou se posiciona com o corpo aberto.

### 1.7 "Pass & Move"
- Segurar R2 antes de passar faz o passador arrancar após o passe (PES, guia antigo). Fonte: https://pesmastery.com/pes-passing-tutorial/
- **"Pass Search Guide":** opção mobile (seção Touch & Flick) adicionada na v4.4.0. **Não achei descrição do que faz.** Pelo nome, deve ser um indicador visual de alvo de passe (inferência).

---

## 2. FINALIZAÇÃO

### 2.1 Barra de força
- **Como funciona:**
  - Segurar Shoot enche a barra. O chute sai ao soltar ou ao encher.
  - A barra é **amarela** no chute normal e fica **roxa** quando você acerta um Stunning Shot. Fonte: https://pesmastery.com/efootball-shooting-tutorial/
- **Mecânica (efootballexplained):**
  - **Velocidade do chute** = Kicking Power + fração da barra usada.
  - Kicking Power **não** altera o erro de direção ou de altura.
  - **Usar mais barra custa precisão.**
  - Finishing não dá velocidade. Ele torna velocidade e direção mais consistentes e reduz o quanto do "erro máximo possível" o chute usa.
  - A pontuação de precisão ganha **2,7 pontos por ponto de Finishing entre 60 e 89**, e **1,1 a partir de 90**.
  - Fonte: https://efootballexplained.com/stats/fin/
- **Força recomendada (guia):**
  - Chute colocado (controlled) em 1x1: 1/4 a 1/2 da barra.
  - Da entrada da área: 1/2 a 3/4.
  - Pênalti: 70–90% (opinião de usuário, PES 2020).
  - Fontes: https://pesmastery.com/efootball-shooting-tutorial/ , https://steamcommunity.com/app/996470/discussions/0/2915472677722526564/
- **Tempo de enchimento:** **não encontrado em nenhuma fonte.** **[ESTIMATIVA]** De 0 a 100% em cerca de 0,9–1,1 s, linear. Os jogos PES/eFootball historicamente têm uma barra de cerca de 1 s. Faixa útil: 25–75%. Acima de cerca de 85%, a bola sobe por cima do gol com frequência fora da área.

### 2.2 Tipos de chute
- **Controlled / finesse (colocado):** console R2 + Shoot. Menos força, mais precisão.
- **Chip (cavadinha):** console L1 + Shoot. O mapeamento varia por fonte.
- **Mobile Touch & Flick:** "Tap R side & flick" (2022).
- Fontes: https://www.fifplay.com/efootball-2026-controls/ (via busca), https://realsport101.com/article/efootball-2022-controls-tricks-playstation-ps4-ps5-xbox-one-series-x-mobile
- **Stunning Shot:**
  - **Entrada:** slide do botão Shoot para a esquerda ou a direita (mobile), ou R2 até o fundo + Shoot (console).
  - **O tipo depende da barra** (lista de skills ef-labo). Fonte: https://ef-labo.com/efootball/player-skills/

    | Skill do jogador | Barra ao comandar o Stunning Shot | Resultado |
    |---|---|---|
    | Dipping Shot | 20–50% | chute com queda |
    | Knuckle Shot | 50–65% | chute de chapa (knuckle) |
    | Rising Shot | 65–95% | chute que sobe |
    | (geral) | abaixo de 50% | chute mais veloz |

  - **Snap Strike (skill):** Stunning Shot com a barra em 40–50% e menos tempo de preparação do chute. Fonte: https://www.sportsdunia.com/esports/efootball-snap-strike-skill
- **Power Shot (console):** L1 + R1 + Shoot. Animação mais longa e mais forte. A skill Power Shot+ encurta a animação. Fonte: TikTok @betclicapogee (https://www.tiktok.com/@betclicapogee/video/7392675434938371360)
- **Dynamic Volley (v6.0.0):** Stunning Shot com a bola no ar. O Smart Assist pode acionar sozinho em bolas altas na frente do gol. Fonte: https://www.konami.com/efootball/en/page/v6/versioninfo_v6-00
- **Curvas por skill:** Long-range Curler dá até **+3 km/h**, Blitz Curler até **+4 km/h** (crescem com Kicking Power). Fonte: https://efootballexplained.com/skills/blitz-curler/
- **Cabeçada:** é Shoot com a bola alta na área. A skill "Bullet Header" dá potência e precisão mesmo desequilibrado. Sem números.

### 2.3 Direção do joystick
- O stick esquerdo no momento do chute define o canto. Com assistência, o canto é corrigido para dentro do gol. No manual, o erro sai do atributo.
- O manual Konami cita que "inclinar mais" durante o chute com curva enche mais a barra (trecho do manual). Detalhe não confirmado.

### 2.4 Precisão afetada por fatores
- **Defensores perto:** reduzem a precisão e a força. Fonte: https://pesmastery.com/efootball-shooting-tutorial/
- **Pé fraco:** dois atributos, Usage (frequência) e Accuracy (4 níveis: Low/Medium/High/Very High).
  - Um guia (playgoals, não específico do eFootball) dá um multiplicador: Finishing 87 × 0,77 = 67 no pé fraco, mais 0–10% de perda de força. **Fórmula incerta para o eFootball.**
  - Fonte: https://playgoals.com/en/stats-guide
- **Stamina abaixo de 20%:** Finishing, passe e Kicking Power caem até 5 pontos. Fonte: https://efootballexplained.com/stats/stamina/
- **Bola que chega rápido (chute de primeira):** mais erro. Analogia com o One-touch Pass (bola tratada como 10% mais lenta). Para chute existe a skill "First-time Shot".

---

## 3. DOMÍNIO E RECEPÇÃO

- **Domínio automático:** o recebedor domina sozinho e a qualidade depende de **Ball Control** (limpeza do primeiro toque e qualidade das fintas). Fonte: https://efootballexplained.com/stats/
  - Um guia cita que com Ball Control 80 um flick limpo sai em cerca de 81% das vezes, contra cerca de 64%. A base exata é incerta e o número tem baixa confiabilidade. Fonte: busca sobre efootballedge (https://efootballedge.com/en/articles/advanced-efootball-mechanics)
- **Direção do domínio:** o stick esquerdo segurado na recepção orienta o primeiro toque. Domínio virado para o gol (console 2022): empurre e solte o LS e segure R2 antes do domínio.
- **Finta de domínio (trap feint):** solte o LS e segure R1 antes de dominar.
- **Deixar a bola passar ("let ball run"/dummy):** **não encontrei** comando confirmado no eFootball atual. As fontes que citam são do EA FC.
- Fonte das recepções (console 2022): https://www.fifplay.com/efootball-2022-controls/ (via busca)
- **No mobile Classic:** não achei o gesto de trap. **[ESTIMATIVA]** Com o stick numa direção = domínio orientado. Com o stick neutro = domínio amortecido parado. Com Dash segurado = domínio longo de arrancada.

---

## 4. CONDUÇÃO E DRIBLE

- **Andar e correr:**
  - Sem botão, a condução é normal. Com Dash segurado, é a arrancada (dash dribble), com toques mais longos.
  - Velocidade com bola = Speed + **Dribbling**.
  - Desde a v5.0.0, Dribbling e Acceleration determinam a aceleração com bola.
  - Fontes: https://efootballexplained.com/stats/ , https://www.konami.com/efootball/en/page/v5/versioninfo_v5-00
- **Tight Possession:** muda o **timing**, não a velocidade máxima. Encurta o intervalo entre toques e acelera o ritmo de toques nas viradas. O ganho é maior entre 60 e 90 e quase nulo acima de 90. Fonte: https://efootballexplained.com/stats/tp/
- **Distância da bola ao pé:** Dribbling **não** muda essa distância (efootballexplained).
- **Sharp Touch:** toque forte seguido de arrancada para deixar o defensor para trás.
  - Console: pressionar e soltar R2 até o fundo dirigindo o LS.
  - Mobile: o manual lista "Dash Dribble ⑩+⑥" e "Sharp Touch ⑩+⑥x2", ou seja, Dash + direção duas vezes ou toque duplo.
  - A skill "Acceleration Burst" desbloqueia o Sharp Touch.
  - A v5.4.0 corrigiu a bola que "flutuava" no Sharp Touch e deixou a resposta e o empurrão da bola mais consistentes.
  - Fontes: https://www.konami.com/efootball/en/page/new_controls , https://www.konami.com/efootball/en/page/mobile_controller , https://www.konami.com/efootball/en/page/v5/versioninfo_v5-40
- **Double Touch (mobile):** toque em Dash + flick rápido do stick na direção para onde o jogador está virado. Ou "slide Dash 90° para a esquerda ou direita da direção do corpo". Fonte: https://gamingonphone.com/guides/efootball-2024-a-complete-dribbling-guide-to-learn-the-fundamentals-and-skill-moves
- **Finesse Dribble (v4.0.0):** mantém o corpo virado para a direção de ataque e dá toques delicados, bom em espaço apertado.
  - Teste da comunidade: não aumenta a velocidade, é afetado por Tight Possession e não por Ball Control/Dribbling.
  - Fonte: https://www.konami.com/efootball/en/page/v4/versioninfo_v4-00 e resumo de testes em r/eFootball
- **Proteção de bola:** o comando Shield foi **removido** na v4.0.0. O jogador protege **automaticamente** com o corpo. Em disputa física, vence o maior **Physical Contact**. Fonte: https://www.konami.com/efootball/en/page/v4/versioninfo_v4-00
- **Super Cancel:** R1 + R2 cancela instantaneamente qualquer ação comandada (chute, passe, corrida automática para a bola). Fonte: https://efootballedge.com/en/articles/advanced-efootball-mechanics , https://www.redbull.com/gb-en/efootball-pes-2020-tips-guide
- **Fintas (skill moves):** no mobile, flick do stick ou gestos. Há a opção "Advanced Feint" no menu. O fan guide recomenda para jogo competitivo. Sem números. Fonte: https://gamingonphone.com/guides/efootball-2026-a-guide-to-perform-feints-and-sombrero/
- **IA defensora (v5.0.0):** o defensor passa a tentar menos o roubo quando o atacante se afasta dele.
- **[ESTIMATIVA] Cadência de toques:**

  | Situação | Intervalo entre toques | Distância bola–pé |
  |---|---|---|
  | Normal | 0,35–0,5 s | 0,6–1,0 m |
  | Dash | 0,6–0,9 s | 1,5–3 m |
  | Sharp Touch | toque único | 4–6 m |

  Tight Possession reduz o intervalo em até cerca de 30%.

---

## 5. DEFESA

- **Match-up:**
  - **Como funciona:** o defensor controlado se coloca entre o atacante e o gol, de frente para ele, bloqueando passes e chutes. É um "jockey" ativo.
  - **Mobile Classic:** slide do botão **Pressure** para a esquerda ou a direita. Console: L2 + LS.
  - Fontes: https://www.konami.com/efootball/en/page/new_controls , https://www.fifplay.com/efootball-2027-controls/
  - Os guias dizem que defender no eFootball é um **sistema de atraso**, não de roubo. O Match-up segurado é o hábito defensivo mais valioso. Fonte: https://www.footymeta.com/efootball/gameplay/how-to-defend/
  - A v6.0.0 alterou o timing de reação da IA associado ao Match-up. Fonte: https://gamemarket.gg/news/efootball/efootball-v6-0-0-team-playstyle-guide-every-option-explained
  - **Auto-tackle:** em Match-up, o defensor fica mais ágil e tenta o desarme e a interceptação automaticamente. Fonte: https://www.fifplay.com/efootball-2027-how-to-defend-better/ (via busca)
- **Pressure (Press):** segurar Pressure faz o jogador controlado correr até o portador e tentar desarmar. Console: X/A. Segurar o botão de desarme é melhor que tocar duas vezes. Fonte: https://careergamers.com/efootball-how-to-defend-complete-guide/
- **Call for Pressure (segundo homem):** slide do botão Pressure **para cima** (mobile Classic). Console R1. Um companheiro controlado pela IA vai pressionar o portador. O risco é desmontar a linha. Fontes: https://inquisitiveuniverse.com/2022/05/30/efootball-mobile-new-controls/ , https://gamemarket.gg/news/efootball/efootball-defending-guide-pressure-match-up-and-tackling
- **Sliding Tackle (carrinho):** botão Tackle no mobile, ou O/B no console. Alto risco.
- **Shoulder Charge (carga de ombro):** Square/X no console.
- **Defensive Engagement (v5.0.0):** a transição de ataque para defesa ficou mais rápida, e a rapidez depende do atributo **Defensive Engagement**.
- **Reação a passes (v6.0.0):** o timing de reação do defensor ao passe ficou levemente mais lento no próprio campo.
- **Troca de jogador (Cursor Change):**
  - **Opções:** Assisted (troca para jogadores perto da bola), Semi-Auto (equivale ao Assisted antigo, pré-v2.2.0) e Manual.
  - **Manual:** botão Switch (mobile) / L1 (console), ou stick direito para troca direcional (console).
  - Fontes: https://www.konami.com/efootball/en/page/2023/season2_patch-notes_v2-20 , https://efootballlab.com/blog/best-efootball-mobile-settings.html
  - **[ESTIMATIVA] Regra de quem é escolhido:**
    1. Defensor com o menor tempo estimado para interceptar a bola ou o portador.
    2. Desempate pelo que está entre a bola e o gol.
    3. Na troca automática, histerese de cerca de 0,5 s para não pular.
    4. Nunca trocar durante uma ação comandada (tackle).
- **Interceptação:** é física, por colisão com o corpo e as pernas (correção da v4.4.0 sobre bola entre as pernas). Não é probabilística pura.
- **Linha de impedimento:** existe opção Offside Trap no plano (um guia de 2026 sugere ligar em estilos de linha alta como Possession e Quick Counter). Possession Game mantém a linha um pouco mais alta. Fontes: https://gamingonphone.com/guides/efootball-guide-to-the-best-gameplay-and-game-plan-settings-for-a-smooth-experience/ , https://pesmastery.com/?p=8355 . **Não achei** o comportamento exato da IA.
- **Smart Assist na defesa:** afastamentos automáticos perto do próprio gol.

---

## 6. GOLEIRO

- **Atributos:**
  - **GK Awareness:** posicionamento e antecipação antes do chute, e redução do ângulo do atacante.
  - **GK Catching:** segurar a bola em vez de soltar o rebote, inclusive chutes mais fortes.
  - **GK Parrying:** para onde a bola vai ao espalmar, de preferência fora das zonas perigosas.
  - **GK Reflexes:** reação a chutes de perto e 1x1.
  - **GK Reach:** alcance do mergulho e área do gol coberta.
  - Fontes: https://www.fifplay.com/efootball-2026-attributes/ , https://gamingonphone.com/guides/efootball-2023-a-complete-guide-to-understanding-attributes
  - Comunidade: Reach > Reflexes > Awareness >>> Parrying > Catching (opinião). Fonte: r/pesmobile (https://redlib.ssps.io/r/pesmobile/comments/1k5d26f/tell_me_the_stat_that_matters_most_for_a)
- **Estilos de goleiro:** Offensive Goalkeeper (sai mais) e Defensive Goalkeeper (fica na linha). São playing styles do jogo, sem números públicos.
- **Controle:** console tem "rush" (sair do gol) e troca manual para o goleiro (L1 + R3). Opinião de guia: a IA deixa o segundo pau mais exposto. Fonte: https://pesmastery.com/efootball-goal-keeping-tutorial/
- **Mobile:** **não achei** comando de saída do goleiro no mobile.
- **v6.0.0:** novas animações de goleiro.
- **[ESTIMATIVA] Comportamento sugerido:**
  - **Posicionamento:** sobre a bissetriz do ângulo bola–traves, a 1–4 m da linha conforme a distância da bola. Avança até cerca de 6–8 m em 1x1.
  - **Saída:** quando a bola solta ou o through cai numa zona em que ele chega antes do atacante (compare os tempos de chegada), com margem que cresce com Awareness.
  - **Defesa:** tempo de reação base de cerca de 0,18–0,30 s, que cai com Reflexes. Alcance lateral de mergulho de 2,5–3,2 m conforme Reach.
  - **Resultado:** probabilidade de segurar versus espalmar pela velocidade do chute versus Catching.

---

## 7. CÂMERA

- **Tipos disponíveis:**
  - Dynamic Wide, Wide, Long, Fan's View, Live Broadcast / Broadcast, Stadium (Custom). Cada uma tem Zoom, Height e Angle personalizáveis.
  - Fontes: https://www.magicgameworld.com/efootball-2022-how-to-change-customize-your-camera-angle/ , https://www.pesmodding.com/2022/04/efootball-22-camera-settings-tutorial.html
- **Valores relatados por jogadores (anedóticos):**
  - Dynamic Wide Custom com Zoom 5 e Height 0 (pesmodding).
  - Stadium Custom com Angle 6, Height 0, Zoom 3 (comentário no YouTube https://www.youtube.com/watch?v=wy0Qq1BtbZQ ).
- **Escala:** as escalas são de passos inteiros no menu, provavelmente 0–10. **Não confirmado.** **Não achei** o padrão do mobile nem parâmetros de seguimento.
- **Dynamic Wide:** visão ampla, popular para enxergar companheiros. É a câmera comum no mobile (observação geral, sem fonte numérica).
- **[ESTIMATIVA] Parâmetros para uma câmera "Dynamic Wide" mobile:**
  - **Enquadramento:** lateral elevada, olhando o campo de lado. Pitch da câmera de cerca de 25–35° abaixo do horizonte. Distância horizontal até a linha da bola de cerca de 35–50 m. Altura de cerca de 18–28 m. FOV vertical de cerca de 30–40°.
  - **Seguimento:** a câmera segue um ponto-alvo = bola + 15–25% à frente na direção do ataque (look-ahead) e para o lado do gol atacado.
  - **Suavização:** filtro de 2ª ordem (mola crítica, tempo de cerca de 0,4–0,6 s em X, mais lento em Y lateral).
  - **Zoom:** abre (de 5 a 10% mais distante) em bolas longas e quando a bola está alta.
  - **Ataque vs. defesa:** a diferença é só o look-ahead, que sempre aponta para o gol que o time da bola ataca. A transição é suave em cerca de 1 s após a troca de posse.

---

## 8. HUD E ASSISTÊNCIAS DO MOBILE

- **Esquemas:**
  - **Classic:** joystick virtual à esquerda, fixo ou móvel ("Moveable Directional Stick"). À direita, 4 botões grandes e contextuais:
    - **Ataque:** Pass, Through, Shoot, Dash.
    - **Defesa:** Switch (Cursor Change), Pressure, Tackle, Dash.
  - **Gestos sobre os botões:**
    - Flick para cima ou para baixo em Pass = passe alto/cruzamento.
    - Flick para cima ou para baixo em Through = through alto.
    - Flick para a esquerda em Through = Stunning Through.
    - Slide em Shoot = Stunning Shot.
    - Slide lateral em Pressure = Match-up.
    - Slide para cima em Pressure = Call for Pressure.
    - Toque duplo/gesto em Dash = Sharp Touch / Double Touch.
  - **Advanced (Touch & Flick):** sem botões. Toque no lado direito = passe. Toque e flick = chute. Segurar no lado direito = Dash. Segurar no lado esquerdo na defesa = Dash + Match-up.
  - Fontes: https://www.konami.com/efootball/en/page/new_controls , https://inquisitiveuniverse.com/2022/05/30/efootball-mobile-new-controls/ , https://realsport101.com/article/efootball-2022-controls-tricks-playstation-ps4-ps5-xbox-one-series-x-mobile , https://efootballlab.com/blog/best-efootball-mobile-settings.html
- **Opções de jogo (mobile):**
  - **Cursor Change:** Assisted / Semi-Auto / Manual.
  - **Smart Assist (on/off):**
    - Ajusta a força do chute.
    - Ajusta a direção do drible para evitar quem se aproxima e corrige perto da linha lateral.
    - Pode trocar o alvo ou o tipo do passe.
    - Faz afastamentos automáticos.
    - Faz o Dynamic Volley automático (v6).
  - **Pass Search Guide** (Touch & Flick).
  - **Feint:** Advanced Feint.
  - **Opacidade dos botões.**
  - Fontes: https://www.konami.com/efootball/en/page/overview , https://operationsports.com/efootball-update-4-0-0-adds-new-finesse-dribble-physical-contact-improvements-smart-assist-and-more , https://gamingonphone.com/guides/efootball-guide-to-the-best-gameplay-and-game-plan-settings-for-a-smooth-experience/
- **HUD:**
  - Barra de força nos pés do jogador.
  - Cursor sobre o jogador controlado.
  - Radar/minimapa (usado como apoio nos passes manuais).
  - Placar e tempo.
- **Controle físico no mobile:** só funciona em modos contra a IA. Fonte: https://gamingonphone.com/guides/efootball-2026-controller-controls-and-button-mapping-guide/
- **Auto-jogo:** **não achei** confirmação de um modo de auto-play nas partidas mobile do eFootball atual. Não confundir com o FC Mobile.
- **[ESTIMATIVA] Layout:** botões de 70–90 dp, Shoot o maior, no canto inferior direito. Pass à esquerda dele, Through acima, Dash à esquerda de Pass. Rótulos e ícones trocam no ganho e na perda da posse com uma transição de cerca de 150 ms.

---

## 9. RITMO E FÍSICA

- **Velocidade máxima (efootballexplained, engenharia reversa):**
  - Base de cerca de **26,5 km/h**, mais 0,5 a 6 km/h conforme o Speed.

    | Speed | Velocidade máxima | Em m/s |
    |---|---|---|
    | 40 | 27,0 km/h | 7,5 m/s |
    | 80 | 30,5 km/h | 8,5 m/s |
    | 99 | 32,5 km/h | 9,0 m/s |
    | 99 + boosters | cerca de 33,8 km/h | cerca de 9,4 m/s |

  - Os pontos de 60 a 89 são os que mais rendem. Acima de 90, cada ponto vale cerca de 40%.
  - Fontes: https://efootballexplained.com/stats/speed/ , https://efootballexplained.com/stats/
- **Aceleração:** muda só o tempo até a velocidade máxima, não a máxima.
  - Com Speed 80, cada ponto de Acceleration entre 60 e 89 economiza cerca de **0,03 s**. Entre 40 e 59, cerca de 0,007 s.
  - Outra fonte (efootball.cz, modelo e não medição) diz que Acceleration 80→90 corta cerca de 0,4 s. As fontes não batem.
  - Fontes: https://efootballexplained.com/stats/accel/ , https://efootball.cz/en/novinky/acceleration-thresholds-decoded
- **Velocidade com bola:** Speed + Dribbling.
- **Stamina:**
  - O sprint é o principal dreno. Com Stamina 40, o jogador perde cerca de **5,4% do tanque por 1% do relógio da partida em sprint**. Com 80, 2,2%. Com 99, 1,5%.
  - A duração da partida não muda a taxa (o tanque escala com a duração).
  - No intervalo, há recuperação fixa por posição (com Stamina 80): cerca de 14% para atacantes, pontas e goleiros, 42% para zagueiros, 47% para laterais e meias.
  - Abaixo de 20%, os atributos de chute e passe caem até 5 pontos.
  - Fonte: https://efootballexplained.com/stats/stamina/
- **Duração da partida:** **não confirmada** pelas fontes. **[ESTIMATIVA]** 10 minutos reais no PvP mobile padrão (5 por tempo), relógio do jogo ×9.
- **Velocidade do jogo:** **não achei** número oficial.
- **[ESTIMATIVA] Valores para implementar:**
  - **Andar:** cerca de 1,5–2 m/s.
  - **Trote/condução normal sem Dash:** cerca de 4,5–6 m/s (55–65% da máxima).
  - **Dash:** 7,5–9,4 m/s.
  - **Aceleração 0→máx:** cerca de 2,5–3,5 s, curva exponencial v(t) = vmax·(1−e^(−t/τ)), τ de cerca de 0,9–1,3 s conforme Acceleration.
  - **Desaceleração:** cerca de 6–9 m/s².
  - **Raio de virada:** cai com a velocidade. Em dash, penalidade de 0,15–0,3 s para virar mais de 90°.
  - **A 30 Hz:** dt = 0,0333 s, então 9 m/s = 0,3 m por tick. A bola a 30 m/s anda 1 m por tick, o que exige teste de colisão contínuo (swept) para interceptações.

---

## 10. IA DOS COMPANHEIROS

- **Team Playstyle** (define como os 10 da IA se movem ao recuperar a bola):
  - **Possession Game:** apoio curto, retém a bola, evita correr para o espaço, linha defensiva um pouco mais alta.
  - **Quick Counter:** linha alta, pressão forte, corredores atacando as costas da defesa logo após o roubo.
  - **Long Ball Counter:** bloco baixo e compacto, bola longa ao recuperar.
  - **Out Wide:** jogo pelas pontas, isolando laterais e cruzando.
  - **Long Ball:** bloco muito baixo e pressão forte, bola direta.
  - **Overload ("Outnumbered", v6.0.0):** novo, cria superioridade numérica. A v6 também trouxe "Fluid Formations".
  - Fontes: https://gamemarket.gg/news/efootball/efootball-team-playstyles-explained-how-each-tactic-works , https://inquisitiveuniverse.com/2023/04/03/managers-playstyles-in-efootball-explained/ , https://gamemarket.gg/news/efootball/efootball-v6-0-0-team-playstyle-guide-every-option-explained , https://mmoculture.com/2026/07/efootball-v6-0-adds-overload-fluid-formations-and-tournaments/
- **Playing Styles individuais (sem bola):**
  - Goal Poacher (CF): vive no ombro do último defensor.
  - Fox in the Box: muito dependente de movimentação sem bola.
  - Deep-lying Forward, Roaming Flank, Prolific Winger, Box-to-Box, Orchestrator, Hole Player, Build Up (zagueiro que recua para receber do goleiro), Offensive/Defensive Full-back, Classic No. 10 (a v6 removeu a penalidade defensiva).
  - Fontes: https://www.fifplay.com/efootball-2026-playing-styles/ , https://gamingonphone.com/guides/efootball-playstyle-guide-for-defenders
- **AI Playing Styles:** governam o comportamento **com bola** quando a IA controla o jogador (dribles, corridas, cruzamentos, passes longos, chutes de longe). Fonte: https://gamingonphone.com/guides/efootball-guide-to-understanding-ai-player-playing-styles
- **Individual Instructions:**
  - **Ataque:** Anchoring (fica central), Counter Target (fica na frente, não recua), Deep Line (recua para ajudar).
  - **Defesa:** Tight Marking, Man Marking (só durante a partida).
  - Fonte: https://gamingonphone.com/guides/efootball-2025-a-complete-guide-for-individual-instructions
- **Problemas relatados:** corredores que param e voltam à posição e falta de corridas inteligentes (anedótico, Steam). Fonte: https://steamcommunity.com/app/1665460/discussions/0/3383914261427712650
- **[ESTIMATIVA] Modelo sugerido:**
  - Cada jogador tem uma posição-base da formação, transformada pelo bloco (ver o modelo de bloco na seção 6 da especificação).
  - Mais um deslocamento por papel: apoio a 8–15 m do portador em ângulo de passe livre, corrida em profundidade disparada quando há espaço atrás da linha e o portador está de frente.
  - Corredores respeitam a linha de impedimento até o momento do passe. Reavaliar a cada 5–10 ticks.

---

# Especificação para implementar (prioridade decrescente)

1. **Botões contextuais Classic:**
   - **Ataque:** Pass / Through / Shoot / Dash. **Defesa:** Switch / Pressure / Tackle / Dash.
   - Gestos de slide e flick sobre os botões para as variantes (passe alto, through alto, Stunning, Match-up, Call for Pressure).
   - Trocar o rótulo no instante da mudança de posse.
2. **Barra de força única para passe e chute:**
   - Enche linear enquanto o botão está pressionado ([EST] cerca de 1 s para 100%). Dispara ao soltar ou no máximo.
   - **Passe:** a força só ajusta a distância em relação ao alvo assistido.
   - **Chute:** a força dá a velocidade (com Kicking Power) **e** aumenta o erro (com Finishing). Erro ≈ f(barra) × (1 − precisão), mais penalidades por pressão, pé fraco e ângulo.
   - Barra amarela, roxa no Stunning.
3. **Assistência de passe por cone:**
   - Escolher o companheiro em um cone de cerca de ±30–45° [EST] em torno da direção do stick.
   - Pontuação = alinhamento angular × distância × linha de passe livre.
   - Corrigir a força para o alvo.
   - Through = alvo à frente do recebedor projetado pela velocidade dele.
4. **Recebedor ativo:** ao sair o passe, o recebedor vai ao ponto de interceptação. Domínio automático cuja qualidade sai de Ball Control. A direção do stick orienta o primeiro toque.
5. **Movimento com curva de aceleração exponencial:**
   - Velocidade máxima 7,5–9,4 m/s por Speed (tabela da seção 9).
   - Com bola: Speed + Dribbling. Dash como modo explícito.
   - Toques mais longos em Dash. Tight Possession encurta o intervalo de toques.
6. **Match-up como pilar da defesa:**
   - O defensor controlado se coloca na linha portador–gol a cerca de 1,5–2,5 m [EST], de frente, espelhando os movimentos.
   - Auto-tackle quando a bola se afasta do pé do atacante.
   - Pressure = correr até o portador. Call for Pressure = 2º defensor da IA pressionando.
7. **Interceptação física:** a bola colide com cápsulas de pernas e corpo, com teste contínuo por tick a 30 Hz (a bola pode andar mais de 1 m por tick).
8. **Troca de cursor:** modos Assisted / Semi-Auto / Manual, com escolha por tempo-de-chegada à bola e histerese.
9. **Câmera "Dynamic Wide" lateral elevada:** look-ahead no sentido do ataque, suavização por mola e zoom-out em bolas longas.
10. **Ações de cancelamento e explosão:** Super Cancel (interrompe a corrida automática para a bola e ações em carga). Sharp Touch (toque longo e arrancada).
11. **Proteção automática pelo corpo:** sem botão. Disputa decidida por Physical Contact.
12. **Stamina:** dreno proporcional ao tempo em sprint, inversamente à Stamina. Atributos de chute e passe −5 abaixo de 20%. Recuperação fixa no intervalo.
13. **Táticas por Team Playstyle:**
    - Parâmetros de bloco: altura da linha, compactação, intensidade de pressão e taxa de corridas em profundidade.
    - 5–6 estilos predefinidos, mais instruções individuais (Anchoring, Counter Target, Deep Line, Tight/Man Marking).
14. **Goleiro:** posicionamento pela bissetriz, saída por comparação de tempo de chegada, mergulho com alcance e tempo de reação por atributos.
15. **Smart Assist opcional:**
    - Corrige a força do chute.
    - Evita a saída de bola na linha lateral ao conduzir.
    - Troca o passe rasteiro bloqueado por um passe alto.
    - Faz afastamentos automáticos perto do próprio gol.
16. **Stunning Shot com o tipo dependendo da barra:** 20–50% Dipping, 50–65% Knuckle, 65–95% Rising, abaixo de 50% mais rápido. Dynamic Volley com a bola no ar.

# Coisas incertas e não encontradas

- **Gestos mobile atuais:** o mapeamento exato atual (v6) do Classic mobile para trap, Super Cancel, chip, chute colocado, cruzamento rasteiro e saída do goleiro. A maior parte das fontes de gesto é de 2022.
- **Números internos não publicados:**
  - Tempo de enchimento da barra de força.
  - Velocidades da bola por tipo de passe ou chute.
  - Ângulo do cone de assistência.
  - Raio de troca de cursor.
  - Distância do Match-up.
  - Tempos de animação.
  - Duração da partida no PvP mobile.
  - Parâmetros padrão de câmera no mobile.
- **Tempo de aceleração:** os valores divergem entre efootballexplained (cerca de 0,03 s por ponto) e efootball.cz (cerca de 0,4 s de 80 para 90).
- **Pé fraco:** a fórmula (multiplicador ×Accuracy) vem de uma fonte genérica e não foi confirmada para o eFootball.
- **Funções sem descrição:** o que o "Pass Search Guide" faz e se existe auto-play no mobile.
- **IA sem documentação:** a IA de linha de impedimento e o timing de corridas dos companheiros. Só há descrições qualitativas.
- **Bloqueio de acesso:** konami.com, fifplay.com, pesmastery.com, efootballexplained.com e efootballedge.com não puderam ser abertos nesta sessão (bloqueio de rede). Os dados atribuídos a eles vêm de trechos de busca. Vale reler essas páginas com acesso liberado, principalmente https://www.konami.com/efootball/en/page/mobile_controller (manual de controles mobile) e as páginas /stats/ do efootballexplained.com.
