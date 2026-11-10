-- Vale dos Cristais: o controlador do jogo (as ruínas e os personagens vão para o chão pelo "no_chao"). Conta os cristais
-- coletados (os scripts visuais dos cristais os mandam para baixo do mundo), mostra o HUD, toca a cena de vitória e
-- deixa o jogador derrubar o guarda (F, perto dele): o guarda cai com o ragdoll ativo e levanta 3 s depois.
local CRISTAIS = {"cristal_1", "cristal_2", "cristal_3", "cristal_4", "cristal_5"}
local venceu, derrubado = false, nil
local estado_guarda = nil
local SONS = "cenas/vale/sons/"
local pego_ate = 0

local function coletados()
  local n = 0
  for _, nome in ipairs(CRISTAIS) do
    local c = achar(nome)
    if c then
      local _, y = c:posicao()
      if y < -100 then n = n + 1 end
    end
  end
  return n
end

function atualizar(e, dt)
  local n = coletados()
  hud.texto(t("Cristais") .. ": " .. n .. "/" .. #CRISTAIS, 0.03, 0.05, {tamanho = 0.04})
  if n == #CRISTAIS and not venceu then
    venceu = true
    log("VALE", "vitoria")
    tocar_linha("vitoria")
    local jx, jy, jz = achar("jogador"):posicao()
    som(SONS .. "vitoria.wav", jx, jy, jz, 1.0)
  end
  -- O guarda: perto dele, F derruba (ragdoll ativo); se ele alcança o jogador, o jogador volta ao começo.
  local jog, guarda = achar("jogador"), achar("guarda")
  if jog and guarda then
    local jx, jy, jz = jog:posicao()
    local gx, gy, gz = guarda:posicao()
    local d = math.sqrt((jx - gx) ^ 2 + (jz - gz) ^ 2)
    if not derrubado and d < 2.5 and (tecla_apertada("f") or controle_apertado("b")) then
      guarda:ragdoll((gx - jx) * 30, 40, (gz - jz) * 30)
      guarda:ragdoll_ativo(0.6)
      derrubado = tempo()
      log("VALE", "guarda derrubado")
      som(SONS .. "derrubar.wav", gx, gy, gz, 1.0)
    elseif derrubado and tempo() - derrubado > 3 then
      guarda:levantar()
      derrubado = nil
      log("VALE", "guarda levantou")
    elseif not derrubado and d < 1.0 and tempo() > pego_ate then
      jog:mover_para(0, altura_terreno(0, 30) + 0.95, 30)
      pego_ate = tempo() + 2
      log("VALE", "pego pelo guarda")
    end
    -- Alerta: o guarda começou a perseguir (um latido onde ele está).
    local estado = guarda:estado_ia()
    if estado == "perseguindo" and estado_guarda ~= "perseguindo" then
      som(SONS .. "alerta.wav", gx, gy, gz, 1.0)
      log("VALE", "alerta")
    end
    estado_guarda = estado
    if not derrubado and d < 2.5 then
      hud.texto(t("F: derrubar o guarda"), 0.5, 0.82, {tamanho = 0.035, alinhar = "centro"})
    end
    if tempo() < pego_ate then
      hud.texto(t("O guarda te pegou! De volta ao começo."), 0.5, 0.4, {tamanho = 0.045, alinhar = "centro"})
    end
  end
  if venceu then
    hud.texto(t("Você juntou todos os cristais!"), 0.5, 0.3, {tamanho = 0.05, alinhar = "centro"})
  end
end
