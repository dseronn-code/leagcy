-- Inventário e missões (incluir("lib/inventario.lua")). Os dados ficam em tabelas simples, prontas para
-- salvar_jogo("slot", {inventario = inventario.dados(), missoes = missoes.dados()}) e voltar com
-- inventario.carregar(d.inventario) / missoes.carregar(d.missoes).
--
--   inventario.adicionar("moeda", 10)       inventario.remover("moeda", 3)   -> falso se não tiver o bastante
--   inventario.quantidade("chave")           inventario.tem("chave")          inventario.itens() -> {{nome, qtd}...}
--   inventario.desenhar(x, y)                -- lista na tela (HUD)
--
--   missoes.iniciar("ponte", "Consertar a ponte", {"tabuas", "pregos"})
--   missoes.cumprir("ponte", "tabuas")       -- objetivo feito; com todos feitos a missão fica concluída
--   missoes.estado("ponte")                  -> nil, "ativa" ou "concluida"
--   missoes.ao_concluir = function(id) ... end
--   missoes.desenhar(x, y)                   -- missões ativas e objetivos na tela

inventario = inventario or {}
local itens = {}

function inventario.adicionar(nome, qtd)
  qtd = qtd or 1
  itens[nome] = (itens[nome] or 0) + qtd
  return itens[nome]
end

function inventario.remover(nome, qtd)
  qtd = qtd or 1
  if (itens[nome] or 0) < qtd then return false end
  itens[nome] = itens[nome] - qtd
  if itens[nome] == 0 then itens[nome] = nil end
  return true
end

function inventario.quantidade(nome) return itens[nome] or 0 end
function inventario.tem(nome, qtd) return (itens[nome] or 0) >= (qtd or 1) end

function inventario.itens()
  local lista = {}
  for nome, qtd in pairs(itens) do lista[#lista + 1] = {nome, qtd} end
  table.sort(lista, function(a, b) return a[1] < b[1] end)
  return lista
end

function inventario.dados()
  local copia = {}
  for nome, qtd in pairs(itens) do copia[nome] = qtd end
  return copia
end

function inventario.carregar(d)
  itens = {}
  for nome, qtd in pairs(d or {}) do itens[nome] = qtd end
end

function inventario.desenhar(x, y)
  local lista = inventario.itens()
  hud.retangulo(x, y, 0.22, 0.05 + 0.04 * #lista, {0, 0, 0, 0.45})
  hud.texto(t("Inventário"), x + 0.01, y + 0.008, {tamanho = 0.03, cor = {1, 0.85, 0.35}})
  for i, it in ipairs(lista) do
    hud.texto(t(it[1]), x + 0.012, y + 0.008 + 0.04 * i, {tamanho = 0.027})
    hud.texto(tostring(it[2]), x + 0.21, y + 0.008 + 0.04 * i, {tamanho = 0.027, alinhar = "direita"})
  end
end

missoes = missoes or {}
local lista_missoes = {}  -- id -> {titulo, objetivos = {{nome, feito}}, estado}
local ordem = {}

function missoes.iniciar(id, titulo, objetivos)
  if lista_missoes[id] then return end
  local m = {titulo = titulo, objetivos = {}, estado = "ativa"}
  for _, o in ipairs(objetivos or {}) do m.objetivos[#m.objetivos + 1] = {o, false} end
  lista_missoes[id] = m
  ordem[#ordem + 1] = id
end

function missoes.cumprir(id, objetivo)
  local m = lista_missoes[id]
  if not m or m.estado ~= "ativa" then return false end
  local todos = true
  for _, o in ipairs(m.objetivos) do
    if o[1] == objetivo then o[2] = true end
    todos = todos and o[2]
  end
  if todos then
    m.estado = "concluida"
    if missoes.ao_concluir then missoes.ao_concluir(id) end
  end
  return true
end

function missoes.estado(id) return lista_missoes[id] and lista_missoes[id].estado end

function missoes.dados()
  local d = {}
  for _, id in ipairs(ordem) do
    local m = lista_missoes[id]
    local objs = {}
    for _, o in ipairs(m.objetivos) do objs[#objs + 1] = {o[1], o[2]} end
    d[#d + 1] = {id = id, titulo = m.titulo, estado = m.estado, objetivos = objs}
  end
  return d
end

function missoes.carregar(d)
  lista_missoes, ordem = {}, {}
  for _, m in ipairs(d or {}) do
    local objs = {}
    for _, o in ipairs(m.objetivos or {}) do objs[#objs + 1] = {o[1], o[2]} end
    lista_missoes[m.id] = {titulo = m.titulo, estado = m.estado, objetivos = objs}
    ordem[#ordem + 1] = m.id
  end
end

function missoes.desenhar(x, y)
  local linhas = 0
  for _, id in ipairs(ordem) do
    local m = lista_missoes[id]
    if m.estado == "ativa" then
      hud.texto(t(m.titulo), x, y + 0.04 * linhas, {tamanho = 0.03, cor = {1, 0.85, 0.35}})
      linhas = linhas + 1
      for _, o in ipairs(m.objetivos) do
        hud.texto((o[2] and "[x] " or "[ ] ") .. t(o[1]), x + 0.015, y + 0.04 * linhas,
                  {tamanho = 0.026, cor = o[2] and {0.5, 1, 0.5, 0.8} or {1, 1, 1}})
        linhas = linhas + 1
      end
    end
  end
end
