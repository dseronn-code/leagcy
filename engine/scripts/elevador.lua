-- Plataforma que sobe e desce 2 m a cada 4 s (use numa caixa "cinematico" ou "estatico").
local x, y0, z

function iniciar(e)
  x, y0, z = e:posicao()
  log("elevador pronto:", e.nome)
end

function atualizar(e, dt)
  e:mover_para(x, y0 + 1 - math.cos(tempo() * math.pi / 2), z)
end
