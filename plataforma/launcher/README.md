# Legacy Launcher

App do Windows (Electron) da plataforma Legacy: entrar / criar conta (código por e-mail) / visitante, Loja (Times
Square), Biblioteca, aba do Legacy Engine (marketing) e Conta. Fala só com o servidor de contas
(`../servidor`); a sessão fica criptografada em `%APPDATA%/Legacy/sessao.bin`, o mesmo arquivo que o Legacy Engine e os
jogos leem — uma conta só para tudo.

- Rodar: `npm install` e `npm start` (servidor local: `LEGACY_API=http://localhost:8000 npm start`).
- Instalador do Windows: `npm run dist` (gera `dist/Legacy Setup x.y.z.exe`, com atalho e desinstalador).
- Testar a interface no navegador: abrir `ui/index.html?api=http://localhost:8000`.

As imagens em `ui/img` são provisórias (renderizador por software); serão trocadas por capturas na RTX.
