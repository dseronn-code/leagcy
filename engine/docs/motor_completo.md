# Motor completo: a camada de jogo

Parte 2 do projeto: completar o Legacy como motor de jogo, além do renderizador. As fases:

| Fase | O que entra | Situação |
|---|---|---|
| 1. Núcleo | Entidades e componentes (EnTT), cenas em arquivo (.json), física (Jolt), personagem, modelos glTF com colisão | ✅ |
| 2. Editor visual | Dear ImGui + ImGuizmo: entidades, propriedades, alças, seleção por clique, Jogar/Parar, criar/duplicar/apagar, salvar | ✅ |
| 3. Scripts | Lua 5.4 (sol2) por entidade, com recarga a quente | ✅ |
| 4. Sistemas de jogo | Áudio 3D ✅, veículos ✅, navegação e IA ✅, interface (HUD) ✅, exportar o jogo ✅, animação avançada ✅, entrada (teclado, mouse, controle), menus, pausa e salvar ✅, partículas de jogo ✅, multijogador ✅ | ✅ |

## Arquitetura

```
src/game/physics.*   Jolt Physics por trás de uma interface pequena (os cabeçalhos do Jolt ficam só no .cpp)
src/game/world.*     World: registro EnTT, componentes, leitura e gravação de cenas .json, sistemas por quadro
src/scene/*          renderizador: scene::Scene guarda o que é desenhado (objetos, malhas, materiais, luzes)
```

O `World` é dono das entidades. Cada entidade com `Renderable` aponta para um objeto da `scene::Scene`; a cada quadro
o `World::update` roda os personagens, avança a física e copia as poses só dos corpos acordados para a `Scene`
(`Scene::setTransform`). Parado, nada é reenviado à GPU. O mundo roda dentro do `Scene::update` (um "animador"), então
funciona igual no modo sem janela e na janela.

**Física:** passo fixo de 1/60 s com 2 subpassos de colisão (1/120 s) e 20/4 iterações do solver: o padrão do Jolt
deixava uma torre de 12 caixas inclinar 46 cm; assim ela fica a 0,17 cm. O resultado não depende da taxa de quadros e
é determinístico: a mesma cena dá o mesmo arquivo byte a byte, em várias threads.

## Editor (`--editor`)

`legacy --scene cenas/parque.json --editor` abre a cena em **modo de edição**: física parada, câmera livre (botão
direito + mouse olha, WASD anda). Clique num objeto para selecionar; arraste as alças para mover (F1 alterna para
girar) ou digite posição e rotação no painel Propriedades. **+ Caixa / + Esfera / + Parede** criam à frente da câmera;
**Duplicar** e **Apagar** agem na selecionada (a cena é gravada num arquivo temporário e montada de novo). **Jogar**
guarda todas as poses e liga a física (com jogador na cena, a câmera passa a segui-lo); **Parar** volta exatamente ao
que estava (teste `restaurar`: diferença 0 nas 36 entidades). **Salvar** grava a cena no arquivo indicado.

Captura para testes e documentação: `--screenshot tela.png --quit-after 10` (grava a janela com a interface) e
`--select nome`.

## Scripts em Lua

Na entidade: `"script": "scripts/elevador.lua"`. Cada entidade roda o arquivo no seu próprio ambiente (as variáveis
do script são só dela). O motor chama `iniciar(e)` uma vez e `atualizar(e, dt)` a cada quadro de jogo. Salvar o
arquivo com o jogo rodando recarrega na hora, mantendo as variáveis (teste `recarga`). Erro num script: aparece no
console uma vez e só aquele script para (teste `script`).

| API | O que faz |
|---|---|
| `e.nome`, `e:valida()` | nome da entidade; se ela ainda existe |
| `e:posicao()` | devolve x, y, z |
| `e:mover_para(x, y, z)` | põe a entidade na posição (corpo da física junto) |
| `e:girar(gx, gy, gz)` | gira em graus nos eixos do mundo |
| `e:impulso(x, y, z)` | empurrão (N·s) num corpo dinâmico |
| `e:velocidade()`, `e:definir_velocidade(x, y, z)` | velocidade em m/s (corpo ou personagem) |
| `achar("nome")` | outra entidade (ou nil) |
| `tempo()` | segundos de jogo |
| `log(...)` | escreve no console |
| `e:ir_para(x, y, z)`, `e:seguir(outra)` | personagem passa a andar sozinho pela navegação |
| `e:velocidade_frente()`, `e:marcha()`, `e:rpm()` | carro: m/s para a frente, marcha (−1 ré, 0 neutro), giro |
| `e:animar(clipe, transicao, laco)`, `e:parar_animacao(transicao)` | ação de animação com transição suave |
| `e:osso("nome")` | posição no mundo de um osso do modelo (x, y, z) |
| `hud.texto(t, x, y, {tamanho, cor, alinhar})` | texto na tela (ver Interface de jogo) |
| `hud.retangulo(x, y, l, a, cor)`, `hud.barra(x, y, l, a, fração, cor, fundo)` | retângulo e barra de progresso |

Exemplos: `scripts/elevador.lua` (sobe e desce), `scripts/girar.lua` (gira), `scripts/pulador.lua` (pula sozinho),
todos em `cenas/parque.json`.

## Áudio 3D (miniaudio)

Na entidade: `"som": {"arquivo": "sons/motor.wav", "volume": 1, "laco": true, "espacial": true, "dist_min": 1,
"dist_max": 100, "queda": 1, "tocar": true}` (WAV, MP3 ou FLAC). O som segue a entidade; o ouvinte é a câmera
(atenuação inversa pela distância, panorâmica pela direção, Doppler). Lua: `e:tocar_som()`, `e:parar_som()` e
`som("arquivo", x, y, z [, volume])` para um som avulso. Sem janela, `--audio-wav saida.wav` grava a mixagem.

Testes (`tools/audio.py`, tom de 440 Hz): a 2 m soa 4,002× mais alto que a 8 m (atenuação inversa prevê 4,0); uma
fonte à direita sai 5× mais alta no canal direito; sem fonte, silêncio total.

## Veículos (Jolt)

Na entidade: `"veiculo": {"comprimento": 4.2, "largura": 1.8, "altura": 0.9, "massa": 1300, "raio_roda": 0.35,
"largura_roda": 0.25, "direcao_maxima": 35, "torque": 500, "rpm_maximo": 6500, "tracao": "dianteira",
"controle": "jogador", "material_roda": "pneu"}`. Quatro rodas com suspensão, diferencial, barras
estabilizadoras, câmbio automático e freio de mão nas traseiras. Com `controle` `jogador`: W acelera, S freia e depois dá ré, A/D
vira, espaço é o freio de mão, e a câmera segue o carro. Ou `roteiro` com
`"roteiro": [{"t": 0, "acelerar": 1, "virar": 0, "freio": 0, "freio_mao": 0}]`. Exemplo: `cenas/carro.json`.

## Animação avançada (personagem com `"visual"`)

Um modelo glTF animado no lugar da cápsula do personagem:

```json
"personagem": {"altura": 0.8, "raio": 0.2, "velocidade": 1.5, "velocidade_correndo": 2.7,
  "visual": {"modelo": "assets/Fox/Fox.gltf", "escala": 0.01, "parado": "Survey", "andar": "Walk", "correr": "Run",
             "velocidade_andar": 1.5, "velocidade_correr": 2.7, "transicao": 0.2, "ik_pes": true,
             "pernas": [["b_LeftLeg01_015", "b_LeftLeg02_016", "b_LeftFoot01_017"], ...]}}
```

- **Mistura pela velocidade** (blend space 1D): parado → andar → correr, com pesos pela velocidade real do
  personagem. Andar e correr compartilham a fase (as patas batem juntas na transição) e tocam acelerados por
  velocidade/`velocidade_andar` (ou `_correr`), a velocidade em que o clipe foi feito: o pé apoiado não patina. A
  rotação acompanha a direção do movimento, suavizada.
- **Ações por script** com transição: `e:animar("nome", transicao, laco)` e `e:parar_animacao(transicao)`; sem laço,
  a ação sai sozinha no fim do clipe.
- **IK dos pés** (duas juntas por perna, analítico): cada pé desce ou sobe até o chão de verdade (raio da física
  abaixo dele, ignorando a própria cápsula), mantendo a altura que a animação dá ao pé; a bacia desce o necessário
  para o pé mais baixo alcançar, suavizada. Desligado no ar (as pernas seguem a animação).
- Sem `"desvio"`, o modelo é posto com o ponto mais baixo da pose parada no pé da cápsula.
- `e:osso("nome")` devolve a posição de um osso no mundo (para prender objetos, testes, efeitos).

Exemplo: a raposa de `cenas/parque.json` segue o jogador. Testes (`tools/animacao.py`, 120 quadros/s):

| Teste | Critério | Resultado |
|---|---|---|
| frente | andando para +X, a cabeça à frente da bacia | 1,000 (cosseno) |
| patinar | pata apoiada escorrega só o que o próprio clipe escorrega (previsto pelo clipe tocado no lugar) | andar 0,09/0,26 m/s (previsto 0,09/0,04); correr 0,13/0,79 (previsto 0,17/0,77) |
| mistura | andar → correr sem saltos maiores que os da corrida normal | 4,0 cm por quadro (andando 3,4; correndo 3,8) |
| acao | entrar e sair de uma ação no meio da caminhada | 2,5 / 3,4 cm por quadro (andando 3,4) |
| degrau | patas da frente sobre um degrau de 15 cm | com IK todas no chão (0,0 cm); sem IK, 15,0 cm dentro do degrau |
| rampa | parada numa rampa de 15° | com IK, pior pata a 1,4 cm; sem IK, 8,8 cm |

A raposa (modelo gratuito) tem limites do próprio clipe: na corrida a pata traseira direita pisa a 1,9 m/s e a
esquerda a 2,9 m/s, então nenhuma velocidade deixa as duas paradas; e o IK posiciona o tornozelo, sem girar o pé
para acompanhar a rampa (daí 1,4 cm nas patas de trás).

## Navegação e NPCs (Recast/Detour)

Personagem com `"controle": "ia"` anda sozinho pela malha de navegação: `"ia": {"alvo": [x, y, z]}` vai até um
ponto; `"ia": {"seguir": "nome"}` segue outra entidade; `"parar_a"` é a distância em que para (m). A malha é gerada ao
começar a partida a partir do cenário parado (caixas estáticas e modelos com `"colisao": "malha"`), no tamanho do
personagem (altura, raio, degrau e rampa máximos), então desvia de paredes, sobe escadas e evita o que é íngreme. O
caminho é recalculado a cada 0,5 s (o alvo pode andar). Lua: `e:ir_para(x, y, z)` e `e:seguir(outra)`.

## Interface de jogo (HUD)

Desenhada pelos scripts no `atualizar(e, dt)` de cada quadro, por cima da imagem (Dear ImGui, fonte vetorial
Roboto: nítida em qualquer resolução, com acentos). Posição e tamanho em fração da tela (0 a 1, a partir do canto de
cima à esquerda), então a mesma interface serve em 720p ou 4K; `tamanho` do texto em fração da altura da tela; cores
`{r, g, b, a}` de 0 a 1 em sRGB (as mesmas de um editor de imagem); `alinhar` = `esquerda`, `centro` ou `direita`.

```lua
function atualizar(e, dt)
  hud.texto("Vida", 0.03, 0.03, {tamanho = 0.035})
  hud.barra(0.03, 0.08, 0.25, 0.025, vida / 100, {0.2, 0.9, 0.3}, {1, 1, 1, 0.15})
end
```

Exemplo completo: `scripts/velocimetro.lua` (km/h, marcha e giro do motor em `cenas/carro.json`). Teste
(`tools/hud.py`, janela 800x450): retângulo, barra e fundo saem exatamente nas cores pedidas (255, 0, 0 / 0, 255, 0 /
0, 0, 255); a barra pela metade troca de cor em x = 0,250 (pedido 0,25); o texto centralizado fica com centro em
0,498.

## Entrada, menus, pausa e salvar o jogo

**Entrada** (teclado, mouse e controle; layout Xbox, qualquer controle que o GLFW reconheça): o jogador anda com WASD
ou o analógico esquerdo, gira a câmera com o mouse (botão direito) ou o analógico direito, pula com espaço ou A,
corre com Shift, L3 ou LB; no carro, RT acelera e LT freia. Nos scripts:

| API | O que faz |
|---|---|
| `tecla("w")`, `tecla_apertada("esc")` | segurada / apertada neste quadro (`a`..`z`, `0`..`9`, `espaco`, `esc`, `enter`, `tab`, `shift`, `ctrl`, `alt`, `cima`, `baixo`, `esquerda`, `direita`, `f1`..`f12`) |
| `mouse()`, `clique()`, `mouse_segurado()` | posição em fração da tela; clique neste quadro; botão segurado |
| `controle("a")`, `controle_apertado("start")`, `eixo("esquerdo_x")`, `tem_controle()` | botões `a b x y lb rb back start l3 r3 cima baixo esquerda direita`; eixos `esquerdo_x/y`, `direito_x/y`, `lt`, `rt` |
| `hud.botao(t, x, y, l, a)` | botão: verdadeiro no quadro em que é acionado (clique, Enter ou A com foco) |
| `hud.deslizar(t, x, y, l, a, valor, min, max)` | controle deslizante (arrastar, ou ←/→ com foco); devolve o valor |
| `hud.alternar(t, x, y, l, a, valor)` | Sim/Não; devolve o valor |
| `e:particulas(sim)`, `explosao(x, y, z, escala)` | liga/desliga as partículas da entidade; explosão na hora |
| `pausar(sim)`, `pausado()`, `tempo_real()` | pausa o jogo (física, personagens e `tempo()` param; os scripts continuam) |
| `volume(v)`, `opcao("motion_blur", sim)`, `opcao("dof", sim)`, `opcao("exposicao", ev)` | opções na hora |
| `qualidade("medio")` | troca o preset com o jogo rodando (o renderizador é refeito, a cena continua na GPU) |
| `salvar_jogo(nome, dados)`, `carregar_jogo(nome)`, `existe_jogo(nome)`, `sair()` | ver abaixo |

O foco dos controles anda com as setas, Tab ou o direcional; o mouse em cima de um controle também dá o foco.

**Menu de pausa padrão** (`scripts/menu_padrao.lua`, em toda cena com mundo): Esc ou Start abre Continuar, Salvar
jogo, Carregar jogo, Opções (volume, motion blur, qualidade Baixa/Média/Alta/Ultra) e Sair. `"menu": "meu.lua"` na
cena troca por outro script; `"menu": false` tira. Fora de um jogo com menu, Esc fecha o programa como antes.

**Salvar e carregar:** `salvar_jogo("slot", {pontos = 42, inventario = {...}})` grava `saves/slot.json` com a
posição, rotação e velocidade (linear e angular) de toda entidade com nome que se mexe (corpos, personagens, carros),
o tempo de jogo e os dados do script (números, textos, listas, tabelas). `carregar_jogo("slot")` aplica tudo no
mundo atual e devolve os dados.

**Testes sem janela:** `--simular "t:evento;..."` (`tecla`, `segurar`, `clique`, `mouse`, `eixo`) em segundos.
`tools/jogo.py`:

| Teste | Resultado |
|---|---|
| botao | clique no meio do botão B aciona só o B; clique fora, nada |
| teclado | ↓ + Enter aciona o 2º botão; Tab + Tab + Enter, o 3º |
| controle | direcional ↓ + A aciona o 2º botão |
| deslizar | clique a 75% da barra → 0,75; → com foco → 0,80 |
| alternar | clique inverte; Enter inverte de novo |
| pausa | Esc pelo menu padrão: 30 quadros com a bola parada no ar e o tempo de jogo congelado (o real andou 0,97 s); Continuar, e a queda segue |
| salvar | salvo a y = 28,921 m (vy −4,52); 1 s depois em 19,178; carregado: y = 28,921, vy −4,52; dados iguais (incluindo "ação") |
| analogico | analógico para a frente: 8,85 m em 2 s (4,5 m/s); W por 1 s: 4,65 m |

Na janela (Xvfb): o menu aparece por cima do jogo, Opções abre pelo clique e a troca para Média refaz o renderizador
sem fechar o jogo.

## Partículas de jogo

Na entidade: `"particulas": {"tipo": "fogo", "raio": 0.3, "escala": 1, "quantidade": 900, "desvio": [0, 0.3, 0],
"ligado": true}` (`fogo`, `fumaca` ou `faiscas`; o fogo acende uma luz que tremula). O efeito acompanha a entidade
(uma tocha carregada, um carro pegando fogo) e `e:particulas(false)` / `e:particulas(true)` desliga e liga (a luz
junto). `explosao(x, y, z [, escala])` dispara uma explosão na hora: bola de fogo que esfria de ~2500 K, fragmentos
incandescentes balísticos, fumaça de fuligem e o clarão de luz (4 explosões simultâneas por cena; só reservadas se
algum script chama `explosao`). As partículas marcam uma máscara reativa para o TAA: fogo andando não deixa rastro.

Teste (`tools/jogo.py particulas`, imagem 480x270 à noite): o brilho do fogo de uma tocha que anda de x = −2 a
+1,6 m vai junto (centro do brilho de 0,41 a 0,54 da tela); desligado, a região cai de 0,631 para 0,088; a
explosão em t = 1 s leva a região dela de 0,000 a 0,520 em 0,1 s.

## Multijogador (ENet)

Na cena: `"multijogador": {"vagas": 3, "nascer": [[3, 0, 0], [-3, 0, 0]]}`. O jogador da cena (`"controle":
"jogador"`) é copiado nas vagas (`heroi_2`, `heroi_3`...), estacionadas fora do mapa até alguém entrar.

```
legacy --scene cenas/parque.json --host            (anfitrião, porta 7777)
legacy --scene cenas/parque.json --conectar 192.168.0.10
```

Anfitrião autoritativo: ele simula tudo (física, personagens, carros, scripts) e manda as poses do que se mexe 30
vezes por segundo (UDP não confiável: o próximo estado substitui o perdido); cada cliente manda só a entrada (andar,
pular, correr, direção da câmera; o anfitrião limita o tamanho do vetor) e mostra o mundo interpolado 100 ms atrás,
liso mesmo com pacotes perdidos. Quem entra ocupa uma vaga livre e nasce no ponto dela; quem sai libera a vaga.
A câmera do cliente segue o boneco dele.

Teste (`tools/rede.py`, anfitrião e cliente sem janela na mesma máquina): o cliente segura W por 1,5 s e o boneco
dele anda 6,30 m no anfitrião; no fim, o boneco e a bola aparecem no cliente na mesma posição que no anfitrião
(diferença 0,00 cm) e a bola parada no chão.

Ainda não tem: previsão no cliente (no LAN o atraso é de poucos ms; pela internet o próprio boneco responde com o
atraso da conexão), animação replicada dos personagens com `"visual"` (aparecem na pose parada) e rodas dos carros
no cliente (o chassi vem certo). Os scripts rodam nos dois lados (no cliente servem para a interface).

## Exportar o jogo (`tools/empacotar.py`)

```
python tools/empacotar.py --cena cenas/carro.json --saida dist/Carro
```

Gera uma pasta que roda sem o código nem o build: o executável e as bibliotecas dele (DLSS), os shaders compilados
(o executável procura `shaders/` na própria pasta antes da pasta do build), a fonte do HUD, as cenas e tudo o que
elas usam (modelos glTF com os `.bin` e as texturas, scripts, sons e os arquivos citados nos scripts), os textos de
licença e `jogar.bat` / `jogar.sh`, que abrem a primeira cena em tela cheia (`--janela` para janela, `--qualidade`
para o preset; `--cena` repetido inclui várias). Arquivo citado que não existe vira aviso e o script sai com erro.
Na primeira execução o motor comprime as texturas em `texcache/`.

Conferido: a Sponza exportada (cena com `"modelo"`) leva o .gltf, o .bin e as 69 texturas (71 arquivos) e roda de
dentro da pasta exportada; rastreando os arquivos abertos (strace), o jogo não lê nada da pasta do motor.

## Formato das cenas (.json)

Comentários `//` são aceitos. Exemplos: `cenas/fisica.json`, `cenas/parque.json`. Rodar:
`legacy --scene cenas/parque.json` (janela; com jogador, WASD anda, espaço pula, Shift corre) ou
`legacy --headless --scene cenas/fisica.json --frames 180 --save-world fim.json`.

```json
{
  "sol": {"elevacao": 40, "azimute": 135, "iluminancia": 100000},
  "camera": {"posicao": [0, 3, 14], "alvo": [0, 1.5, 0], "fov": 60},
  "materiais": {"madeira": {"cor": [0.55, 0.35, 0.18], "rugosidade": 0.6, "metalico": 0, "emissao": [0, 0, 0]}},
  "entidades": [
    {"nome": "chao", "forma": "caixa", "meia_extensao": [30, 0.25, 30], "posicao": [0, -0.25, 0],
     "fisica": {"tipo": "estatico", "atrito": 0.8}},
    {"nome": "pilha", "forma": "caixa", "meia_extensao": [0.3, 0.3, 0.3], "posicao": [0, 0.3, 0], "material": "madeira",
     "fisica": {"tipo": "dinamico", "massa": 8, "quique": 0.1},
     "repetir": {"quantidade": [1, 10, 1], "passo": [0, 0.6, 0]}},
    {"nome": "bola", "forma": "esfera", "raio": 0.5, "posicao": [3, 5, 0], "velocidade": [0, 0, -4],
     "fisica": {"tipo": "dinamico"}},
    {"nome": "sponza", "modelo": "assets/Sponza/Sponza.gltf", "escala": 1,
     "fisica": {"tipo": "estatico", "colisao": "malha"}},
    {"nome": "jogador", "posicao": [0, 0, 8],
     "personagem": {"altura": 1.8, "raio": 0.3, "velocidade": 4.5, "velocidade_correndo": 8, "pulo": 5,
                    "rampa_maxima": 50, "degrau_maximo": 0.4, "controle": "jogador"}}
  ]
}
```

- `forma`: `caixa` (`meia_extensao`), `esfera` (`raio`), `capsula` (`raio`, `meia_altura`).
- `rotacao_graus` [x, y, z] (Euler) ou `rotacao` [w, x, y, z] (quaternion).
- `fisica.tipo`: `estatico`, `dinamico` ou `cinematico`; `massa` (kg; 0 = densidade da água), `atrito`, `quique`,
  `amortecimento_linear`, `amortecimento_angular`.
- `personagem`: o `posicao` é o dos pés; `controle` `jogador` (teclado) ou `roteiro` com
  `"roteiro": [{"t": 0, "andar": [1, 0, 0], "correr": false, "pular": false}]`.
- `--save-world` grava no mesmo formato, com as poses e velocidades depois da simulação.

## Testes (`tools/fisica.py`)

| Teste | Critério | Resultado |
|---|---|---|
| queda | queda livre de 20 m por 1 s, igual ao integrador (Euler semi-implícito a 1/120 s) | 15,0541 m (esperado 15,0541) |
| quique | esfera com quique 1 volta a 90–105% da altura | 101,7% |
| pilha | torre de 12 caixas parada 10 s: topo < 2 cm, tudo dormindo | 0,17 cm |
| tunel | 200 esferas de 5 cm a 30 m/s contra chão de 10 cm | nenhuma atravessa |
| deterministico | cenas/fisica.json duas vezes | arquivos idênticos |
| andar | 3 s a 4,5 m/s | 13,500 m |
| pulo | pulo a 5 m/s, igual ao integrador a 1/60 s | 1,228 m (integrador 1,233) |
| escada | 8 degraus de 25 cm | sobe até 2,000 m |
| rampa | sobe 35°, não sobe 60° | 3,24 m / 0,00 m |
| empurrar | anda contra caixa de 20 kg | a caixa anda 2,8 m |
| sponza | 10 s andando na Sponza com colisão pela malha | no chão (y = −0,02 m), para na parede (x = 9,94) |
| restaurar | Jogar 3 s e Parar no editor | as 36 entidades voltam com diferença 0 |
| script | Lua sobe uma caixa 1 m/s por 2 s; outro script com erro | y = 3,000 m; o erro não derruba nada |
| recarga | o .lua muda no disco com o motor rodando | a versão nova vale sem reiniciar |
| carro_reta / carro_curva / carro_freio | carro acelera em linha reta, vira, freia | 14,1 m/s (desvio lateral 0,01 m); vira 107°; para (0,01 m/s) em pé |
| npc | parede de 10 m entre o NPC e o alvo | contorna e para a 0,24 m do alvo |
| npc_seguir | NPC segue um personagem andando | fica a 1,45 m dele (para a 1,5 m) |

## Bibliotecas (todas gratuitas, de código aberto)

| Biblioteca | Versão | Uso | Licença |
|---|---|---|---|
| Jolt Physics | 5.2.0 | física (Horizon Forbidden West, Death Stranding 2) | MIT |
| EnTT | 3.13.2 | entidades e componentes (Minecraft Bedrock) | MIT |
| nlohmann/json | 3.11.3 | cenas em arquivo | MIT |
| Dear ImGui (docking) | 1.92.8 | interface do editor | MIT |
| ImGuizmo | 105e4fd | alças de mover e girar | MIT |
| Lua | 5.4.7 | scripts | MIT |
| sol2 | 3.5.0 | ponte C++ ↔ Lua | MIT |
| miniaudio | 0.11.24 | áudio 3D | domínio público / MIT-0 |
| Recast/Detour | 1.6.0 | malha de navegação e caminhos | zlib |
| Roboto (fonte) | — | texto do HUD | Apache 2.0 |
| Raposa (modelo de exemplo) | — | personagem animado | CC-BY 4.0 (PixelMannen, tomkranis) |
| ENet | 1.3.18 | multijogador (UDP) | MIT |
