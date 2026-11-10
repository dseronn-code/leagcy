# Tecnologia de jogos de futebol (pesquisa, 10/11/2026)

Fonte: trechos de busca (vários hosts primários bloqueados). [I] = incerto; [CP] = conhecimento prévio; números de
projeto marcados como tal. Conferir nos originais antes de virar constante.

## Ordem de implementação
1. **Simulação determinística separada da animação**: tick fixo 30 Hz, estado abstrato (22 "simulation bones", bola,
   posse, ações). A animação só segue. Base para rollback, replay, validação no servidor e testes.
2. **Rede**: lockstep por inputs + rollback (GGPO) com servidor relay/validador (não P2P puro). Plano B: servidor
   autoritativo com predição.
3. **Física da bola própria** (não corpo rígido genérico): arrasto, Magnus, decaimento de spin, quique, rolagem, modelo
   de toque. Jolt só para colisões jogador-jogador, ragdoll e traves.
4. **Motion matching + inertialization**, foot IK e IK de bola.
5. **IA de time**: posições estratégicas relativas à formação e à bola (SBSP/RoboCup) + utility AI com pitch control e xT.
6. **Mobile**: 30 fps garantido, 60 em topo; Swappy; ADPF térmico; torcida em impostores/VAT; **sem geometry shader**.

## Animação
- Motion matching: Clavet (GDC 2016, For Honor), "dance cards" de Zadziuk (GDC 2016). Learned Motion Matching (Holden
  et al., SIGGRAPH 2020). O3DE: matriz de features ~24 MB. Inertialization (Bollo, GDC 2018).
- EA HyperMotion (FIFA 22): 22 jogadores em Xsens, ~8,7 milhões de frames, ML-Flow ajusta passada perto da bola.
- eFootball: UE4 customizado, "Motion Matching" com 4x mais animações que o PES.
- Dados: LAFAN1 é CC BY-NC-ND (**não pode no jogo**); CMU "livre para comercial" [I]; Mixamo royalty-free (protótipo);
  ActorCore/Rokoko [I]; Soccer-X (arXiv 2411.16216, 10 h de futebol com bola rastreada, licença [I]). **Para qualidade
  FC/eFootball: captura própria** (Xsens/Rokoko/Move.ai), roteiro de locomoção, condução, recepções em vários ângulos,
  passes/chutes/cruzamentos em 3 forças, carrinhos, goleiro.
- Procedural: root separado + warping limitado; foot locking por contatos pré-processados; **toque na bola**: prever a
  bola no frame de contato (física analítica), escolher clipe e pé, warp de tempo ±10-15% e espaço, IK final; fora da
  tolerância = toque pesado. Facing ≠ direção (strafe). LOD de animação (busca a 10 Hz longe da bola).

## Física da bola
- Bola: raio ~0,11 m, 410-450 g [CP]. ρ ≈ 1,2 kg/m³ [CP].
- C_D: ~0,45-0,5 abaixo do crítico, ~0,15-0,2 acima (drag crisis); Brazuca Re crítico 2,2-2,5×10⁵ (~15-17 m/s,
  estimado); a 35 m/s C_D 0,16 (Brazuca), 0,20 (Jabulani). C_S ~0,2 (Goff & Carré 2009).
- Magnus: C_L cresce com S = rω/v [valores I — calibrar].
- Spin: cai ~2% em 0,8 s (5,8 → 5,7 rot/s); máx. ~16 rot/s.
- FIFA Quality Programme 2024: rebote de 2 m → 0,60-0,85 m (COR ≈ 0,55-0,65); rolagem de rampa 4-8 m.
- Modelo: F = −½ρA·C_D·|v|v + ½ρA·C_L·|v|²(ω̂×v̂) + mg; quique com COR e atrito de Coulomb (troca spin↔velocidade);
  rolagem com fase de deslizamento e de rolamento; integração semi-implícita a 120-240 Hz em subpassos.
- **Primeiro toque** (EA FC 26): velocidade relativa, altura, ângulo pedido, pressão, parte do corpo, esticada,
  atributos → erro amostrado com semente determinística.
- Assistência de passe: assistido/semi/manual; pontuar colegas por ângulo, distância, linha livre e xT; travar o nível
  por modo no ranqueado.

## IA de time
- SBSP (FC Portugal): posição-base da formação ajustada por bola, situação e tipo de jogador.
- Camadas: bloco do time (largura, profundidade, linha, deslocamento para o lado da bola) → slot de cada jogador →
  ações ativas por utility AI (pressionar, cobrir, marcar, interceptar, profundidade, apoio, terceiro homem) com
  histerese; behavior tree só para bola parada e goleiro. Gatilhos de pressão: passe para trás, receptor de costas,
  toque pesado, bola na lateral, passe lento.
- Pitch control (Spearman): controle pelo tempo de chegada; parâmetros de Laurie Shaw: 5 m/s, reação 0,7 s,
  tti_sigma 0,45; grade 32×20 a 5-10 Hz.
- xT (Karun Singh): grade 16×12, pré-calculada.
- Goleiro: bissetriz do ângulo bola-traves, saída por tempo de interceptação, defesa por ponto de interceptação.
- Google Research Football (2019) como banco de cenários.

## Rede
- eFootball: P2P em partida rápida/amigo; cliente-servidor em liga/eventos.
- Recomendado: **lockstep + rollback via relay**: tick 30 Hz, input delay 2-3 ticks, rollback até 6-8 ticks [projeto];
  IA estratégica a 5-10 Hz; snapshot mínimo sem animação.
- Determinismo: Jolt `JPH_CROSS_PLATFORM_DETERMINISTIC` (~8%), `-ffp-contract=off`, sem fast-math, trigonometria
  própria; checksum por tick; considerar ponto fixo.
- 4G: perda 1,7-5%, jitter 12-24 ms → enviar os últimos K inputs em cada pacote (8 bits de direção + botões); poucos kbps.
- Desconexão: IA assume, servidor decide. Anti-cheat: servidor re-simula com os inputs e assina o resultado; Play
  Integrity; matchmaking com limite de ping e relays regionais.

## Mobile
- FC Mobile: ~30/60/90-120 fps por aparelho. Alvo: 30 garantido, 60 nos topo; gameplay 30 Hz desacoplado.
- Swappy (frame pacing); ADPF (thermal headroom, performance hint) para escalar resolução, LOD, sombras, busca de MM.
- 22 jogadores com instancing por time, 3 LODs; torcida em impostores/VAT.
- **Vulkan: sem geometry/tessellation shader** (Mali); Android Vulkan Profiles; RT desligado no celular.

## Controles
- eFootball: Virtual Pad e Touch & Flick; fintas Easy/Smart/Advanced.
- Recomendado: joystick flutuante, 3-4 botões com força por tempo, gestos opcionais, botões contextuais na defesa,
  buffer de input de 150-200 ms [projeto]; meta de 100-150 ms do toque à reação [projeto]; Swappy; ≤ 2 frames em voo.

## Roteiro sugerido
Mês 1-2 simulação determinística + replay/checksum + testes da bola; 2-3 rollback e simulador de rede; 3-5 IA e
goleiro; em paralelo mocap e MM; 5+ torcida, LODs, térmico, controles, anti-cheat.
