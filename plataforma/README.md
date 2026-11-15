# Plataforma Legacy

O ecossistema Legacy gira em torno de **uma conta só**: a mesma conta entra no launcher, nos jogos e no Legacy Engine.

- `servidor/` — servidor de contas: cadastro com código por e-mail, login, visitante, username, perfil,
  configurações e biblioteca de jogos. Todos os apps falam com ele; nenhum guarda conta por conta própria.
- `launcher/` — Legacy Launcher (app do Windows): login/visitante/cadastro, loja e biblioteca, download, instalação,
  atualização e "Jogar". (em construção)
- O **Legacy Engine** e a **Times Square** abrem com a sessão do launcher (ou pedem o login Legacy) e usam o perfil
  e as configurações da conta.

Rodar o servidor localmente: `cd servidor && pip install fastapi uvicorn && uvicorn app:app --reload`.
Testes: `python3 -m pytest servidor/teste_app.py`.
