# Motor completo: a camada de jogo

Parte 2 do projeto: completar o Legacy como motor de jogo, além do renderizador. As fases:

| Fase | O que entra | Situação |
|---|---|---|
| 1. Núcleo | Entidades e componentes (EnTT), cenas em arquivo (.json), física (Jolt), personagem, modelos glTF com colisão | ✅ |
| 2. Editor visual | Dear ImGui + ImGuizmo: entidades, propriedades, alças, seleção por clique, Jogar/Parar, criar/duplicar/apagar, salvar | ✅ |
| 3. Scripts | Lua 5.4 (sol2) por entidade, com recarga a quente | ✅ |
| 4. Sistemas de jogo | Áudio 3D ✅; veículos, navegação e IA, interface, animação avançada, exportar o jogo | em andamento |

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

Exemplos: `scripts/elevador.lua` (sobe e desce), `scripts/girar.lua` (gira), `scripts/pulador.lua` (pula sozinho),
todos em `cenas/parque.json`.

## Áudio 3D (miniaudio)

Na entidade: `"som": {"arquivo": "sons/motor.wav", "volume": 1, "laco": true, "espacial": true, "dist_min": 1,
"dist_max": 100, "queda": 1, "tocar": true}` (WAV, MP3 ou FLAC). O som segue a entidade; o ouvinte é a câmera
(atenuação inversa pela distância, panorâmica pela direção, Doppler). Lua: `e:tocar_som()`, `e:parar_som()` e
`som("arquivo", x, y, z [, volume])` para um som avulso. Sem janela, `--audio-wav saida.wav` grava a mixagem.

Testes (`tools/audio.py`, tom de 440 Hz): a 2 m soa 4,002× mais alto que a 8 m (atenuação inversa prevê 4,0); uma
fonte à direita sai 5× mais alta no canal direito; sem fonte, silêncio total.

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
