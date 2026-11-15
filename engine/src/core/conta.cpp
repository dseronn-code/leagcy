#include "core/conta.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

#include "core/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dpapi.h>
#include <shellapi.h>
#include <winhttp.h>
#endif

namespace conta {
namespace {

Conta g;
using json = nlohmann::json;

struct Resposta {
  int status = 0;
  std::string corpo;
};

#ifdef _WIN32
std::wstring largo(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// WinHTTP: o cliente HTTPS do próprio Windows (certificados do sistema, proxy do sistema).
bool http(const std::string& metodo, const std::string& url, const std::string& corpo, Resposta& r) {
  std::wstring wurl = largo(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256], caminho[2048];
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = caminho;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
  HINTERNET s = WinHttpOpen(L"Legacy/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return false;
  WinHttpSetTimeouts(s, 5000, 5000, 10000, 15000);
  bool ok = false;
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  if (c) {
    HINTERNET q = WinHttpOpenRequest(c, largo(metodo).c_str(), caminho, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (q) {
      std::wstring cab = L"Content-Type: application/json\r\n";
      if (!g.token.empty()) cab += L"Authorization: Bearer " + largo(g.token) + L"\r\n";
      if (WinHttpSendRequest(q, cab.c_str(), DWORD(-1), (LPVOID)corpo.data(), DWORD(corpo.size()), DWORD(corpo.size()), 0) &&
          WinHttpReceiveResponse(q, nullptr)) {
        DWORD st = 0, tam = sizeof(st);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st,
                            &tam, WINHTTP_NO_HEADER_INDEX);
        r.status = int(st);
        DWORD n = 0;
        while (WinHttpQueryDataAvailable(q, &n) && n > 0) {
          std::string buf(n, '\0');
          DWORD lidos = 0;
          if (!WinHttpReadData(q, buf.data(), n, &lidos)) break;
          r.corpo.append(buf.data(), lidos);
        }
        ok = true;
      }
      WinHttpCloseHandle(q);
    }
    WinHttpCloseHandle(c);
  }
  WinHttpCloseHandle(s);
  return ok;
}
#else
// Linux: o curl do sistema (cabeçalhos e corpo por arquivo temporário: o token não aparece na lista de processos).
bool http(const std::string& metodo, const std::string& url, const std::string& corpo, Resposta& r) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::temp_directory_path() / ("legacy_http_" + std::to_string(std::rand()));
  {
    std::ofstream(tmp.string() + ".corpo", std::ios::binary) << corpo;
    std::ofstream cab(tmp.string() + ".cab");
    cab << "Content-Type: application/json\n";
    if (!g.token.empty()) cab << "Authorization: Bearer " << g.token << "\n";
  }
  fs::permissions(tmp.string() + ".cab", fs::perms::owner_read | fs::perms::owner_write);
  const std::string cmd = "curl -s -m 15 -X " + metodo + " -H @" + tmp.string() + ".cab --data-binary @" + tmp.string() +
                          ".corpo -w '\\n%{http_code}' '" + url + "'";
  FILE* p = popen(cmd.c_str(), "r");
  std::string saida;
  if (p) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) saida.append(buf, n);
    pclose(p);
  }
  fs::remove(tmp.string() + ".cab");
  fs::remove(tmp.string() + ".corpo");
  const size_t k = saida.rfind('\n');
  if (k == std::string::npos) return false;
  r.status = std::atoi(saida.c_str() + k + 1);
  r.corpo = saida.substr(0, k);
  return r.status > 0;
}
#endif

bool chamar(const std::string& metodo, const std::string& rota, const json& corpo, json& resposta, std::string& erro) {
  Resposta r;
  if (!http(metodo, api() + rota, corpo.is_null() ? std::string() : corpo.dump(), r)) {
    erro = "Sem conexão com o servidor Legacy.";
    return false;
  }
  resposta = json::parse(r.corpo, nullptr, false);
  if (r.status < 200 || r.status >= 300) {
    erro = resposta.is_object() && resposta.contains("detail") && resposta["detail"].is_string()
               ? resposta["detail"].get<std::string>()
               : "Erro " + std::to_string(r.status) + " no servidor Legacy.";
    return false;
  }
  return true;
}

std::filesystem::path arquivoSessao(const std::string& app) {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
#else
  const char* base = std::getenv("HOME");
#endif
  std::filesystem::path p = base ? base : ".";
#ifndef _WIN32
  p /= ".config";
#endif
  return p / "Legacy" / ("sessao_" + app + ".bin");
}

void guardar(const std::string& app) {
  std::error_code ec;
  const auto p = arquivoSessao(app);
  std::filesystem::create_directories(p.parent_path(), ec);
  std::string dados = g.token;
#ifdef _WIN32
  DATA_BLOB in{DWORD(dados.size()), (BYTE*)dados.data()}, out{};
  if (CryptProtectData(&in, L"Legacy", nullptr, nullptr, nullptr, 0, &out)) {
    dados.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
  }
#endif
  std::ofstream(p, std::ios::binary) << dados;
#ifndef _WIN32
  std::filesystem::permissions(p, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
#endif
}

bool aplicar(const json& r, const std::string& app) {
  g.token = r.value("token", std::string());
  const json& p = r.contains("perfil") ? r["perfil"] : json();
  g.visitante = !p.is_object();
  g.username = g.visitante ? "Visitante" : p.value("username", std::string());
  g.email = g.visitante ? "" : p.value("email", std::string());
  g.logada = !g.token.empty();
  if (g.logada) {
    guardar(app);
    LOG_INFO("Conta Legacy: %s", g.username.c_str());
  }
  return g.logada;
}

}  // namespace

const Conta& atual() { return g; }

std::string api() {
  const char* e = std::getenv("LEGACY_API");
  return e && *e ? e : "https://conta.legacyengine.com.br";
}

bool trocarCodigo(const std::string& codigo, const std::string& app, std::string& erro) {
  json r;
  if (!chamar("POST", "/sessao/trocar", {{"codigo", codigo}, {"app", app}}, r, erro)) return false;
  return aplicar(r, app);
}

bool login(const std::string& email, const std::string& senha, const std::string& app, std::string& erro) {
  json r;
  if (!chamar("POST", "/sessao/login", {{"email", email}, {"senha", senha}, {"app", app}}, r, erro)) return false;
  if (r.contains("perfil") && r["perfil"].is_object() && r["perfil"].value("precisa_username", false)) {
    erro = "Termine o cadastro (username) no Legacy Launcher.";
    return false;
  }
  return aplicar(r, app);
}

bool carregarSessao(const std::string& app) {
  std::ifstream f(arquivoSessao(app), std::ios::binary);
  if (!f) return false;
  std::string dados((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
#ifdef _WIN32
  DATA_BLOB in{DWORD(dados.size()), (BYTE*)dados.data()}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return false;
  dados.assign((char*)out.pbData, out.cbData);
  LocalFree(out.pbData);
#endif
  g.token = dados;
  json eu;
  std::string erro;
  if (!chamar("GET", "/eu", json(), eu, erro)) {
    g = Conta{};
    return false;
  }
  json r = {{"token", g.token}};
  if (!eu.value("visitante", true)) r["perfil"] = eu;
  return aplicar(r, app);
}

void sair(const std::string& app) {
  json r;
  std::string erro;
  if (g.logada) chamar("POST", "/sessao/sair", json(), r, erro);
  g = Conta{};
  std::error_code ec;
  std::filesystem::remove(arquivoSessao(app), ec);
}

void abrirPaginaConta() {
  const std::string url = "https://legacyengine.com.br/conta";
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  const std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
  [[maybe_unused]] int rc = std::system(cmd.c_str());
#endif
}

}  // namespace conta
