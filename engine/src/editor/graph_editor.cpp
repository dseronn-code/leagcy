#include "editor/graph_editor.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <queue>
#include <set>

#include <imgui.h>

namespace editor {
namespace {

using nlohmann::json;

// Campos iniciais de cada tipo de nó novo (os mesmos nomes de visual_script.cpp e node_material.cpp).
const std::vector<std::pair<std::string, json>>& scriptTypes() {
  static const std::vector<std::pair<std::string, json>> t = {
      {"ao_iniciar", json::object()},
      {"ao_atualizar", json::object()},
      {"ao_tecla", {{"tecla", "e"}}},
      {"a_cada", {{"segundos", 1.0}}},
      {"ao_chegar_perto", {{"alvo", "jogador"}, {"distancia", 2.0}}},
      {"ao_sair_de_perto", {{"alvo", "jogador"}, {"distancia", 2.0}}},
      {"quando", {{"a", "$variavel"}, {"op", ">="}, {"b", 1}}},
      {"log", {{"texto", "oi"}}},
      {"texto", {{"texto", "Olá"}, {"segundos", 2.0}}},
      {"mover", {{"x", 0.0}, {"y", 1.0}, {"z", 0.0}, {"relativo", true}, {"duracao", 1.0}}},
      {"girar", {{"x", 0.0}, {"y", 90.0}, {"z", 0.0}, {"duracao", 1.0}}},
      {"esperar", {{"segundos", 1.0}}},
      {"som", {{"arquivo", "som.wav"}, {"volume", 1.0}}},
      {"impulso", {{"x", 0.0}, {"y", 5.0}, {"z", 0.0}}},
      {"definir", {{"variavel", "x"}, {"valor", 0}}},
      {"somar", {{"variavel", "x"}, {"valor", 1}}},
      {"se", {{"a", "$x"}, {"op", ">="}, {"b", 1}}},
      {"repetir", {{"vezes", 3}}},
      {"particulas", {{"ligado", true}}},
      {"animar", {{"animacao", "Walk"}, {"laco", true}}},
      {"explosao", {{"escala", 1.0}}},
      {"lua", {{"codigo", "log('lua')"}}},
      {"parar", json::object()}};
  return t;
}
const std::vector<std::pair<std::string, json>>& materialTypes() {
  static const std::vector<std::pair<std::string, json>> t = {
      {"ruido", {{"escala", 4}, {"oitavas", 5}, {"semente", 1}}},
      {"voronoi", {{"escala", 6}, {"saida", "distancia"}}},
      {"tijolos", {{"linhas", 8}, {"colunas", 4}, {"argamassa", 0.06}}},
      {"listras", {{"escala", 8}, {"inclinacao", 0.0}}},
      {"gradiente", {{"eixo", "v"}}},
      {"constante", {{"valor", 0.5}}},
      {"misturar", {{"a", 0.0}, {"b", 1.0}, {"t", 0.5}}},
      {"multiplicar", {{"a", 1.0}, {"b", 1.0}}},
      {"somar", {{"a", 0.0}, {"b", 0.0}}},
      {"inverter", {{"entrada", 0.0}}},
      {"contraste", {{"entrada", 0.0}, {"min", 0.3}, {"max", 0.7}}},
      {"rampa", {{"entrada", 0.0}, {"cores", json::array({json::array({0, "#000000"}), json::array({1, "#ffffff"})})}}},
      {"deformar", {{"entrada", 0.0}, {"x", 0.5}, {"y", 0.5}, {"forca", 0.05}}}};
  return t;
}
const std::set<std::string> kEvents = {"ao_iniciar", "ao_atualizar", "ao_tecla", "a_cada", "ao_chegar_perto",
                                       "ao_sair_de_perto", "quando"};
constexpr float kNodeWidth = 210.0f, kRow = 24.0f, kHeader = 24.0f;
constexpr int kOutputNode = -1000;  // a "saida" do material, desenhada como um nó

bool isLink(const json& v) { return v.is_object() && v.contains("no"); }

}  // namespace

bool GraphEditor::open(const std::string& path) {
  std::ifstream f(path);
  if (!f) {
    status_ = "não abri " + path;
    return false;
  }
  try {
    graph_ = json::parse(f);
  } catch (const json::exception& e) {
    status_ = std::string("JSON inválido: ") + e.what();
    return false;
  }
  if (!graph_.contains("nos")) graph_["nos"] = json::array();
  if (!material() && !graph_.contains("ligacoes")) graph_["ligacoes"] = json::array();
  path_ = path;
  layout();
  status_ = "aberto " + path;
  return true;
}

bool GraphEditor::save() {
  if (path_.empty()) return false;
  std::ofstream f(path_);
  f << graph_.dump(2);
  status_ = f ? "salvo " + path_ : "falha ao salvar";
  return bool(f);
}

json* GraphEditor::node(int id) {
  for (json& n : graph_["nos"])
    if (n.value("id", -1) == id) return &n;
  return nullptr;
}

std::vector<std::string> GraphEditor::outputs(const json& n) const {
  if (material()) return {"valor"};
  const std::string t = n.value("tipo", "");
  if (t == "se") return {"sim", "nao"};
  if (t == "repetir") return {"corpo", "depois"};
  if (t == "parar") return {};
  return {"proximo"};
}

void GraphEditor::layout() {
  // Nós sem posição: colunas pela distância (em ligações) até um evento ou até a saída, linhas na ordem.
  std::map<int, int> depth;
  std::queue<int> q;
  std::map<int, std::vector<int>> next;
  if (material()) {
    for (const json& n : graph_["nos"])
      for (auto& [k, v] : n.items())
        if (isLink(v)) next[n.value("id", 0)].push_back(v["no"].get<int>());  // ao contrário: da saída para trás
    for (auto& [k, v] : graph_["saida"].items())
      if (isLink(v)) {
        depth[v["no"].get<int>()] = 1;
        q.push(v["no"].get<int>());
      }
  } else {
    for (const json& l : graph_["ligacoes"]) {
      const int from = l.is_array() ? l[0].get<int>() : l.value("de", 0), to = l.is_array() ? l[1].get<int>() : l.value("para", 0);
      next[from].push_back(to);
    }
    for (const json& n : graph_["nos"])
      if (kEvents.count(n.value("tipo", ""))) {
        depth[n["id"].get<int>()] = 0;
        q.push(n["id"].get<int>());
      }
  }
  while (!q.empty()) {
    const int a = q.front();
    q.pop();
    for (int b : next[a])
      if (!depth.count(b) && depth.size() < 10000) {
        depth[b] = depth[a] + 1;
        q.push(b);
      }
  }
  std::map<int, int> rows;
  int maxDepth = 0;
  for (auto& [id, d] : depth) maxDepth = std::max(maxDepth, d);
  for (json& n : graph_["nos"]) {
    if (n.contains("pos_editor")) continue;
    const int id = n.value("id", 0);
    int d = depth.count(id) ? depth[id] : maxDepth + 1;
    if (material()) d = maxDepth + 1 - d;  // material: da esquerda (geradores) para a direita (saída)
    const int r = rows[d]++;
    n["pos_editor"] = json::array({40.0f + float(d) * (kNodeWidth + 70.0f), 30.0f + float(r) * 170.0f});
  }
  if (material() && !graph_["saida"].contains("pos_editor"))
    graph_["saida"]["pos_editor"] = json::array({40.0f + float(maxDepth + 2) * (kNodeWidth + 70.0f), 30.0f});
}

int GraphEditor::addNode(const std::string& type, glm::vec2 pos) {
  int id = 1;
  for (const json& n : graph_["nos"]) id = std::max(id, n.value("id", 0) + 1);
  json n = {{"id", id}, {"tipo", type}};
  for (const auto& [t, fields] : (material() ? materialTypes() : scriptTypes()))
    if (t == type)
      for (auto& [k, v] : fields.items()) n[k] = v;
  n["pos_editor"] = json::array({pos.x, pos.y});
  graph_["nos"].push_back(n);
  selected_ = id;
  return id;
}

bool GraphEditor::connect(int from, const std::string& pin, int to) {
  if (from == to) return false;
  if (material()) {  // o campo `pin` de `to` passa a ler a saída de `from`
    json* t = to == kOutputNode ? &graph_["saida"] : node(to);
    if (!t || !node(from)) return false;
    (*t)[pin] = json{{"no", from}};
    return true;
  }
  if (!node(from) || !node(to) || kEvents.count(node(to)->value("tipo", ""))) return false;
  for (const json& l : graph_["ligacoes"])
    if (l.is_array() && l.size() >= 2 && l[0] == from && l[1] == to && (l.size() > 2 ? l[2].get<std::string>() : "proximo") == pin)
      return false;
  graph_["ligacoes"].push_back(pin == "proximo" ? json::array({from, to}) : json::array({from, to, pin}));
  return true;
}

void GraphEditor::removeNode(int id) {
  json& ns = graph_["nos"];
  for (size_t i = 0; i < ns.size(); ++i)
    if (ns[i].value("id", -1) == id) {
      ns.erase(i);
      break;
    }
  if (material()) {
    auto unlink = [&](json& n) {
      for (auto& [k, v] : n.items())
        if (isLink(v) && v["no"] == id) v = 0.0;
    };
    for (json& n : ns) unlink(n);
    unlink(graph_["saida"]);
  } else {
    json keep = json::array();
    for (const json& l : graph_["ligacoes"]) {
      const int a = l.is_array() ? l[0].get<int>() : l.value("de", 0), b = l.is_array() ? l[1].get<int>() : l.value("para", 0);
      if (a != id && b != id) keep.push_back(l);
    }
    graph_["ligacoes"] = keep;
  }
  if (selected_ == id) selected_ = -1;
}

void GraphEditor::draw() {
  if (path_.empty()) return;
  ImGui::SetNextWindowSize(ImVec2(900, 520), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(330, 200), ImGuiCond_FirstUseEver);
  bool open = true;
  ImGui::Begin(("Grafo: " + path_ + "###grafo").c_str(), &open);
  if (ImGui::Button("Salvar")) save();
  ImGui::SameLine();
  ImGui::TextDisabled("%s | botão direito: novo nó | arraste de uma saída (direita) até uma entrada | Delete apaga",
                      status_.c_str());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 canvas = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  dl->AddRectFilled(canvas, ImVec2(canvas.x + avail.x, canvas.y + avail.y), IM_COL32(28, 30, 34, 255));
  for (float x = std::fmod(scroll_.x, 32.0f); x < avail.x; x += 32.0f)
    dl->AddLine(ImVec2(canvas.x + x, canvas.y), ImVec2(canvas.x + x, canvas.y + avail.y), IM_COL32(45, 48, 54, 255));
  for (float y = std::fmod(scroll_.y, 32.0f); y < avail.y; y += 32.0f)
    dl->AddLine(ImVec2(canvas.x, canvas.y + y), ImVec2(canvas.x + avail.x, canvas.y + y), IM_COL32(45, 48, 54, 255));
  ImGui::InvisibleButton("##fundo", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool canvasHovered = ImGui::IsItemHovered();
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
    scroll_.x += ImGui::GetIO().MouseDelta.x;
    scroll_.y += ImGui::GetIO().MouseDelta.y;
  }
  if (canvasHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)) {
    popupPos_ = glm::vec2(ImGui::GetMousePos().x - canvas.x - scroll_.x, ImGui::GetMousePos().y - canvas.y - scroll_.y);
    ImGui::OpenPopup("novo_no");
  }
  if (ImGui::BeginPopup("novo_no")) {
    for (const auto& [t, f] : (material() ? materialTypes() : scriptTypes()))
      if (ImGui::MenuItem(t.c_str())) addNode(t, popupPos_);
    ImGui::EndPopup();
  }
  dl->PushClipRect(canvas, ImVec2(canvas.x + avail.x, canvas.y + avail.y), true);
  const ImVec2 origin(canvas.x + scroll_.x, canvas.y + scroll_.y);
  auto posOf = [&](const json& n) {
    const json& p = n.value("pos_editor", json::array({0, 0}));
    return ImVec2(origin.x + p[0].get<float>(), origin.y + p[1].get<float>());
  };
  // Pinos (em tela) achados ao desenhar: entradas (nó, campo) e saídas (nó, saída).
  struct Pin {
    int node;
    std::string name;
    ImVec2 at;
  };
  std::vector<Pin> ins, outs;
  auto fieldRows = [&](const json& n) {
    std::vector<std::string> keys;
    for (auto& [k, v] : n.items())
      if (k != "id" && k != "tipo" && k != "pos_editor") keys.push_back(k);
    return keys;
  };
  std::vector<int> ids;
  for (const json& n : graph_["nos"]) ids.push_back(n.value("id", 0));
  if (material()) ids.push_back(kOutputNode);
  int clickedPin = 0;
  for (int id : ids) {
    json& n = id == kOutputNode ? graph_["saida"] : *node(id);
    const std::vector<std::string> keys = fieldRows(n);
    const std::vector<std::string> outsOf = id == kOutputNode ? std::vector<std::string>{} : outputs(n);
    const float h = kHeader + float(keys.size()) * kRow + float(material() ? 0 : outsOf.size()) * kRow + 8.0f;
    const ImVec2 p = posOf(n), q(p.x + kNodeWidth, p.y + h);
    const bool event = !material() && kEvents.count(n.value("tipo", ""));
    const ImU32 head = id == kOutputNode ? IM_COL32(120, 80, 40, 255) : event ? IM_COL32(150, 40, 40, 255) : IM_COL32(40, 80, 140, 255);
    dl->AddRectFilled(p, q, IM_COL32(52, 55, 62, 245), 6.0f);
    dl->AddRectFilled(p, ImVec2(q.x, p.y + kHeader), head, 6.0f, ImDrawFlags_RoundCornersTop);
    dl->AddRect(p, q, selected_ == id ? IM_COL32(255, 200, 60, 255) : IM_COL32(20, 20, 24, 255), 6.0f, 0,
                selected_ == id ? 2.0f : 1.0f);
    const std::string title = id == kOutputNode ? "saída do material" : std::to_string(id) + ": " + n.value("tipo", "?");
    dl->AddText(ImVec2(p.x + 8, p.y + 4), IM_COL32(255, 255, 255, 255), title.c_str());
    ImGui::PushID(id);
    // Arrastar pelo título.
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("titulo", ImVec2(kNodeWidth, kHeader));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      json& pe = n["pos_editor"];
      pe = json::array({pe[0].get<float>() + ImGui::GetIO().MouseDelta.x, pe[1].get<float>() + ImGui::GetIO().MouseDelta.y});
    }
    if (ImGui::IsItemClicked()) selected_ = id;
    // Entrada de execução (script): no título, à esquerda.
    if (!material() && !event) ins.push_back({id, "", ImVec2(p.x, p.y + kHeader * 0.5f)});
    // Campos.
    float y = p.y + kHeader + 4.0f;
    for (const std::string& k : keys) {
      json& v = n[k];
      ImGui::SetCursorScreenPos(ImVec2(p.x + 10, y));
      ImGui::PushItemWidth(kNodeWidth - 100);
      const std::string label = k + "##" + k;
      if (material() && (v.is_number() || isLink(v) || v.is_array() || (v.is_string() && v.get<std::string>().rfind('#', 0) == 0)))
        ins.push_back({id, k, ImVec2(p.x, y + kRow * 0.4f)});
      if (isLink(v)) {
        ImGui::TextDisabled("%s <- nó %d", k.c_str(), v["no"].get<int>());
      } else if (v.is_boolean()) {
        bool b = v.get<bool>();
        if (ImGui::Checkbox(label.c_str(), &b)) v = b;
      } else if (v.is_number_integer()) {
        int x = v.get<int>();
        if (ImGui::DragInt(label.c_str(), &x, 0.2f)) v = x;
      } else if (v.is_number()) {
        float x = v.get<float>();
        if (ImGui::DragFloat(label.c_str(), &x, 0.01f)) v = x;
      } else if (v.is_string()) {
        char buf[256];
        std::strncpy(buf, v.get<std::string>().c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) v = std::string(buf);
      } else {  // listas e expressões: o JSON em texto (vale quando é um JSON válido)
        char buf[512];
        std::strncpy(buf, v.dump().c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        if (ImGui::InputText(label.c_str(), buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
          try {
            v = json::parse(buf);
          } catch (const json::exception&) {
          }
        }
      }
      ImGui::PopItemWidth();
      y += kRow;
    }
    // Saídas: material = uma (o valor do nó, no título); script = uma por linha com o nome.
    if (id != kOutputNode) {
      if (material()) {
        outs.push_back({id, "valor", ImVec2(q.x, p.y + kHeader * 0.5f)});
      } else {
        for (const std::string& o : outsOf) {
          dl->AddText(ImVec2(q.x - 12 - ImGui::CalcTextSize(o.c_str()).x, y + 3), IM_COL32(200, 200, 200, 255), o.c_str());
          outs.push_back({id, o, ImVec2(q.x, y + kRow * 0.5f)});
          y += kRow;
        }
      }
    }
    ImGui::PopID();
  }
  // Fios: script = "ligacoes"; material = campos {"no": id}.
  auto findOut = [&](int nodeId, const std::string& pin) -> const Pin* {
    for (const Pin& o : outs)
      if (o.node == nodeId && (material() || o.name == pin)) return &o;
    return nullptr;
  };
  auto findIn = [&](int nodeId, const std::string& field) -> const Pin* {
    for (const Pin& i : ins)
      if (i.node == nodeId && i.name == field) return &i;
    return nullptr;
  };
  auto wire = [&](ImVec2 a, ImVec2 b, ImU32 c) {
    const float dx = std::max(std::abs(b.x - a.x) * 0.5f, 40.0f);
    dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, c, 2.5f);
  };
  if (material()) {
    for (int id : ids) {
      const json& n = id == kOutputNode ? graph_["saida"] : *node(id);
      for (auto& [k, v] : n.items())
        if (isLink(v)) {
          const Pin* a = findOut(v["no"].get<int>(), "valor");
          const Pin* b = findIn(id, k);
          if (a && b) wire(a->at, b->at, IM_COL32(120, 200, 255, 255));
        }
    }
  } else {
    for (const json& l : graph_["ligacoes"]) {
      const int from = l.is_array() ? l[0].get<int>() : l.value("de", 0), to = l.is_array() ? l[1].get<int>() : l.value("para", 0);
      const std::string pin = l.is_array() ? (l.size() > 2 ? l[2].get<std::string>() : "proximo") : l.value("saida", "proximo");
      const Pin* a = findOut(from, pin);
      const Pin* b = findIn(to, "");
      if (a && b) wire(a->at, b->at, IM_COL32(230, 230, 230, 255));
    }
  }
  // Pinos (bolinhas) e arrastar fios.
  const ImVec2 mouse = ImGui::GetMousePos();
  auto near = [&](ImVec2 a) { return (mouse.x - a.x) * (mouse.x - a.x) + (mouse.y - a.y) * (mouse.y - a.y) < 64.0f; };
  for (const Pin& i : ins) dl->AddCircleFilled(i.at, 5.0f, near(i.at) ? IM_COL32(255, 220, 90, 255) : IM_COL32(180, 180, 190, 255));
  for (const Pin& o : outs) {
    dl->AddCircleFilled(o.at, 5.0f, near(o.at) ? IM_COL32(255, 220, 90, 255) : IM_COL32(120, 220, 140, 255));
    if (near(o.at) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      dragFrom_ = o.node;
      dragPin_ = o.name;
      clickedPin = 1;
    }
    if (near(o.at) && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !material()) {  // apaga os fios desta saída
      json keep = json::array();
      for (const json& l : graph_["ligacoes"]) {
        const std::string pin = l.size() > 2 ? l[2].get<std::string>() : "proximo";
        if (!(l[0] == o.node && pin == o.name)) keep.push_back(l);
      }
      graph_["ligacoes"] = keep;
    }
  }
  for (const Pin& i : ins)  // botão direito numa entrada do material: desliga o campo
    if (material() && near(i.at) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      json& t = i.node == kOutputNode ? graph_["saida"] : *node(i.node);
      if (isLink(t[i.name])) t[i.name] = 0.0;
    }
  if (dragFrom_ >= 0) {
    const Pin* a = findOut(dragFrom_, dragPin_);
    if (a) wire(a->at, mouse, IM_COL32(255, 220, 90, 255));
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      for (const Pin& i : ins)
        if (near(i.at)) connect(dragFrom_, material() ? i.name : dragPin_, i.node);
      dragFrom_ = -1;
    }
  }
  (void)clickedPin;
  dl->PopClipRect();
  if (selected_ >= 0 && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete)) removeNode(selected_);
  ImGui::End();
  if (!open) path_.clear();
}

}  // namespace editor
