-- Velocímetro do carro: km/h, marcha e giro do motor, no canto de baixo à direita.
local RPM_MAXIMO = 6500

function atualizar(e, dt)
  local kmh = math.abs(e:velocidade_frente()) * 3.6
  local marcha = e:marcha()
  local rpm = e:rpm()
  hud.retangulo(0.74, 0.74, 0.24, 0.22, {0, 0, 0, 0.45})
  hud.texto(string.format("%d", math.floor(kmh + 0.5)), 0.95, 0.75, {tamanho = 0.1, alinhar = "direita"})
  hud.texto("km/h", 0.95, 0.85, {tamanho = 0.03, alinhar = "direita"})
  local nome = marcha < 0 and "R" or (marcha == 0 and "N" or tostring(marcha))
  hud.texto(nome, 0.76, 0.76, {tamanho = 0.06, cor = {1, 0.8, 0.2}})
  local fracao = rpm / RPM_MAXIMO
  local cor = fracao > 0.85 and {1, 0.2, 0.15} or {0.2, 0.85, 1}
  hud.barra(0.76, 0.9, 0.2, 0.025, fracao, cor, {1, 1, 1, 0.15})
  hud.texto("rpm", 0.76, 0.865, {tamanho = 0.025, cor = {1, 1, 1, 0.7}})
end
