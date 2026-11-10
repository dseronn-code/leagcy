-- Sequenciador de cutscenes (incluir("lib/sequencia.lua")): uma linha do tempo com trilha de câmera, legendas,
-- animações, sons, movimentos e chamadas, tocada pelo script.
--
--   local s = sequencia.nova{
--     camera = {                                   -- chaves da câmera (curva suave que passa por todas)
--       {t = 0, pos = {0, 2, 8}, alvo = {0, 1, 0}, fov = 50},
--       {t = 4, pos = {4, 3, 5}, alvo = {0, 1, 0}},
--       {t = 8, pos = {6, 1.5, 0}, alvo = {"raposa"}, fov = 35},  -- alvo pode ser uma entidade (segue ela)
--     },
--     eventos = {
--       {t = 0.5, fala = "Guarda: quem vem lá?", duracao = 2.5},   -- legenda na tarja de baixo (traduzida por t)
--       {t = 1, animar = {"guarda", "Survey", 0.3}},             -- entidade, clipe, transição, laço
--       {t = 2, som = {"sons/sino.wav", 0, 2, 0}},              -- arquivo, posição, volume
--       {t = 3, ir_para = {"raposa", 2, 0, 1}},                  -- anda até lá pela navegação
--       {t = 6, ragdoll = {"raposa", 0, 0, 60}},                 -- cai com um impulso
--       {t = 7, chamar = function() abrir_portao() end},
--     },
--     duracao = 9,       -- padrão: a última chave ou evento
--     barras = true,     -- tarjas pretas de cinema (entram e saem em 0,5 s)
--     pular = true,      -- Enter ou A pulam para o fim
--     travar = true,     -- o jogador não anda durante a cena (controle_jogador)
--   }
--   s:tocar()                    -- começa (ou recomeça)
--   s:atualizar(dt)              -- a cada quadro, no atualizar() do script
--   s:tocando(), s:tempo()       -- estado
--   s:pular()                    -- vai para o fim: os eventos `chamar` que faltam rodam e `ir_para` vira mover_para
--   s.ao_terminar = function() ... end
--
-- No fim a câmera volta para a do jogo e o jogador volta a andar. A cena pausa junto com o jogo (pausar()).

sequencia = sequencia or {}
local Seq = {}
Seq.__index = Seq

local function fim_da_cena(def)
  local d = def.duracao or 0
  for _, k in ipairs(def.camera or {}) do d = math.max(d, k.t) end
  for _, e in ipairs(def.eventos or {}) do d = math.max(d, e.t + (e.duracao or 0)) end
  return d
end

function sequencia.nova(def)
  local s = setmetatable({}, Seq)
  s.camera = def.camera or {}
  table.sort(s.camera, function(a, b) return a.t < b.t end)
  s.eventos = {}
  for i, e in ipairs(def.eventos or {}) do s.eventos[i] = e end
  table.sort(s.eventos, function(a, b) return a.t < b.t end)
  s.duracao = fim_da_cena(def)
  s.barras = def.barras ~= false
  s.pode_pular = def.pular ~= false
  s.travar = def.travar ~= false
  s.t = 0
  s.ativa = false
  return s
end

-- Ponto de um alvo/posição: {x, y, z} ou {"nome"} (posição atual da entidade; {"nome", dy} soma altura).
local function ponto(p)
  if type(p[1]) == "string" then
    local e = achar(p[1])
    if not e then return nil end
    local x, y, z = e:posicao()
    return {x, y + (p[2] or 0), z}
  end
  return {p[1], p[2], p[3]}
end

-- Hermite com tangentes de Catmull-Rom pelos tempos das chaves (passa por todas, velocidade contínua); na primeira e
-- na última chave a tangente é zero (a câmera arranca e para suave).
local function curva(chaves, campo, t, dim)
  local n = #chaves
  local function v(i)
    local x = chaves[i][campo]
    if x == nil then return nil end
    if dim == 1 then return {x} end
    return ponto(x)
  end
  -- Chaves sem o campo herdam a anterior (ex.: fov só na primeira).
  local vals = {}
  for i = 1, n do vals[i] = v(i) or vals[i - 1] end
  if not vals[1] then
    for i = 1, n do if vals[i] then vals[1] = vals[i] break end end
    if not vals[1] then return nil end
    for i = 2, n do vals[i] = vals[i] or vals[i - 1] end
  end
  if t <= chaves[1].t or n == 1 then return vals[1] end
  if t >= chaves[n].t then return vals[n] end
  local i = 1
  while chaves[i + 1].t < t do i = i + 1 end
  local t0, t1 = chaves[i].t, chaves[i + 1].t
  local h = math.max(t1 - t0, 1e-6)
  local s = (t - t0) / h
  local function tangente(k)
    if k <= 1 or k >= n then return nil end
    local dt = chaves[k + 1].t - chaves[k - 1].t
    local r = {}
    for c = 1, #vals[k] do r[c] = (vals[k + 1][c] - vals[k - 1][c]) / math.max(dt, 1e-6) end
    return r
  end
  local m0, m1 = tangente(i), tangente(i + 1)
  local s2, s3 = s * s, s * s * s
  local h00, h10, h01, h11 = 2 * s3 - 3 * s2 + 1, s3 - 2 * s2 + s, -2 * s3 + 3 * s2, s3 - s2
  local r = {}
  for c = 1, #vals[i] do
    r[c] = h00 * vals[i][c] + h01 * vals[i + 1][c] + h10 * h * (m0 and m0[c] or 0) + h11 * h * (m1 and m1[c] or 0)
  end
  return r
end

local function executar(e, pulando)
  if e.chamar then
    e.chamar()
  elseif e.animar and not pulando then
    local ent = achar(e.animar[1])
    if ent then ent:animar(e.animar[2], e.animar[3] or 0.2, e.animar[4] or false) end
  elseif e.som and not pulando then
    som(e.som[1], e.som[2] or 0, e.som[3] or 0, e.som[4] or 0, e.som[5])
  elseif e.ir_para then
    local ent = achar(e.ir_para[1])
    if ent then
      if pulando then ent:mover_para(e.ir_para[2], e.ir_para[3], e.ir_para[4])
      else ent:ir_para(e.ir_para[2], e.ir_para[3], e.ir_para[4]) end
    end
  elseif e.ragdoll then
    local ent = achar(e.ragdoll[1])
    if ent then ent:ragdoll(e.ragdoll[2] or 0, e.ragdoll[3] or 0, e.ragdoll[4] or 0) end
  end
end

function Seq:tocar()
  self.t = 0
  self.proximo = 1
  self.ativa = true
  if self.travar then controle_jogador(false) end
  self:aplicar_camera()
end

function Seq:tocando() return self.ativa end
function Seq:tempo() return self.t end

function Seq:aplicar_camera()
  if #self.camera == 0 then return end
  local p = curva(self.camera, "pos", self.t, 3)
  local a = curva(self.camera, "alvo", self.t, 3)
  local f = curva(self.camera, "fov", self.t, 1)
  if p and a then
    self.cam_pos, self.cam_alvo = p, a
    camera(p[1], p[2], p[3], a[1], a[2], a[3], f and f[1] or nil)
  end
end

function Seq:terminar()
  if not self.ativa then return end
  self.ativa = false
  camera_jogo()
  if self.travar then controle_jogador(true) end
  if self.ao_terminar then self.ao_terminar() end
end

function Seq:pular()
  if not self.ativa then return end
  while self.proximo <= #self.eventos do
    executar(self.eventos[self.proximo], true)
    self.proximo = self.proximo + 1
  end
  self.t = self.duracao
  self:terminar()
end

local function barras(fracao)
  local a = 0.11 * fracao
  if a <= 0 then return end
  hud.retangulo(0, 0, 1, a, {0, 0, 0, 1})
  hud.retangulo(0, 1 - a, 1, a, {0, 0, 0, 1})
end

function Seq:atualizar(dt)
  if not self.ativa then return end
  self.t = self.t + (dt or 0)
  while self.proximo <= #self.eventos and self.eventos[self.proximo].t <= self.t do
    executar(self.eventos[self.proximo], false)
    self.proximo = self.proximo + 1
  end
  if self.t >= self.duracao then
    self:terminar()
    return
  end
  self:aplicar_camera()
  -- Tarjas e legenda.
  if self.barras then
    local entra = math.min(self.t / 0.5, 1)
    local sai = math.min((self.duracao - self.t) / 0.5, 1)
    barras(math.min(entra, sai))
  end
  for _, e in ipairs(self.eventos) do
    if e.fala and self.t >= e.t and self.t < e.t + (e.duracao or 3) then
      hud.texto(t(e.fala), 0.5, 0.905, {tamanho = 0.036, alinhar = "centro"})
    end
  end
  if self.pode_pular then
    hud.texto(t("Enter: pular"), 0.97, 0.955, {tamanho = 0.024, alinhar = "direita", cor = {1, 1, 1, 0.55}})
    if tecla_apertada("enter") or controle_apertado("a") then self:pular() end
  end
end
