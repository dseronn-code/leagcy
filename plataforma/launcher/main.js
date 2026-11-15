// Legacy Launcher (Electron): janela do app; a interface fica em ui/. A sessão da conta Legacy é guardada pelo
// sistema (safeStorage, criptografada com a chave do usuário do Windows) e compartilhada com o Legacy Engine e os
// jogos: eles leem o mesmo arquivo de sessão (%APPDATA%/Legacy/sessao.bin) — uma conta só para tudo.
const { app, BrowserWindow, ipcMain, safeStorage, shell } = require('electron');
const { spawn } = require('child_process');
const https = require('https');
const fs = require('fs');
const path = require('path');

const API = process.env.LEGACY_API || 'https://conta.legacyengine.com.br';
const pastaLegacy = () => path.join(app.getPath('appData'), 'Legacy');
const arquivoSessao = () => path.join(pastaLegacy(), 'sessao.bin');

function criarJanela() {
  const win = new BrowserWindow({
    width: 1360, height: 840, minWidth: 1100, minHeight: 680, backgroundColor: '#07080b', title: 'Legacy',
    autoHideMenuBar: true,
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, nodeIntegration: false },
  });
  win.loadFile(path.join(__dirname, 'ui', 'index.html'));
  win.webContents.setWindowOpenHandler(({ url }) => { shell.openExternal(url); return { action: 'deny' }; });
}

ipcMain.handle('config', () => ({ api: API, versao: app.getVersion() }));
ipcMain.handle('sessao:ler', () => {
  try {
    const dados = fs.readFileSync(arquivoSessao());
    return safeStorage.isEncryptionAvailable() ? safeStorage.decryptString(dados) : dados.toString();
  } catch { return null; }
});
ipcMain.handle('sessao:gravar', (_e, token) => {
  fs.mkdirSync(pastaLegacy(), { recursive: true });
  if (!token) { try { fs.unlinkSync(arquivoSessao()); } catch {} return true; }
  fs.writeFileSync(arquivoSessao(), safeStorage.isEncryptionAvailable() ? safeStorage.encryptString(token) : token);
  return true;
});

// ---- jogos instalados: os instaladores (Inno Setup, sem administrador) vão para %LOCALAPPDATA%\Programs\Legacy\<nome>
const PRODUTOS = {
  'times-square': { pasta: 'Times Square', args: ['--scene', 'cenas\\times_square.json', '--fullscreen'] },
  'legacy-engine': { pasta: 'Legacy Engine', args: ['--editor', '--scene', 'cenas\\vale\\vale.json'] },
};
const pastaProduto = (id) => path.join(process.env.LOCALAPPDATA || app.getPath('appData'), 'Programs', 'Legacy',
                                        PRODUTOS[id].pasta);
const exeProduto = (id) => path.join(pastaProduto(id), 'legacy.exe');

ipcMain.handle('produto:instalado', (_e, id) => (PRODUTOS[id] ? fs.existsSync(exeProduto(id)) : false));

function baixar(url, destino, progresso, voltas = 0) {
  return new Promise((ok, falha) => {
    https.get(url, (r) => {
      if ([301, 302, 303, 307, 308].includes(r.statusCode) && voltas < 5)
        return baixar(r.headers.location, destino, progresso, voltas + 1).then(ok, falha);
      if (r.statusCode !== 200) return falha(new Error('Download falhou (' + r.statusCode + ')'));
      const total = Number(r.headers['content-length'] || 0);
      let feito = 0;
      const f = fs.createWriteStream(destino);
      r.on('data', (c) => { feito += c.length; if (total) progresso(feito / total); });
      r.pipe(f);
      f.on('finish', () => f.close(ok));
    }).on('error', falha);
  });
}

// Instalar: baixa o instalador do catálogo e roda em silêncio (o instalador cria atalhos e desinstalador).
ipcMain.handle('produto:instalar', async (e, id, url) => {
  const destino = path.join(app.getPath('temp'), id + '-setup.exe');
  await baixar(url, destino, (p) => e.sender.send('produto:progresso', id, p));
  await new Promise((ok, falha) => {
    const p = spawn(destino, ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'], { windowsHide: true });
    p.on('exit', (c) => (c === 0 ? ok() : falha(new Error('O instalador terminou com erro ' + c))));
    p.on('error', falha);
  });
  return fs.existsSync(exeProduto(id));
});

// Jogar: o launcher pede ao servidor um código de uso único e abre o jogo com ele (--legacy-codigo): o jogo entra
// na mesma conta Legacy sem senha e sem o token do launcher na linha de comando.
ipcMain.handle('produto:jogar', (_e, id, codigo) => {
  const args = [...PRODUTOS[id].args, ...(codigo ? ['--legacy-codigo', codigo] : [])];
  const p = spawn(exeProduto(id), args, { cwd: pastaProduto(id), detached: true, stdio: 'ignore',
                                          env: { ...process.env, LEGACY_API: API } });
  p.unref();
  return true;
});

app.whenReady().then(criarJanela);
app.on('window-all-closed', () => app.quit());
