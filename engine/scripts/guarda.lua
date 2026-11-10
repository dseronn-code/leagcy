-- Guarda do parque: patrulha (IA da cena) e conversa quando o jogador chega perto e aperta E (ou X no controle).
-- Mostra as bibliotecas de jogo: diálogo com escolhas, inventário e missões.
incluir("lib/dialogo.lua")
incluir("lib/inventario.lua")

local conversa = {
  inicio = {quem = "Guarda", fala = "Olá! Este é o parque de testes do Legacy.", proximo = "pedido"},
  pedido = {quem = "Guarda", fala = "Pode me ajudar? Suba a escada e toque o elevador.", opcoes = {
    {"Claro, deixa comigo.", "aceitou", function()
      missoes.iniciar("elevador", "Ajudar o guarda", {"subir a escada", "tocar o elevador"})
      inventario.adicionar("moeda", 5)
    end},
    {"Agora não.", "recusou"},
  }},
  aceitou = {quem = "Guarda", fala = "Valeu! Tome 5 moedas adiantadas."},
  recusou = {quem = "Guarda", fala = "Tudo bem, estarei por aqui."},
}

missoes.ao_concluir = function(id)
  inventario.adicionar("moeda", 20)
  som_ok = tempo_real()
end

function atualizar(e, dt)
  local jog = achar("jogador")
  if not jog then return end
  local gx, gy, gz = e:posicao()
  local jx, jy, jz = jog:posicao()
  local perto = math.sqrt((gx - jx) ^ 2 + (gz - jz) ^ 2) < 2.5
  if dialogo.ativo() then
    dialogo.atualizar(dt)
  elseif perto then
    hud.texto(t("E: falar com o guarda"), 0.5, 0.82, {tamanho = 0.035, alinhar = "centro"})
    if tecla_apertada("e") or controle_apertado("x") then dialogo.iniciar(conversa, "inicio") end
  end
  -- Objetivos da missão pelo lugar do jogador: alto na escada e perto do elevador.
  if missoes.estado("elevador") == "ativa" then
    if jy > 2.2 then missoes.cumprir("elevador", "subir a escada") end
    local el = achar("elevador")
    if el then
      local ex, ey, ez = el:posicao()
      if math.sqrt((ex - jx) ^ 2 + (ez - jz) ^ 2) < 1.5 then missoes.cumprir("elevador", "tocar o elevador") end
    end
  end
  missoes.desenhar(0.03, 0.05)
  if #inventario.itens() > 0 then inventario.desenhar(0.76, 0.05) end
  if som_ok and tempo_real() - som_ok < 3 then
    hud.texto(t("Missão concluída! +{n} moedas", {n = 20}), 0.5, 0.3, {tamanho = 0.05, alinhar = "centro", cor = {0.5, 1, 0.5}})
  end
end
