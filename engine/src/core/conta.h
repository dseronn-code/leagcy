#pragma once
// Conta Legacy dentro do motor e dos jogos: o Legacy Engine e os jogos não têm conta própria — usam a conta Legacy,
// do mesmo servidor do Legacy Launcher (como a Unreal e o Fortnite usam a conta Epic).
//
// Como a sessão chega aqui:
//   1. Aberto pelo Legacy Launcher: ele passa --legacy-codigo <código de uso único>, trocado por uma sessão própria
//      (POST /sessao/trocar), sem senha nem token na linha de comando.
//   2. Aberto direto: usa a sessão guardada da última vez (Windows: protegida pelo DPAPI, só o usuário do Windows lê);
//      senão, o editor pede e-mail e senha da conta Legacy.
// Servidor: LEGACY_API (padrão https://account.legacyengine.com.br).
#include <string>

namespace conta {

struct Conta {
  bool logada = false;     // sessão válida no servidor (conta ou visitante)
  bool visitante = true;   // sem conta (só jogos que deixam)
  std::string username, email, token;
};

const Conta& atual();
std::string api();

// Troca o código do launcher por uma sessão deste app ("legacy-engine", "times-square"...).
bool trocarCodigo(const std::string& codigo, const std::string& app, std::string& erro);
// Login direto com a conta Legacy (o editor, quando não foi aberto pelo launcher).
bool login(const std::string& email, const std::string& senha, const std::string& app, std::string& erro);
// Sessão guardada da última vez para este app; confere no servidor (GET /eu).
bool carregarSessao(const std::string& app);
void sair(const std::string& app);
// Abre no navegador a página da conta Legacy (configurações da conta ficam lá, não no motor).
void abrirPaginaConta();

}  // namespace conta
