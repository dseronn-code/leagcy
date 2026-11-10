-- Menu de pausa padrão do Legacy (toda cena .json com mundo; "menu": false na cena desliga, "menu": "outro.lua" troca).
-- Esc ou Start abre e fecha. Mouse, setas + Enter, ou direcional + A no controle.
-- Continuar | Salvar jogo | Carregar jogo | Opções (volume, motion blur, qualidade, idioma) | Sair.
-- Textos por t(): traduzidos pelos arquivos de idiomas/ (o português é a chave).

local tela = nil        -- nil (jogando), "pausa" ou "opcoes"
local aviso, aviso_ate = nil, 0
local motion_blur = true
local qualidade_atual = nil
local SLOT = "rapido"

local function avisar(texto)
  aviso, aviso_ate = texto, tempo_real() + 2.0
end

local function abrir(qual)
  tela = qual
  pausar(qual ~= nil)
end

local L, X = 0.32, 0.34  -- largura e esquerda dos botões (fração da tela)

local function menu_pausa()
  hud.texto(t("Pausado"), 0.5, 0.18, {tamanho = 0.075, alinhar = "centro"})
  local y = 0.32
  local function botao(texto)
    local apertou = hud.botao(texto, X, y, L, 0.065)
    y = y + 0.08
    return apertou
  end
  if botao(t("Continuar")) then abrir(nil) end
  if botao(t("Salvar jogo")) then
    if salvar_jogo(SLOT, {salvo_em = tempo()}) then avisar(t("Jogo salvo")) else avisar(t("Não consegui salvar")) end
  end
  if botao(t("Carregar jogo")) then
    if existe_jogo(SLOT) then
      carregar_jogo(SLOT)
      avisar(t("Jogo carregado"))
      abrir(nil)
    else
      avisar(t("Nenhum jogo salvo ainda"))
    end
  end
  if botao(t("Opções")) then abrir("opcoes") end
  if botao(t("Sair")) then sair() end
end

local QUALIDADES = {{"baixo", "Baixa"}, {"medio", "Média"}, {"alto", "Alta"}, {"ultra", "Ultra"}}

local function menu_opcoes()
  hud.texto(t("Opções"), 0.5, 0.14, {tamanho = 0.07, alinhar = "centro"})
  local v = hud.deslizar(t("Volume"), X, 0.27, L, 0.08, volume(), 0, 1)
  if math.abs(v - volume()) > 1e-4 then volume(v) end
  local mb = hud.alternar(t("Motion blur"), X, 0.38, L, 0.065, motion_blur)
  if mb ~= motion_blur then
    motion_blur = mb
    opcao("motion_blur", mb)
  end
  hud.texto(t("Qualidade"), 0.5, 0.475, {tamanho = 0.035, alinhar = "centro", cor = {1, 1, 1, 0.8}})
  local largura = L / 4 - 0.006
  for i, q in ipairs(QUALIDADES) do
    local escolhida = qualidade_atual == q[1]
    local cor = escolhida and {0.15, 0.55, 0.3, 0.95} or nil
    if hud.botao(t(q[2]), X + (i - 1) * (largura + 0.008), 0.52, largura, 0.06, {cor = cor, tamanho = 0.028}) then
      qualidade_atual = q[1]
      qualidade(q[1])
      avisar(t("Qualidade: {q}", {q = t(q[2])}))
    end
  end
  -- Idioma (só com mais de um em idiomas/): a escolha fica guardada para as próximas vezes.
  local lista = idiomas()
  local y_voltar = 0.64
  if #lista > 1 then
    hud.texto(t("Idioma"), 0.5, 0.6, {tamanho = 0.035, alinhar = "centro", cor = {1, 1, 1, 0.8}})
    local l = L / #lista - 0.006
    for i, id in ipairs(lista) do
      local cor = idioma() == id[1] and {0.15, 0.55, 0.3, 0.95} or nil
      if hud.botao(id[2], X + (i - 1) * (l + 0.008), 0.645, l, 0.06, {cor = cor, tamanho = 0.028}) then idioma(id[1]) end
    end
    y_voltar = 0.76
  end
  if hud.botao(t("Voltar"), X, y_voltar, L, 0.065) then abrir("pausa") end
end

function atualizar(e, dt)
  if tecla_apertada("esc") or controle_apertado("start") then
    if tela == nil then abrir("pausa") elseif tela == "opcoes" then abrir("pausa") else abrir(nil) end
  elseif tela ~= nil and controle_apertado("b") then
    if tela == "opcoes" then abrir("pausa") else abrir(nil) end
  end
  if tela ~= nil then
    hud.retangulo(0, 0, 1, 1, {0, 0, 0, 0.55})
    if tela == "pausa" then menu_pausa() else menu_opcoes() end
  end
  if aviso and tempo_real() < aviso_ate then
    hud.texto(aviso, 0.5, 0.9, {tamanho = 0.04, alinhar = "centro", cor = {1, 0.9, 0.4}})
  end
end
