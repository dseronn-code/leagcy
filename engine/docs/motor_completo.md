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

## Script visual (nós no estilo do Blueprint)

Sem escrever código: um grafo de nós em JSON (`"script_visual": "porta.vs.json"` na entidade, ou o grafo direto ali)
vira Lua e roda no mesmo motor de scripts. Editar o `.json` com o jogo rodando gera o Lua de novo (recarga a quente).

```json
{"nos": [{"id": 1, "tipo": "ao_chegar_perto", "alvo": "jogador", "distancia": 2},
         {"id": 2, "tipo": "mover", "relativo": true, "y": 2, "duracao": 1},
         {"id": 3, "tipo": "esperar", "segundos": 0.5},
         {"id": 4, "tipo": "som", "arquivo": "porta.wav"}],
 "ligacoes": [[1, 2], [2, 3], [3, 4]]}
```

- **Eventos:** `ao_iniciar`, `ao_atualizar`, `ao_tecla` (`tecla`), `a_cada` (`segundos`), `ao_chegar_perto` /
  `ao_sair_de_perto` (`alvo`, `distancia`), `quando` (`a`, `op`, `b`: dispara quando a condição passa a valer).
- **Ações:** `log`, `texto` (na tela por `segundos`), `mover` (`x y z`, `relativo`, `duracao`), `girar` (graus,
  `duracao`), `esperar`, `som`, `impulso`, `particulas`, `animar`, `explosao`, `definir` / `somar` (`variavel`,
  `valor`), `se` (saídas `sim` e `nao`), `repetir` (`vezes`; saídas `corpo` e `depois`; `$indice` dentro), `parar` e
  `lua` (`codigo`: escape para o Lua).
- **Valores:** número, texto, `$variavel`, `#tempo`, `@entidade.x|y|z|distancia` (`@eu` é a própria) ou
  `["+", a, b]` (e `- * /`, comparações, `e`, `ou`). Textos aceitam `{expressão}`: `"Moedas: {$moedas}"`.
- **Ligações:** `[de, para]` ou `[de, para, "saida"]`. Ações que levam tempo (`esperar`, `mover`/`girar` com duração)
  rodam numa corrotina por disparo (o jogo segue); um disparo novo do mesmo evento espera o anterior terminar. Ciclo
  fora de `repetir` é recusado com mensagem.

**Editor de grafos** (no `--editor`: campo + "Abrir grafo", ou `--grafo arquivo`): os nós viram caixas arrastáveis
com os campos editáveis; fios curvos entre os pinos (arraste de uma saída, à direita, até a entrada de outro nó;
botão direito numa saída apaga os fios dela); botão direito no fundo abre o menu de nós novos; Delete apaga o
selecionado; "Salvar" grava o `.json` (e a recarga a quente põe o script novo no jogo). O mesmo editor abre os
materiais por nós (cada campo com `{"no": id}` é um pino de dados; a "saída do material" aparece como um nó).

![Editor de grafos com um script visual](screenshots/editor_grafo.png)

Testes (`tools/visual.py`): eventos (início 1x, repetir 1-3, `a_cada` + `se` chega a 3 em 1,50 s, `quando` dispara
1x), chegar perto do jogador (2,03 s) e subir 2 m em 1 s + esperar 0,5 s (y = 2,5 em 3,53 s), recarga do `.json`
com o jogo rodando, o grafo com ciclo recusado sem derrubar o motor e o editor de grafos (Xvfb: um nó novo ligado e
salvo; o script editado roda o nó).

## Materiais por nós

Como o editor de materiais do Unreal ou o Substance: em `"materiais"`, um material com `"nos"` é um grafo assado em
texturas na carga da cena (todas repetem sem emenda: os ruídos são periódicos) — cor (sRGB), rugosidade + metal e,
se a saída tem `"altura"`, o normal map do relevo.

```json
"tijolo": {"resolucao": 512,
  "nos": [{"id": 1, "tipo": "tijolos", "linhas": 8, "colunas": 4, "argamassa": 0.08},
          {"id": 2, "tipo": "ruido", "escala": 16, "oitavas": 4},
          {"id": 3, "tipo": "rampa", "entrada": {"no": 2}, "cores": [[0, "#8a3324"], [1, "#b5523a"]]},
          {"id": 4, "tipo": "misturar", "a": "#bdb6a8", "b": {"no": 3}, "t": {"no": 1}}],
  "saida": {"cor": {"no": 4}, "rugosidade": 0.85, "altura": {"no": 1}, "forca_relevo": 0.01}}
```

- **Geradores:** `ruido` (`escala`, `oitavas`, `ganho`, `semente`), `voronoi` (`saida`: `distancia` ou `borda`),
  `tijolos` (`linhas`, `colunas`, `argamassa`), `listras` (`escala`, `inclinacao`), `gradiente` (`eixo` u/v),
  `constante`.
- **Operações:** `misturar` (`a`, `b`, `t`), `multiplicar`, `somar`, `inverter`, `contraste` (`min`, `max`), `rampa`
  (`entrada`, `cores`), `deformar` (desloca a UV da `entrada` por `x`/`y`, `forca`).
- **Entradas:** número, cor `[r, g, b]` (linear) ou `"#rrggbb"` (sRGB), ou `{"no": id}`. Saída: `cor`, `rugosidade`,
  `metalico`, `altura` (+ `forca_relevo`).
- **Num arquivo:** `"tijolo": {"grafo": "tijolo.mat.json"}` — o que o editor de grafos abre e grava.

Testes (`tools/materiais.py`): tijolos com 8 fileiras e argamassa em 18% da textura; nenhuma emenda nas bordas (a
diferença na emenda é menor que entre vizinhas no meio); metal enferrujado com metal 5/255 na ferrugem e 228 fora,
rugosidade 227 x 81 e normal map inclinado nas bordas; a face renderizada varia 15,5 contra 0,4 lisa; nó desconhecido
recusado com mensagem.

![Materiais por nós: tijolos e metal enferrujado](screenshots/materiais_nos.png)

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

## Ragdoll (Jolt)

`e:ragdoll(ix, iy, iz)` num personagem com `"visual"`: o corpo vira peças da física ligadas por juntas e o modelo
passa a seguir as peças; `e:levantar()` põe o personagem de pé onde a bacia caiu, com a animação de volta;
`e:caido()` diz se está em ragdoll. O impulso (N·s) soma à velocidade que o personagem já tinha (cai para o lado em
que corria).

- **Montagem automática pelo esqueleto do glTF**, sem configurar osso por osso: a partir da bacia (a primeira junta
  que se divide em pernas e coluna; ossos de controle acima dela ficam fora), cada osso vira uma cápsula até os
  filhos (ou, nas pontas — cabeça, mãos, pés —, 60% do osso pai na mesma direção); ossos curtos demais seguem o pai.
  Juntas swing-twist do Jolt com limites (cone de 50°, torção de 25°) e um atrito leve; partes que se tocam na pose
  inicial não colidem entre si. A massa do personagem (`"massa"`) é dividida pelo volume das peças.
- Contínuo (sem atravessar o chão em queda rápida) e no passo fixo da física: o mesmo resultado a qualquer FPS.
- **Levantar sem salto**: a pose caída se mistura com a animação em 0,5 s (cada osso no mesmo ritmo, posição e
  rotação), e o personagem fica de pé virado para onde o tronco estava deitado.
- **Em rede**: o anfitrião simula e manda a matriz de cada parte junto com o estado; o cliente interpola entre dois
  estados (como as rodas dos carros) e faz a mesma transição ao levantar (teste `ragdoll` em `tools/rede.py`).

![ragdoll](screenshots/ragdoll.png)

Testes (`tools/ragdoll.py`, a raposa de 20 kg):

| Teste | Critério | Resultado |
|---|---|---|
| cai | bacia perto do chão, nenhum osso dentro do piso | bacia 0,41 → 0,07 m; osso mais baixo a 0,8 cm acima |
| juntas | distância entre ossos ligados muda < 5% | 4,0% (bacia–coxa: 3,6 mm) |
| empurrao | 3 m/s para +X: vai para +X | 1,02 m em X, 0,04 m em Z |
| repouso | caída, nada se mexe > 1 cm em 0,5 s | 0,00 cm |
| levantar | e:levantar e andar: de pé na altura normal | bacia a 0,41 m (antes: 0,41); andou 3,5 m |
| degrau | cai sobre a quina de um degrau de 30 cm | nenhum osso dentro (0,0 cm) |
| transicao | ao levantar, nenhum osso salta > 5 cm por quadro (60 quadros/s); virada como estava | 3,2 cm (sem a mistura: ~35 cm de uma vez); 10° |

### Ragdoll ativo (no estilo do Physical Animation)

`e:ragdoll_ativo(forca, alvo)` liga motores nas juntas do ragdoll (motores de posição do Jolt, mola criticamente
amortecida, torque pela massa de cada parte): cada parte é puxada para a orientação que tinha em relação à mãe na pose
alvo — `"pose"` (padrão: a pose de quando caiu) ou `"animacao"` (o clipe tocando). `forca` 0 desliga (ragdoll mole), 1
segura a pose (cai "duro", reage a golpes sem desmontar). `e:ragdoll_desvio()` dá o desvio médio das juntas em graus.

Teste (`tools/ragdoll.py ativo`): sem contato nenhum ragdoll muda de pose; tombando, 0,3 s depois o desvio é 37,5° solto
e 14,7° com os motores; deitado no chão (2,5 s), 44,6° x 9,0° (o que sobra é o chão empurrando as partes).

### Retargeting (animação de um esqueleto em outro)

No `"visual"` do personagem: `"retarget": {"de": "outro.gltf", "prefixo_origem": "", "prefixo_destino": "Var_"}` (ou
`"mapa": {"osso_origem": "osso_destino", ...}`) copia todos os clipes do outro modelo para este, assados a 30 quadros/s.
Cada osso mapeado gira, no espaço do modelo, o mesmo que o de origem a partir do repouso; uma **pose de retargeting
automática** corrige posturas de repouso diferentes (cada osso aponta como o da origem); a bacia anda o deslocamento da
origem escalado pela altura dela acima do pé (pernas mais longas, passos e quique maiores). Ossos fora do mapa seguem o
pai.

Testes (`tools/retarget.py`, com uma variante do CesiumMan de outro esqueleto — nomes trocados, pernas 30% maiores,
braços 20% menores, tronco inclinado no repouso e sem animação): cada osso aponta igual ao da original em todos os
quadros (pior 0,01°); o quique da bacia sai 1,27x o da original (altura da bacia acima do tornozelo 1,24x); com o mapa
de uma perna só, a outra fica parada (3,3° contra 111,3° no passo).

## Navegação e NPCs (Recast/Detour)

Personagem com `"controle": "ia"` anda sozinho pela malha de navegação: `"ia": {"alvo": [x, y, z]}` vai até um
ponto; `"ia": {"seguir": "nome"}` segue outra entidade; `"parar_a"` é a distância em que para (m). A malha é gerada ao
começar a partida a partir do cenário parado (caixas estáticas e modelos com `"colisao": "malha"`), no tamanho do
personagem (altura, raio, degrau e rampa máximos), então desvia de paredes, sobe escadas e evita o que é íngreme. O
caminho é recalculado a cada 0,5 s (o alvo pode andar). Lua: `e:ir_para(x, y, z)` e `e:seguir(outra)`.

## Comportamentos de NPC

Na cena: `"controle": "ia", "ia": {"patrulha": [[x, y, z], ...], "perseguir": "jogador", "fugir_de": "lobo",
"visao": 12, "campo_visao": 140, "parar_a": 1.5}`. A cada quadro o NPC escolhe, em ordem: **fugir** de quem está em
`fugir_de` quando o vê (e continua fugindo uns segundos de costas, pela memória de onde viu); **perseguir** quem está em
`perseguir` enquanto o vê, correndo; **procurar** no último lugar onde o viu, por 4 s; **seguir**; **patrulhar** os
pontos em laço; ou ir ao `alvo`. "Ver" = dentro de `visao` metros, dentro do cone de `campo_visao` graus para onde ele
olha e com a linha de visada livre (raio da física); bem perto (2,5 m) ele percebe mesmo de costas. Tudo pela malha de
navegação (desvia de paredes). Lua: `e:patrulhar({{x,y,z}, ...})`, `e:perseguir(outra)`, `e:fugir_de(outra)`,
`e:ve(outra)`, `e:estado_ia()` (`parado`, `patrulhando`, `perseguindo`, `procurando`, `fugindo`, `seguindo`, `indo`).

Testes (`tools/ia.py`):

| Teste | Resultado |
|---|---|
| patrulha | passa pelos 3 pontos em ordem e recomeça (0 → 1 → 2 → 0 → 1 → 2) |
| persegue | herói 8 m à frente: vê, corre e para a 1,40 m (pedido 1,5) |
| parede | parede no meio: não vê, não sai do lugar |
| costas | herói 6 m atrás (fora do cone de 140°): não vê |
| procura | herói visto foge mais rápido até sair da visão: perseguindo → procurando → patrulhando |
| foge | o herói anda até ele: foge, a distância vai de 6,2 m a 9,7 m |

## Câmera de cinema e bibliotecas de jogo

`camera(x, y, z, alvo_x, alvo_y, alvo_z [, fov])` põe a câmera onde o script quiser (cenas de cinema, câmeras fixas)
e `camera_jogo()` devolve. Teste: o script vira a câmera da caixa azul para a vermelha (centro da imagem de
27, 41, 107 para 45, 6, 8).

`incluir("lib/dialogo.lua")` carrega uma biblioteca no ambiente do script (procura ao lado do script e em
`scripts/`). Prontas:
- **`lib/dialogo.lua`**: conversas em árvore (`quem`, `fala`, `proximo` ou `opcoes` com ação), fala letra a letra,
  escolhas por mouse, setas + Enter ou direcional + A; pausa o jogo durante a conversa.
- **`lib/inventario.lua`**: inventário (`adicionar`, `remover` sem ficar negativo, `quantidade`, `tem`, `itens`,
  `desenhar`) e missões (`iniciar` com objetivos, `cumprir`, `estado`, `ao_concluir`, `desenhar`), com `dados()` e
  `carregar()` para ir no `salvar_jogo`.
- **`lib/sequencia.lua`** (cutscenes): linha do tempo com trilha de câmera (chaves com tempo, posição, alvo — um
  ponto ou uma entidade, que a câmera acompanha — e FOV; curva suave que passa por todas, arrancando e parando
  devagar), legendas, `animar`, `som`, `ir_para`, `ragdoll` e `chamar` (qualquer função), tarjas de cinema, Enter
  ou A pulam (os `chamar` que faltam rodam e `ir_para` vira teletransporte, para o jogo ficar no estado certo) e o
  jogador fica parado durante a cena (`controle_jogador(falso)`, que também serve sozinho). No fim, a câmera de
  antes volta (posição, direção e FOV) e `ao_terminar` é chamada.

Exemplo: o guarda de `cenas/parque.json` (`scripts/guarda.lua`) patrulha, conversa quando o jogador chega perto e
aperta E, dá uma missão com dois objetivos e moedas. Testes em `tools/jogo.py`: `dialogo` (Enter avança, ↓ + Enter
escolhe a 2ª opção e roda a ação dela, o jogo pausa e volta) e `inventario` (contas, missão concluída e tudo igual
depois de salvar e carregar).

Testes do sequenciador (`tools/sequencia.py`, cena de 4 s com três chaves de câmera e eventos):

| Teste | Critério | Resultado |
|---|---|---|
| camera | imagem: centro vermelho no começo, azul no fim, câmera do jogo depois | vermelho / azul / câmera do jogo |
| curva | passa pelas chaves; velocidade sem trancos | nas 3 chaves; muda no máximo 0,27 m/s entre quadros (máx. 3,2 m/s) |
| eventos | cada um no quadro do seu tempo, em ordem | a, b, c, fim — 0 ms de atraso |
| travar | W segurado não anda na cena, anda depois | 0,0 cm na cena; 6,75 m depois |
| pular | Enter em 1 s: acaba, roda os `chamar` restantes, o jogador anda | acabou em 1,03 s; a, b, c chamados; andou 6,3 m |
| tarjas | com janela: tarjas pretas e legenda | preto (0) em cima e embaixo; 315 pixels de legenda |

### Linhas do tempo em JSON (no estilo do Sequencer)

Sem código: `"linhas_do_tempo"` na cena define sequências com as mesmas faixas da biblioteca acima e mais
**faixas de objetos** (posição `pos` e rotação `rot` em graus por chave, na mesma curva suave da câmera):

```json
"linhas_do_tempo": {
  "abrir_portao": {"tocar_ao_iniciar": false, "barras": true,
    "objetos": [{"nome": "portao", "chaves": [{"t": 0, "rot": [0, 0, 0]}, {"t": 2, "rot": [0, 90, 0]}]}],
    "camera": [{"t": 0, "pos": [0, 2, 6], "alvo": [0, 1, 0]}, {"t": 2, "pos": [3, 2, 4], "alvo": ["portao"]}],
    "eventos": [{"t": 0.5, "fala": "O portão abre..."}, {"t": 1.5, "som": ["portao.wav", 0, 1, 0]},
                {"t": 2, "log": "aberto"}, {"t": 2, "lua": "codigo"}]}}
```

Qualquer script toca com `tocar_linha("abrir_portao")` (um script visual, pelo nó `lua`). Lua: `e:orientar(x, y, z)`
(rotação absoluta) e `e:rotacao()`. Testes (`tools/linha.py`): a porta gira 0° → 45,0° no meio → 90,00° no fim; o
elevador passa pela chave do meio (y = 1,000 em 1,5 s) e para em 3,000; eventos na hora certa contada do início da
linha (1,5 e 2,5 s); a câmera da linha e a volta para a do jogo.

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
| `camera(x, y, z, ax, ay, az, fov)`, `camera_jogo()` | câmera de cinema pelo script / volta à do jogo |
| `incluir("lib/dialogo.lua")` | carrega uma biblioteca de jogo no ambiente do script |
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

## Idiomas

Os textos do jogo passam por `t("texto")`: a chave é o próprio texto em português, então sem tradução (ou sem a
pasta) aparece o português e nunca uma chave crua. As traduções ficam em `idiomas/<código>.json`:

```json
{"_nome": "English", "Continuar": "Continue", "Qualidade: {q}": "Quality: {q}"}
```

- `t("Qualidade: {q}", {q = "Alta"})` troca `{q}`; `idioma()` diz o atual e `idioma("en")` troca (e guarda a escolha
  do jogador em `saves/idioma.txt`); `idiomas()` lista `{código, nome}` para montar um seletor.
- Ordem de escolha: `--idioma en` na linha de comando, a escolha guardada, `"idioma"` da cena, português.
- Já traduzidos: o menu de pausa (com a escolha de idioma em Opções quando há mais de um), `lib/dialogo.lua`
  (falas, nomes e opções passam por `t` sozinhos), `lib/sequencia.lua` (legendas), `lib/inventario.lua` (itens,
  missões e objetivos) e o guarda do parque. Vêm inglês e espanhol; um idioma novo é só um arquivo a mais.
- A fonte do HUD cobre o Latin-1 (á, ç, ñ, ¿, ¡...). O empacotador leva `idiomas/` junto.

Testes (`tools/idiomas.py`):

| Teste | Resultado |
|---|---|
| traduz | en "Continue", "Quality: High"; es "Calidad: Alta", "¡Misión cumplida! +20 monedas"; sem tradução sai como está |
| lista | `idiomas()`: en=English, es=Español, pt=Português |
| escolha | cena "en" → en; depois de `idioma("es")` a escolha guardada vence a cena; `--idioma pt` vence tudo |
| menu | menu de pausa com janela: o 1º botão em inglês ("Continue") mais estreito que em português ("Continuar") |

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

**Previsão no cliente:** o boneco do próprio jogador anda na hora com a entrada local (a mesma física de
personagem, passo fixo de 1/60 s, contra o cenário). Cada entrada vai numerada; o anfitrião devolve, junto com o
estado, até qual entrada já processou, e o cliente compara com o que tinha previsto naquele ponto: diferença pequena é
corrigida aos poucos (sem tranco), grande (nascimento, teleporte) de uma vez. Os outros objetos seguem interpolados
100 ms atrás. Personagens com `"visual"` animam no cliente pela velocidade estimada das poses; as rodas dos carros
(posição, giro e esterço) vêm do anfitrião.

Teste (`tools/rede.py`, anfitrião e cliente sem janela na mesma máquina, o jogador é a raposa animada):

| Teste | Resultado |
|---|---|
| rede | o cliente segura W 1,5 s: o boneco anda 2,25 m no anfitrião; no fim, boneco a 0,01 cm e bola a 0,00 cm nos dois lados; no cliente a pata oscila 5,3 cm em relação à bacia (animação) |
| previsao | com 150 ms de atraso simulado em cada sentido (`LEGACY_REDE_ATRASO_MS`): o boneco reage em 67 ms (2 quadros) com previsão, 367 ms sem; no fim os dois lados concordam (0,0 cm) |
| rodas | carro dirigido por roteiro no anfitrião: a imagem final do cliente igual à do anfitrião (diferença média 0,21/255), chassi e rodas |
| ragdoll | raposa em ragdoll no anfitrião: no cliente cai junto (bacia a 2 cm do chão), deitada no mesmo lugar (0,00 cm) e de pé no fim (0,41 m) |

Os scripts rodam nos dois lados (no cliente servem para a interface).

## Geometria virtualizada (no estilo do Nanite)

`--nanite` (ou `"nanite": true` na cena) troca os LODs inteiros das malhas rígidas grandes por uma hierarquia de
pedaços que a GPU escolhe um a um, com erro de até `--lod-pixels` (1) na tela:

- **Na carga** (em paralelo, uma malha por núcleo; guardada em `texcache/<hash>.lod`, a segunda carga só lê): os
  triângulos são reordenados em pedaços compactos de até 124 triângulos (o nível 0; a numeração nova vale para tudo:
  luzes de malha, ray tracing, visibility buffer). Grupos de ~8 pedaços vizinhos são juntados, simplificados pela
  metade com a borda do grupo travada (meshoptimizer; o erro é a distância geométrica — com peso em normais e UV ele
  inflava e a GPU nunca escolhia os níveis grossos) e divididos de novo, nível após nível, até a raiz. Cada pedaço
  guarda a esfera e o erro do grupo que o formou e do que o substitui. Os pedaços ficam em ordem de nível e posição
  (Morton) e cada bloco de 32 guarda limites conservadores.
- **Na GPU** (task shader): o bloco inteiro é descartado quando é fino demais, grosso demais ou está fora da tela;
  depois cada pedaço é desenhado quando o próprio erro projetado cabe no limite e o do grupo seguinte não. Irmãos
  usam a mesma esfera: o corte nunca abre buracos e não há troca brusca de LOD.
- **Oclusão por pedaço em duas fases**: na fase 0 cada pedaço testa a pirâmide de profundidade do frame anterior
  (com as matrizes anteriores); depois da pirâmide nova, um repasse desenha o que ela escondia e a nova mostra; os
  objetos novos testam a nova. Conservadora: imagem idêntica com e sem.
- **Rasterização por software** (`--sw-raster`, pedaços com menos de `--sw-raster-px` 16 pixels): em compute, cada
  triângulo grava profundidade + triângulo por pixel com atomicMax de 64 bits; uma passada de tela cheia transforma o
  vencedor em G-buffer com o mesmo material (gradientes de UV analíticos) e a profundidade decide contra o
  rasterizador fixo. Imagem igual à do rasterizador fixo (0,11/255). Desligada até medir na RTX.
- **Com o ray tracing**: o pixel de um pedaço simplificado guarda o pedaço e o triângulo no visibility buffer, e os
  passes de raios reconstroem a superfície exata dele pelos meshlets; os raios saem além do erro daquele pedaço (a
  malha do ray tracing é a completa), sem manchas de sombra, e o ReSTIR mantém o reuso temporal.
- **Streaming dos pedaços (ainda não)**: no Nanite ele poupa memória porque o ray tracing da UE5 usa uma malha
  substituta grosseira. Aqui sombras, reflexos e a superfície exata usam a malha completa, então vértices e BLAS
  ficam inteiros de qualquer jeito e o streaming pouparia só as listas de índices. O ganho de verdade pede ray tracing
  numa versão grossa da malha — troca qualidade por memória, decisão para medir na RTX.
- Desligada por padrão nas cenas das metas: no Sponza (480x270, contra a referência por path tracing) o erro vai de
  0,0655 para 0,0734 com ela — essas cenas já batem a meta de desempenho sem ela. Ela é para as cenas que sem ela não
  cabem.

![campo de rochas](screenshots/nanite_campo.png)

Cena `nanite`: um campo de 16 x 16 rochas procedurais de 589.824 triângulos cada — **151 milhões de triângulos**.
Testes (`tools/nanite.py`, sem GPU: os tempos servem para comparar entre si, não para a RTX):

| Teste | Critério | Resultado |
|---|---|---|
| nivel0 | erro de 0,01 pixel: igual à malha inteira | diferença 0,000/255 |
| grafo | rocha de 589.824 triângulos | 12 níveis; 6.423 → 14.726 pedaços (2,3x); raiz com 1.731 (0,29%) |
| qualidade | 1 pixel, contra a malha inteira | diferença média 0,65/255 |
| desempenho | G-buffer, 4 x 4 rochas (9,4 milhões de triângulos) | 435 ms com, 4.713 ms sem (10,8x) |
| software | rasterização por software x fixa | 0,11/255; 0,04% dos pixels acima de 8 |

Outras medidas: o campo de 151 milhões de triângulos foi de 5.248 ms (primeira versão) para ~1.930 ms no G-buffer; a
carga com o cache, de 19,4 s para 11,2 s; os blocos e a oclusão não mudam a imagem (comparação pixel a pixel).

**Na RTX 3070 Ti (rodada 21j, 1080p, 151 milhões de triângulos):** G-buffer de 4,1 ms com a geometria virtualizada
contra 24,7 ms sem (6,1x; frame inteiro 35,6 x 56,9 ms); rasterização por software dos pedaços pequenos (`--sw-raster`)
3,1 ms (-24%); sem a oclusão por pedaço 3,55 ms (nessa cena a oclusão custa mais do que poupa). Os 5 testes passam lá
com os mesmos números do Linux (qualidade 0,656 x 0,652/255).

Três correções vieram dessas rodadas na RTX:
- **Alinhamento:** as listas de LOD e de blocos (vec4) começavam em qualquer palavra do buffer; a GPU as lê por ponteiro
  (buffer_reference), que supõe 16 bytes. A NVIDIA lia o endereço arredondado — erros trocados, buracos nas rochas e
  níveis grossos mesmo a 0,01 px; o lavapipe lia certo e escondia. Agora começam alinhadas.
- **Dobra:** um grupo simplificado que perde mais de 20% da área "para fora" (projetada nas normais; dobras se
  cancelam) não vira pai — fica para o próximo nível (no Windows o último grupo de uma rocha perdia 81%).
- **Mesma rocha nos dois sistemas:** as posições da rocha são arredondadas numa grade de 1/65536 (o sin/cos de cada
  biblioteca difere no último bit), e triângulos simplificados virados contra as normais são desvirados.

## Mundo aberto (células por distância, no estilo do World Partition)

Com `"mundo_aberto": {"celula": 64, "carregar": 192, "descarregar": 256}` na cena, as entidades paradas com visual são
divididas numa grade de células (no plano x-z, pelo lado `celula` em metros). A cada quadro, o motor mede a distância do
jogador (ou do ouvinte/câmera, sem jogador) até cada célula: abaixo de `carregar`, a célula acorda; acima de
`descarregar`, dorme (a folga entre os dois evita liga-desliga na borda).

Uma entidade dormindo:
- some do desenho e da cena de ray tracing (sai do TLAS e o culling a descarta);
- sai da física (o corpo é tirado do mundo do Jolt e volta igual ao acordar);
- não roda o script (`atualizar` pula) e o som dela fica mudo.

Ficam fora da grade (sempre carregados): personagens, veículos, corpos dinâmicos, partículas, entidades de sistema, formas
maiores que uma célula (o chão) e as marcadas com `"sempre_carregado": true`. Um script só de lógica precisa dessa marca
(sem `"forma"`, a entidade ganha a caixa padrão e dormiria com a célula dela).

Lua: `celulas()` devolve `carregadas, total`; `e:dormindo()` diz se a entidade está dormindo. `e:mover_para()` agora
também teletransporta personagens (o centro da cápsula, como `e:posicao()`).

Testes (`tools/mundo.py`, 40 caixas a cada 50 m, células de 50 m): com o jogador em x = 0, 5 de 40 células carregadas;
teletransportado para x = 1000, a caixa de x = 0 dorme e a de x = 1000 acorda; uma bola solta sobre a caixa acordada para
em cima (y = 1,25) e outra sobre a dormindo cai até o chão (0,25); de volta a 0, tudo volta; e a câmera só vê a caixa
de x = 500 com a célula dela carregada.

### Terreno de quilômetros

```json
"terreno": {"tamanho": 4096, "altura": 120, "escala": 600, "oitavas": 6, "semente": 3, "resolucao": 64,
            "material": "chao", "atrito": 0.8},
"materiais": {"chao": {"cor": [0.3, 0.4, 0.2], "terreno": true}}
```

A altura vem de ruído fractal (`escala` = tamanho das colinas em metros, `altura` = variação, `base` = deslocamento) ou
de um mapa de altura (`"mapa": "altura.png"`, tons de cinza de 8 ou 16 bits, cobrindo o `tamanho` em volta de
`"centro": [x, z]`). O terreno é dividido em ladrilhos (`ladrilho`, o padrão é a célula do mundo aberto) de
`resolucao` x `resolucao` quadrados; cada ladrilho é uma entidade parada com a malha detalhada e colisão (malha no
Jolt). Com o mundo aberto ligado, cada ladrilho tem também uma malha simples (1/8 da resolução): quando a célula dorme,
o ladrilho troca para ela e sai da física, então o horizonte continua inteiro sem pagar pelo detalhe. Uma saia vertical
na borda de cada ladrilho esconde as frestas entre ladrilhos de resoluções diferentes. Normais e UVs são calculadas em
coordenadas do mundo (sem emenda). `"terreno": true` no material liga as camadas texturizadas (grama, terra, rocha e
cascalho, pela inclinação e por ruído). A navegação dos NPCs inclui o terreno. Lua: `altura_terreno(x, z)`.

Testes (`tools/mundo.py`, terreno de 1024 m em 256 ladrilhos de 64 m): o jogador teletransportado para x = 0 e para
x = 400 fica em pé no chão (centro da cápsula a 0,90 m acima da altura do terreno); uma bola num ladrilho dormindo
atravessa (sem física longe do jogador); 40 de 256 células carregadas; do alto, olhando o horizonte, nenhum buraco
entre os ladrilhos de perto e de longe.

![Terreno de 1 km visto do alto](screenshots/terreno_km.png)

### Esculpir o terreno (editor e Lua)

No editor (`--editor`), o painel **Terreno** liga o pincel: arrastando com o botão esquerdo na cena, `subir` /
`descer` (metros por segundo), `aplanar` (até a altura alvo) ou `suavizar`, com raio ajustável e o contorno do pincel
desenhado no chão. **Salvar mapa** grava um `.altura` (alturas em metros, tamanho e centro), que vira o `"mapa"` do
terreno na cena. Na primeira pincelada a altura (ruído ou PNG) vira uma grade editável com um ponto por vértice dos
ladrilhos; cada pincelada refaz só os ladrilhos tocados (malha de perto e de longe, BLAS do ray tracing e colisão).
Lua: `terreno_pincel(x, z, raio, forca, modo)` e `salvar_terreno(arquivo)`.

Testes (`tools/terreno.py`): subir 3 m com raio 5 (altura 3,0 no centro, 0,0 a 5,5 m; uma bola para em y = 3,25 — a
colisão foi refeita), aplanar a 1,0 m, suavizar baixa um pico de 4,0 para 0,92, salvar e abrir o `.altura` numa cena
nova (3,0 no morro, 0,0 longe), a imagem muda só onde o morro subiu (31,7 x 0,2/255) e o pincel do editor (Xvfb).

![Editor: painel Terreno e o pincel](screenshots/editor_terreno.png)

Ainda não: origem flutuante (para mundos acima de ~10 km, onde o float perde precisão) e carregar células de arquivos
separados do disco (hoje a cena inteira é lida no início; a célula só liga e desliga).

## Luz global sem ray tracing (sondas de irradiância assadas)

Para GPUs sem ray tracing (`--no-rt`), a luz ambiente vem de uma grade de sondas pré-calculada (como o "volumetric
lightmap" do Unreal), no lugar do céu sem sombra de antes. Numa GPU com ray tracing, `--assar-gi` assa: cada sonda lança
256 raios (espiral de Fibonacci); no ponto atingido entra a luz direta (sol e uma luz local, com sombra) e o difuso
das sondas do passe anterior — 3 passes = 3 rebotes. Cada sonda guarda a irradiância em harmônicos esféricos L1 (já
convolvida com o cosseno), se está dentro de uma parede (mais de 1/4 dos raios no verso das faces) e, para não vazar
luz pelas paredes, a distância média até as superfícies em 6 lados; na leitura, as 8 sondas em volta do ponto são
pesadas pela posição, pelo lado da superfície e por um teste de Chebyshev nessa distância (como o DDGI).

Opções: `--sondas-gi arquivo.gi` (sem ela: `cena.json.gi` ao lado da cena, ou `texcache/gi_<cena>.gi` nas de
demonstração, se existir), `--assar-gi` (assa de novo), `--gi-espacamento m` (padrão: o maior lado da cena / 48),
`--gi-raios n`, `--gi-rebotes n`. O arquivo vai junto com o jogo; as sondas valem para o sol e o céu de quando foram
assadas (luz que muda de lugar não atualiza).

Com ray tracing, as sondas (se houver) também servem de palpite para o cache de radiância onde ele ainda não tem dados.
Sem sondas, o palpite agora é o céu por um raio de visibilidade — antes era o céu × 0,5 sem teste, o que fazia uma sala
fechada brilhar azul com RT (medido: 64/255 de azul numa sala sem luz; agora 0,4). A suíte do renderer
(`tools/testes.py`: fornalha, atmosfera, cintilação, sombras, ReSTIR) segue passando.

Testes (`tools/sondas.py`, lavapipe): no Sponza sem RT, o erro contra a imagem com RT cai de 31,7/255 (só céu) para
9,8/255 com as sondas (3,2x menor), assadas em 35 s; numa sala fechada com o sol lá fora, por dentro fica 1,5/255 com
as sondas (0,4 com RT; 84,5 só com o céu); a segunda execução lê o arquivo sem assar de novo.

![Sponza: com RT, sem RT só com o céu, sem RT com as sondas](screenshots/sondas_gi_sponza.png)

## Jogo de demonstração: Vale dos Cristais (`cenas/vale`)

`--scene cenas/vale/vale.json` (ou `python tools/empacotar.py --cena cenas/vale/vale.json --saida dist/Vale`). Junta as
fases novas num jogo pequeno: terreno de 256 m no mundo aberto (células de 32 m), ruínas e cristais com materiais por
nós, 5 cristais com script visual (`cristal.vs.json`: giram, soltam faíscas e, com o jogador perto, explodem e somem —
uma trava no grafo conta cada um uma vez), o guarda (a raposa com IA) que patrulha as ruínas e persegue o jogador
(alcançou = volta ao começo), **F** perto dele o derruba com o ragdoll ativo (levanta 3 s depois), cenas de abertura e
de vitória como linhas do tempo em JSON, HUD com o contador e o menu de pausa padrão. O controlador (`vale.lua`) põe as
ruínas e os personagens no chão do terreno ao começar.

Teste (`tools/demo.py`, joga sozinho): a abertura toca e termina; o jogador coletando os 5 cristais dispara a vitória
(que também termina); F derruba o guarda e ele levanta; o guarda vê o jogador, persegue e o alcança (nesse teste a
prova é o "pego pelo guarda"; com o jogador posto 6 m à frente, a amostra seguinte ainda viu "patrulhando"); a imagem
tem céu, terreno e os cristais.

![Vale dos Cristais](screenshots/vale_dos_cristais.png)

## Malhas comprimidas (meshopt, Draco) e texturas KTX2

- **meshopt** (`EXT_meshopt_compression`, o formato do gltfpack) e **malha quantizada** (`KHR_mesh_quantization`):
  descompressão pela meshoptimizer (MIT). Teste: o DamagedHelmet convertido por `tools/gltf_meshopt.cpp` (comprime a
  geometria com o codificador da meshoptimizer e ZERA os dados originais no .bin, então só aparece algo se o motor
  descomprimir) sai igual ao original — diferença máxima de 1/255 por pixel, só comprimido e também quantizado; a
  geometria cai para 64% (60% quantizada).
- **Draco** (`KHR_draco_mesh_compression`, a compressão mais comum em modelos baixados da internet): decodificada
  pela biblioteca do Google (Apache 2.0); os acessores da primitiva passam a apontar para os dados decodificados e o
  resto do carregador não muda. Teste: o Avocado da Khronos na versão Draco contra a versão normal — diferença média
  de 0,05/255 por pixel.
- **Texturas KTX2** (`KHR_texture_basisu`, ETC1S ou UASTC com zstd, o formato do gltfpack e do Sketchfab):
  decodificadas pelo transcodificador Basis Universal (Apache 2.0) e depois seguem o caminho normal (mipmaps e BC7).
  Teste: o StainedGlassLamp da Khronos na versão KTX2 contra a versão PNG — diferença média de 0,03/255 por pixel
  (19 texturas; a versão KTX2 do arquivo tem 1/3 dos texels e cabe em 24 MB de vídeo contra 78 MB).

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
| meshoptimizer | 0.22 | malhas comprimidas dos glTF | MIT |
| Basis Universal (transcodificador) + zstd | 1.60 | texturas KTX2 | Apache 2.0 / BSD |
| Draco | 1.5.7 | malhas comprimidas (Draco) | Apache 2.0 |
