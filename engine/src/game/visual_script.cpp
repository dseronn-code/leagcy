#include "game/visual_script.h"

#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace game {
namespace {

using nlohmann::json;

std::string luaString(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (c == '\n') {
      o += "\\n";
      continue;
    }
    o += c;
  }
  return o + "\"";
}

bool validName(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  return true;
}

struct Compiler {
  std::map<int, json> nodes;
  std::map<int, std::map<std::string, std::vector<int>>> out;  // nó -> saída -> destinos
  std::string error;

  // Valor de um campo: número, "texto", "$variavel", "#tempo", "@entidade.x|y|z|distancia" ("eu" = a própria) ou
  // ["op", a, b] com + - * / e comparações.
  std::string value(const json& v) {
    if (v.is_number()) return v.dump();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_array() && v.size() == 3 && v[0].is_string()) {
      const std::string op = v[0].get<std::string>();
      static const std::map<std::string, std::string> ops = {{"+", "+"},  {"-", "-"},  {"*", "*"},   {"/", "/"},
                                                             {"==", "=="}, {"!=", "~="}, {"<", "<"},  {"<=", "<="},
                                                             {">", ">"},   {">=", ">="}, {"e", "and"}, {"ou", "or"}};
      auto it = ops.find(op);
      if (it == ops.end()) {
        error = "operador desconhecido: " + op;
        return "nil";
      }
      // Aritmética e ordem com números (variável ainda vazia vale 0); == e != comparam o valor como está.
      const bool arith = op == "+" || op == "-" || op == "*" || op == "/" || op == "<" || op == "<=" || op == ">" ||
                         op == ">=";
      const std::string a = value(v[1]), b = value(v[2]);
      return arith ? "(_num(" + a + ") " + it->second + " _num(" + b + "))" : "(" + a + " " + it->second + " " + b + ")";
    }
    if (!v.is_string()) return "nil";
    const std::string s = v.get<std::string>();
    if (s.size() > 1 && s[0] == '$') return "V[" + luaString(s.substr(1)) + "]";
    if (s == "#tempo") return "tempo()";
    if (s.size() > 1 && s[0] == '@') {
      const size_t dot = s.find('.');
      const std::string ent = s.substr(1, dot == std::string::npos ? std::string::npos : dot - 1);
      const std::string comp = dot == std::string::npos ? "x" : s.substr(dot + 1);
      if (comp == "distancia") return "_dist(" + luaString(ent) + ")";
      const int idx = comp == "x" ? 1 : comp == "y" ? 2 : comp == "z" ? 3 : 0;
      if (!idx) {
        error = "campo desconhecido em " + s + " (x, y, z ou distancia)";
        return "nil";
      }
      return "(select(" + std::to_string(idx) + ", _pos(" + luaString(ent) + ")))";
    }
    return luaString(s);
  }

  // Texto com {expressão} dentro: "Moedas: {$moedas}".
  std::string text(const json& v) {
    if (!v.is_string()) return "tostring(" + value(v) + ")";
    const std::string s = v.get<std::string>();
    std::string o;
    size_t i = 0;
    while (i < s.size()) {
      const size_t a = s.find('{', i);
      if (a == std::string::npos) {
        o += (o.empty() ? "" : " .. ") + luaString(s.substr(i));
        break;
      }
      const size_t b = s.find('}', a);
      if (b == std::string::npos) {
        o += (o.empty() ? "" : " .. ") + luaString(s.substr(i));
        break;
      }
      if (a > i) o += (o.empty() ? "" : " .. ") + luaString(s.substr(i, a - i));
      o += (o.empty() ? "" : " .. ") + std::string("_txt(") + value(json(s.substr(a + 1, b - a - 1))) + ")";
      i = b + 1;
    }
    return o.empty() ? "\"\"" : o;
  }

  float num(const json& n, const char* k, float def) { return n.contains(k) && n[k].is_number() ? n[k].get<float>() : def; }
  std::string field(const json& n, const char* k, const json& def) { return value(n.contains(k) ? n[k] : def); }

  void chain(std::ostringstream& o, int id, const std::string& in, std::set<int>& path) {
    for (int next : outputs(id, "proximo")) node(o, next, in, path);
  }
  std::vector<int> outputs(int id, const std::string& name) {
    auto a = out.find(id);
    if (a == out.end()) return {};
    auto b = a->second.find(name);
    return b == a->second.end() ? std::vector<int>{} : b->second;
  }

  void node(std::ostringstream& o, int id, const std::string& in, std::set<int>& path) {
    if (!error.empty()) return;
    auto it = nodes.find(id);
    if (it == nodes.end()) {
      error = "ligação para o nó " + std::to_string(id) + ", que não existe";
      return;
    }
    if (path.count(id)) {
      error = "ciclo no grafo passando pelo nó " + std::to_string(id) + " (para repetir, use o nó \"repetir\")";
      return;
    }
    path.insert(id);
    const json& n = it->second;
    const std::string t = n.value("tipo", "");
    if (t == "log") {
      o << in << "log(\"VS\", " << text(n.value("texto", json(""))) << ")\n";
    } else if (t == "mover") {
      const bool rel = n.value("relativo", false);
      const std::string x = field(n, "x", 0), y = field(n, "y", 0), z = field(n, "z", 0);
      o << in << "do local x0, y0, z0 = E:posicao()\n";
      if (rel) o << in << "  _mover(x0 + _num(" << x << "), y0 + _num(" << y << "), z0 + _num(" << z << "), ";
      else o << in << "  _mover(_num(" << x << "), _num(" << y << "), _num(" << z << "), ";
      o << "_num(" << field(n, "duracao", 0) << ")) end\n";
    } else if (t == "girar") {
      o << in << "_girar(_num(" << field(n, "x", 0) << "), _num(" << field(n, "y", 0) << "), _num(" << field(n, "z", 0)
        << "), _num(" << field(n, "duracao", 0) << "))\n";
    } else if (t == "esperar") {
      o << in << "_esperar(_num(" << field(n, "segundos", 1) << "))\n";
    } else if (t == "som") {
      o << in << "do local x, y, z = E:posicao(); som(" << value(n.value("arquivo", json(""))) << ", x, y, z, _num("
        << field(n, "volume", 1) << ")) end\n";
    } else if (t == "impulso") {
      o << in << "E:impulso(_num(" << field(n, "x", 0) << "), _num(" << field(n, "y", 0) << "), _num(" << field(n, "z", 0)
        << "))\n";
    } else if (t == "particulas") {
      o << in << "E:particulas(" << field(n, "ligado", true) << ")\n";
    } else if (t == "animar") {
      o << in << "E:animar(" << value(n.value("animacao", json(""))) << ", 0.2, " << field(n, "laco", false) << ")\n";
    } else if (t == "explosao") {
      o << in << "do local x, y, z = E:posicao(); explosao(x, y, z, _num(" << field(n, "escala", 1) << ")) end\n";
    } else if (t == "definir" || t == "somar") {
      const std::string var = n.value("variavel", "");
      if (!validName(var)) {
        error = "nó " + std::to_string(id) + ": variável inválida '" + var + "'";
        return;
      }
      if (t == "definir") o << in << "V[" << luaString(var) << "] = " << field(n, "valor", 0) << "\n";
      else o << in << "V[" << luaString(var) << "] = _num(V[" << luaString(var) << "]) + _num(" << field(n, "valor", 1) << ")\n";
    } else if (t == "texto") {
      o << in << "_textos[#_textos + 1] = {" << text(n.value("texto", json(""))) << ", _num(" << field(n, "x", 0.5)
        << "), _num(" << field(n, "y", 0.2) << "), tempo() + _num(" << field(n, "segundos", 2) << ")}\n";
    } else if (t == "lua") {
      o << in << "do " << n.value("codigo", "") << " end\n";
    } else if (t == "parar") {
      o << in << "do return end\n";
    } else if (t == "se") {
      const std::string a = field(n, "a", 0), b = field(n, "b", 0), op = n.value("op", "==");
      const std::string cond = value(json::array({op, n.contains("a") ? n["a"] : json(0), n.contains("b") ? n["b"] : json(0)}));
      (void)a;
      (void)b;
      o << in << "if " << cond << " then\n";
      for (int s : outputs(id, "sim")) node(o, s, in + "  ", path);
      o << in << "else\n";
      for (int s : outputs(id, "nao")) node(o, s, in + "  ", path);
      o << in << "end\n";
    } else if (t == "repetir") {
      const std::string v = "_i" + std::to_string(id);
      o << in << "for " << v << " = 1, _num(" << field(n, "vezes", 2) << ") do\n";
      o << in << "  V[\"indice\"] = " << v << "\n";
      for (int s : outputs(id, "corpo")) node(o, s, in + "  ", path);
      o << in << "end\n";
      for (int s : outputs(id, "depois")) node(o, s, in, path);
    } else {
      error = "nó " + std::to_string(id) + ": tipo de ação desconhecido '" + t + "'";
      return;
    }
    if (t != "se" && t != "repetir") chain(o, id, in, path);
    path.erase(id);
  }
};

const std::set<std::string> kEvents = {"ao_iniciar", "ao_atualizar", "ao_tecla", "a_cada", "ao_chegar_perto",
                                       "ao_sair_de_perto", "quando"};

}  // namespace

std::string compileVisualScript(const json& graph, std::string& error) {
  Compiler c;
  if (!graph.is_object() || !graph.contains("nos") || !graph["nos"].is_array()) {
    error = "o script visual precisa de \"nos\" (lista de nós)";
    return {};
  }
  for (const json& n : graph["nos"]) {
    if (!n.contains("id") || !n["id"].is_number_integer() || !n.contains("tipo")) {
      error = "nó sem \"id\" inteiro ou sem \"tipo\": " + n.dump();
      return {};
    }
    const int id = n["id"].get<int>();
    if (c.nodes.count(id)) {
      error = "id repetido: " + std::to_string(id);
      return {};
    }
    c.nodes[id] = n;
  }
  for (const json& l : graph.value("ligacoes", json::array())) {
    int from = 0, to = 0;
    std::string pin = "proximo";
    if (l.is_array() && l.size() >= 2) {
      from = l[0].get<int>();
      to = l[1].get<int>();
      if (l.size() >= 3 && l[2].is_string()) pin = l[2].get<std::string>();
    } else if (l.is_object()) {
      from = l.value("de", 0);
      to = l.value("para", 0);
      pin = l.value("saida", pin);
    } else {
      error = "ligação inválida: " + l.dump();
      return {};
    }
    if (!c.nodes.count(from) || !c.nodes.count(to)) {
      error = "ligação " + std::to_string(from) + " -> " + std::to_string(to) + " cita nó que não existe";
      return {};
    }
    c.out[from][pin].push_back(to);
  }

  std::ostringstream o;
  o << "-- Gerado do script visual (não edite: mude o .json). Cada evento roda numa corrotina; um disparo novo do mesmo\n"
       "-- evento espera o anterior terminar.\n"
       "local E\nlocal V = {}\nlocal _ativo, _co, _textos, _cada, _perto = {}, {}, {}, {}, {}\n"
       "local function _num(v) return tonumber(v) or 0 end\n"
       "local function _txt(v) if type(v) == 'number' and v == math.floor(v) then return string.format('%d', v) end "
       "return tostring(v) end\n"
       "local function _esperar(s) local t = 0 while t < s do t = t + coroutine.yield() end end\n"
       "local function _pos(nome) local x = nome == 'eu' and E or achar(nome) if not x then return 0, 0, 0 end "
       "return x:posicao() end\n"
       "local function _dist(nome) local ax, ay, az = E:posicao() local bx, by, bz = _pos(nome) "
       "return math.sqrt((ax - bx)^2 + (ay - by)^2 + (az - bz)^2) end\n"
       "local function _mover(x, y, z, dur)\n"
       "  if dur <= 0 then E:mover_para(x, y, z) return end\n"
       "  local x0, y0, z0 = E:posicao() local t = 0\n"
       "  while t < dur do t = t + coroutine.yield() local k = math.min(t / dur, 1) local s = k * k * (3 - 2 * k)\n"
       "    E:mover_para(x0 + (x - x0) * s, y0 + (y - y0) * s, z0 + (z - z0) * s) end\nend\n"
       "local function _girar(x, y, z, dur)\n"
       "  if dur <= 0 then E:girar(x, y, z) return end\n"
       "  local t, feito = 0, 0\n"
       "  while t < dur do t = t + coroutine.yield() local k = math.min(t / dur, 1)\n"
       "    E:girar(x * (k - feito), y * (k - feito), z * (k - feito)) feito = k end\nend\n"
       "local function _disparar(id, f)\n"
       "  local c = _ativo[id] if c and coroutine.status(c) ~= 'dead' then return end\n"
       "  c = coroutine.create(f) _ativo[id] = c _co[#_co + 1] = c\nend\n";
  std::ostringstream ini, upd;
  for (const auto& [id, n] : c.nodes) {
    const std::string t = n.value("tipo", "");
    if (!kEvents.count(t)) continue;
    const std::string fn = "_ev" + std::to_string(id);
    std::set<int> path;
    std::ostringstream body;
    c.chain(body, id, "  ", path);
    if (!c.error.empty()) {
      error = c.error;
      return {};
    }
    o << "local function " << fn << "()\n" << body.str() << "end\n";
    const std::string k = std::to_string(id);
    if (t == "ao_iniciar") {
      ini << "  _disparar(" << k << ", " << fn << ")\n";
    } else if (t == "ao_atualizar") {
      upd << "  _disparar(" << k << ", " << fn << ")\n";
    } else if (t == "ao_tecla") {
      upd << "  if tecla_apertada(" << luaString(n.value("tecla", "e")) << ") then _disparar(" << k << ", " << fn
          << ") end\n";
    } else if (t == "a_cada") {
      upd << "  _cada[" << k << "] = (_cada[" << k << "] or 0) + dt\n  if _cada[" << k << "] >= _num("
          << c.field(n, "segundos", 1) << ") then _cada[" << k << "] = _cada[" << k << "] - _num(" << c.field(n, "segundos", 1)
          << ") _disparar(" << k << ", " << fn << ") end\n";
    } else if (t == "ao_chegar_perto" || t == "ao_sair_de_perto") {
      const bool enter = t == "ao_chegar_perto";
      upd << "  do local dentro = _dist(" << luaString(n.value("alvo", "jogador")) << ") <= _num("
          << c.field(n, "distancia", 2) << ")\n";
      upd << (enter ? "    if dentro and not _perto[" : "    if not dentro and _perto[") << k << "] then _disparar(" << k
          << ", " << fn << ") end\n    _perto[" << k << "] = dentro end\n";
    } else if (t == "quando") {  // a condição passa de falsa a verdadeira
      const std::string cond = c.value(json::array({n.value("op", "=="), n.contains("a") ? n["a"] : json(0),
                                                     n.contains("b") ? n["b"] : json(0)}));
      upd << "  do local agora = " << cond << " and true or false\n    if agora and not _perto[" << k
          << "] then _disparar(" << k << ", " << fn << ") end\n    _perto[" << k << "] = agora end\n";
    }
  }
  if (!c.error.empty()) {
    error = c.error;
    return {};
  }
  o << "function iniciar(e)\n  E = e\n" << ini.str() << "end\n";
  o << "function atualizar(e, dt)\n  E = e\n" << upd.str();
  o << "  for i = #_co, 1, -1 do\n    local ok, err = coroutine.resume(_co[i], dt)\n"
       "    if not ok then error(err) end\n"
       "    if coroutine.status(_co[i]) == 'dead' then table.remove(_co, i) end\n  end\n";
  o << "  local agora = tempo()\n  for i = #_textos, 1, -1 do\n    local tx = _textos[i]\n"
       "    if agora >= tx[4] then table.remove(_textos, i) else hud.texto(tx[1], tx[2], tx[3]) end\n  end\nend\n";
  return o.str();
}

}  // namespace game
