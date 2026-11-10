-- Diálogos com escolhas (incluir("lib/dialogo.lua")). Árvore de falas:
--
--   local conversa = {
--     inicio = {quem = "Guarda", fala = "Parado! Quem vem lá?", opcoes = {
--       {"Um amigo.", "amigo"},
--       {"Ninguém que te interesse.", "briga", function() reputacao = reputacao - 1 end},
--     }},
--     amigo = {quem = "Guarda", fala = "Pode passar.", proximo = nil},
--     briga = {quem = "Guarda", fala = "Então volte por onde veio!"},
--   }
--   dialogo.iniciar(conversa, "inicio")      -- pausa o jogo enquanto fala (dialogo.iniciar(c, "inicio", false) não)
--   function atualizar(e, dt) dialogo.atualizar(dt) end
--
-- Fala sem opções: Enter, A ou clique avança para `proximo` (ou termina). Com opções: botões (mouse, setas + Enter,
-- direcional + A). A fala aparece letra a letra; Enter durante a digitação mostra tudo. dialogo.ativo() diz se há
-- conversa; dialogo.ao_terminar = function(ultimo_no) ... end é chamada no fim.

dialogo = dialogo or {}
local estado = nil  -- {arvore, no, letras, pausou}
local VELOCIDADE = 45  -- letras por segundo

function dialogo.iniciar(arvore, inicio, pausa)
  estado = {arvore = arvore, no = inicio or "inicio", letras = 0, pausou = pausa ~= false}
  if estado.pausou then pausar(true) end
end

function dialogo.ativo() return estado ~= nil end
function dialogo.no_atual() return estado and estado.no end

local function terminar()
  local ultimo = estado.no
  if estado.pausou then pausar(false) end
  estado = nil
  if dialogo.ao_terminar then dialogo.ao_terminar(ultimo) end
end

local function ir(no)
  if no == nil or estado.arvore[no] == nil then
    terminar()
  else
    estado.no = no
    estado.letras = 0
  end
end

function dialogo.atualizar(dt)
  if not estado then return end
  local no = estado.arvore[estado.no]
  if not no then terminar() return end
  local texto = t(no.fala or "")  -- falas, nomes e opções passam pelos idiomas/
  estado.letras = math.min(#texto, estado.letras + (dt or 0) * VELOCIDADE)
  local completo = estado.letras >= #texto
  -- Caixa de diálogo na parte de baixo da tela.
  hud.retangulo(0.08, 0.66, 0.84, 0.3, {0.03, 0.04, 0.07, 0.86})
  if no.quem then hud.texto(t(no.quem), 0.11, 0.68, {tamanho = 0.038, cor = {1, 0.85, 0.35}}) end
  -- Corta no limite de um caractere UTF-8 (acentos não quebram).
  local n = math.floor(estado.letras)
  while n > 0 and n < #texto and texto:byte(n + 1) >= 0x80 and texto:byte(n + 1) < 0xC0 do n = n - 1 end
  hud.texto(texto:sub(1, n), 0.11, 0.735, {tamanho = 0.034})
  local avancar = tecla_apertada("enter") or controle_apertado("a") or (clique() and not no.opcoes)
  if not completo then
    if avancar then estado.letras = #texto end  -- mostra a fala inteira
    return
  end
  if no.opcoes and #no.opcoes > 0 then
    for i, op in ipairs(no.opcoes) do
      if hud.botao(t(op[1]), 0.12, 0.79 + (i - 1) * 0.055, 0.76, 0.048, {tamanho = 0.028}) then
        if op[3] then op[3]() end
        ir(op[2])
        return
      end
    end
  else
    hud.texto(t("Enter >"), 0.89, 0.91, {tamanho = 0.026, alinhar = "direita", cor = {1, 1, 1, 0.6}})
    if avancar then
      if no.acao then no.acao() end
      ir(no.proximo)
    end
  end
end
