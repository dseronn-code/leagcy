# FCLegacy Mobile — documento de projeto (v0.1, 10/11/2026)

Jogo de futebol 11 contra 11 para celular, no Legacy Engine. Jogabilidade no estilo eFootball, com a meta de ser
melhor que ele. Partidas de 6 minutos (2 tempos de 3, com acréscimos). Campanha online com gente de verdade: cada um
escolhe um clube, joga as competições reais dele, e dá para comprar e vender clubes (ideia do Empire Eleven, levada
para o multijogador).

Pesquisas completas em `pesquisa/` (01 eFootball/FC Mobile, 02 Empire Eleven, 03 competições e licenças,
04 tecnologia).

---

## 1. Pilares

1. **Jogo justo e responsivo.** O comando do jogador vale mais que a animação; sem "script"; rede com atraso baixo e
   visível; pareamento por habilidade, não por quem pagou.
2. **Futebol de verdade.** Bola com física real (arrasto, curva, quique, rolagem), toque na bola que depende de
   velocidade, ângulo e pressão, IA de time que se posiciona como time.
3. **Campanha com amigos que nenhum jogo de ação tem.** Liga persistente, competições reais do clube, temporada com
   tabela, mercado e donos de clube.
4. **Legado.** Cada clube guarda títulos, donos, presidentes e ídolos para sempre.

## 2. A partida (núcleo)

### Duração e regras
- 2 tempos de 3 minutos de relógio real (o cronômetro do jogo corre 15x: 45 min em 3 min), acréscimos pelo tempo de
  bola parada. Configurável fora do ranqueado (2x5, 2x8).
- Impedimento, faltas, cartões, pênaltis, laterais, escanteios, tiro de meta, substituições (5), disputa de pênaltis
  (reaproveita a mecânica do Pênalti da Treta).

### Controles (dois esquemas desde o começo)
- **Clássico:** joystick flutuante + botões com força por tempo de pressão: passe, passe em profundidade, chute, corrida;
  modificadores para passe alto/cruzamento, chute colocado, cavadinha, toque rápido. Na defesa: pressionar,
  contenção (match-up), carrinho, trocar jogador, goleiro sai.
- **Gestual:** toque no colega para passar, toque no espaço para profundidade, gesto no gol para chutar, flicks para
  fintas.
- Fintas por gesto (corte, elástico, chapéu/sombrero); primeiro toque direcional; ombro a ombro; "super cancel"
  (controle manual total).
- Assistência opcional que **só vale contra o computador ou em fila separada**.
- HUD editável (posição, tamanho, opacidade). Controle Bluetooth.

### Física da bola (própria, determinística)
Arrasto com crise de arrasto (C_D ~0,45 → ~0,15-0,2 acima de ~15-17 m/s), efeito Magnus para curva, spin que decai
devagar, quique com coeficiente 0,55-0,65 na grama e troca de spin com o chão, rolagem com fase de deslizamento.
Calibrada pelos testes da FIFA (rebote de 2 m → 0,60-0,85 m; rampa → 4-8 m de rolagem).

### Toque na bola
Erro amostrado em função da velocidade relativa bola/jogador, altura, ângulo pedido, pressão, parte do corpo, esticada e
atributos. Toque pesado é consequência, não sorteio escondido; a semente é registrada no replay.

### IA de time
Bloco do time (largura, profundidade, altura da linha, deslocamento para o lado da bola) definido pela tática; slots
dos jogadores na formação (fluida: uma de ataque, outra de defesa); ações por utilidade (pressionar, cobrir, marcar,
fechar linha, correr em profundidade, apoiar, terceiro homem) usando **pitch control** e **xT**. Gatilhos de pressão.
Goleiro próprio (ângulo, saída do gol, defesas pelo ponto de interceptação). Estilos de equipe: posse, contra-ataque
rápido, bola longa, pelas pontas, pressão total; ~20 estilos individuais.

### Animação
Fase 1: clipes de captura de movimento (Mixamo e afins) com transição suave, IK dos pés e IK do pé na bola — o que o
motor já tem e o Pênalti já usa. Fase 2: **motion matching com inertialization** e captura própria (roupa inercial ou
câmeras), seguindo o roteiro de locomoção, condução, recepções, passes, chutes, carrinhos e goleiro.
A animação nunca decide a jogada: ela segue a simulação.

### Simulação e rede
- **Simulação determinística a 30 Hz**, separada do desenho (60/120 fps interpolados). Mesmo resultado com as mesmas
  entradas: permite replay, rede por rollback, validação no servidor e testes automáticos.
- **Online 1x1:** troca só os comandos (poucos kbps), atraso de entrada de 2-3 ticks + rollback, passando por um
  servidor relay que também re-simula e assina o resultado (anti-trapaça). Ping e perda na tela. Desconexão: o
  computador assume e o servidor decide.
- **Sem script, provado:** replay com os comandos sobrepostos, log da partida, estatísticas (xG, duelos, posse).

## 3. Campanha (o diferencial)

### Começo: de técnico a dono
- Todo mundo começa como **técnico** de um clube (da IA ou de outro jogador). Salário e prêmios vão para a **carteira
  pessoal**, separada do caixa do clube.
- Com dinheiro e reputação, você faz **oferta por um clube**: direta ao dono, em leilão (24-48 h) ou no leilão de clube
  abandonado (dono inativo 14 dias).
- **Cotas:** o clube tem 100 cotas; quem tem mais de 50% é o presidente; sócios votam em estádio, venda e técnico e
  recebem dividendos.
- **Diretor de futebol com teto:** o dono delega negócios até um valor; acima disso, aprova.
- **Multiclube** (clube principal + satélite em outra divisão), com imposto em transferências entre eles.
- **Estádio próprio** pago com a carteira pessoal (obra em fases), que pode ser alugado a outro clube; ingresso por setor.
- **Marca própria** que patrocina só clubes **dos outros** (regra anti-dinheiro-circular).
- Cota de TV por divisão para o dono atual; prêmios das competições; parte das taxas do mercado vai para os prêmios.

### Competições reais
- Cada competição é um **arquivo de configuração** (grupos/potes, cruzamentos, jogo único/ida e volta, critérios,
  vagas, rebaixamento) — os formatos mudam todo ano.
- Temporada: estadual do clube (Paulistão, Carioca, Mineiro, Gaúcho, Nordeste…) → Brasileirão (Série A/B com playoff da
  B) → Copa do Brasil → Libertadores/Sul-Americana pelas vagas reais (regra 2027: G4 grupos, 5º pré, campeão da Copa do
  Brasil grupos, vice pré) → Supercopa/Recopa.
- **Mundo com número fixo de clubes**: os 20 da A, os 20 da B e os dos estaduais; clubes sem dono humano são da IA.
- Calendário compacto: uma temporada do jogo dura ~4 semanas reais; janelas de jogo marcadas com horário; quem não
  joga recebe resultado simulado (com aviso), como já fazemos na campanha do Pênalti.
- Histórico permanente do clube (títulos, donos, artilheiros).

### Jogadores
- Elencos com **notas próprias** calculadas de estatísticas reais (minutos, gols, idade, posição) ou pela comunidade.
  Não copiar notas da EA nem do Football Manager.
- Mercado com leilão com prazo, propostas, empréstimos, base (juvenis), staff (preparador, nutricionista, psicólogo).
- Evolução por partidas e treino (pontos de evolução), lesões, cansaço e moral.

### Social
Ligas privadas entre amigos (o modo principal), associações/federações, chat da liga, votações de sócios, ranking de
donos, torneios de fim de semana.

### Economia
Moeda só dentro do jogo; nada que vire dinheiro real. Sem pagar para vencer: nas ligas entre amigos há a opção
"elencos equilibrados". Se um dia tiver monetização, só conveniência e cosméticos, com taxas publicadas.

## 4. Nomes reais e licenças

- **Uso privado entre amigos** (APK fechado, sem cobrar): nomes reais de clubes e competições como texto; elencos
  carregados por um arquivo que o próprio grupo mantém (ou editor no jogo). Sem escudos oficiais nem rostos reais.
- **Lançamento público:** clubes, jogadores e competições fictícios por padrão, com editor; licenciar (FIFPRO/Fenapaf,
  clubes, CBF, CONMEBOL) se crescer. No Brasil, usar nome e rosto de atleta sem autorização dele gera processo (mais de
  20 vitórias contra EA/Konami, ~R$ 80 mil cada).

## 5. Tecnologia

- Legacy Engine (C++20, Vulkan). Novo módulo `fclegacy/sim`: simulação determinística em C++ puro, sem dependência do
  renderizador — roda no celular, no servidor e nos testes.
- Animação: `Model::layers` + IK do motor (fase 1); motion matching (fase 2).
- Rede: ENet do motor + rollback próprio; servidor relay/validador (Linux, mesmo código da simulação).
- Campanha: serviço de backend (contas, clubes, mercado, competições, calendário). Precisa de um servidor na internet
  (VPS ou serviço gerenciado) — decisão em aberto.
- Celular: 30 fps garantido, 60/120 nos topo de linha; Swappy; ajuste térmico (ADPF); torcida em impostores.
- **Bloqueio conhecido:** o motor hoje exige *geometry shader*, que a maioria das GPUs Mali (Exynos, Dimensity,
  Tensor) não tem. Para rodar nesses celulares é preciso trocar esse caminho do renderizador.

## 6. Etapas

| Etapa | Entrega | Como provar |
|---|---|---|
| **M0 — Núcleo** | Simulação determinística: bola, 22 jogadores (cinemática), posse, toque, passe, chute, regras básicas; visualização 2D de depuração | Testes automáticos (física da bola contra os números da FIFA, determinismo por checksum, replay) |
| **M1 — Partida jogável** | 11x11 contra o computador no celular: câmera de TV, controles clássicos, IA de time e goleiro, animações Mixamo, estádio do Pênalti | APK; partida de 6 minutos inteira |
| **M2 — Sensação** | Primeiro toque, fintas, defesa manual, bolas paradas, faltas e cartões, substituições; ajuste fino | Playtests com você e seus amigos |
| **M3 — Online 1x1** | Rollback + relay, ping na tela, anti-trapaça por re-simulação | Partida entre dois celulares no 4G |
| **M4 — Campanha** | Backend, contas, clubes, competições por configuração, calendário, mercado, compra e venda de clubes | Temporada de teste com o grupo |
| **M5 — Animação de verdade** | Motion matching + captura própria | Comparação lado a lado |
| **M6 — Acabamento** | Torcida, menus, narração/sons, desempenho, ajuste térmico | Celulares de entrada e topo |

## 7. Decisões em aberto
1. Modelo do seu celular e dos amigos (Adreno/Snapdragon roda hoje; Mali precisa da troca do renderizador).
2. Onde hospedar o servidor da campanha e do online.
3. Público-alvo: só o grupo de amigos (nomes reais) ou loja (fictício + editor).
4. Captura de movimento própria (roupa/câmeras) para a fase de animação de verdade.
