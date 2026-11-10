-- Bola que pula sozinha a cada 2 segundos (impulso para cima, proporcional à massa padrão de 1 kg/L).
local proximo = 1

function atualizar(e, dt)
  if tempo() >= proximo then
    e:impulso(0, 300, 0)
    proximo = proximo + 2
  end
end
