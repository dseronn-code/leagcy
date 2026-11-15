// Legacy Launcher (Electron): janela do app; a interface fica em ui/. A sessão da conta Legacy é guardada pelo
// sistema (safeStorage, criptografada com a chave do usuário do Windows) e compartilhada com o Legacy Engine e os
// jogos: eles leem o mesmo arquivo de sessão (%APPDATA%/Legacy/sessao.bin) — uma conta só para tudo.
const { app, BrowserWindow, ipcMain, safeStorage, shell } = require('electron');
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

app.whenReady().then(criarJanela);
app.on('window-all-closed', () => app.quit());
