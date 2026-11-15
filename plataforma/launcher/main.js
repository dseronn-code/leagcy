// Legacy Launcher (Electron): janela do app; a interface fica em ui/. A sessão da conta Legacy é guardada pelo
// sistema (safeStorage, criptografada com a chave do usuário do Windows) e compartilhada com o Legacy Engine e os
// jogos: eles leem o mesmo arquivo de sessão (%APPDATA%/Legacy/sessao.bin) — uma conta só para tudo.
const { app, BrowserWindow, ipcMain, safeStorage, shell } = require('electron');
const { spawn } = require('child_process');
const https = require('https');
const fs = require('fs');
const path = require('path');
const atualizador = require('./atualizador');

const API = process.env.LEGACY_API || 'https://account.legacyengine.com.br';
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

const arquivoVersao = (id) => path.join(pastaProduto(id), 'versao-legacy.txt');
const desinstalador = (id) => path.join(pastaProduto(id), 'unins000.exe');

ipcMain.handle('produto:instalado', (_e, id) => (PRODUTOS[id] ? fs.existsSync(exeProduto(id)) : false));
// Instalado? Qual versão? (a versão é a da Release do GitHub de onde veio o instalador)
ipcMain.handle('produto:info', (_e, id) => {
  if (!PRODUTOS[id]) return { instalado: false, versao: null };
  const instalado = fs.existsSync(exeProduto(id));
  let versao = null;
  try { versao = fs.readFileSync(arquivoVersao(id), 'utf8').trim(); } catch {}
  return { instalado, versao };
});

// Última versão publicada (Release mais nova do GitHub): é dela que saem os instaladores do catálogo.
function getJson(url) {
  return new Promise((ok, falha) => {
    https.get(url, { headers: { 'User-Agent': 'Legacy-Launcher', Accept: 'application/vnd.github+json' } }, (r) => {
      let t = '';
      r.on('data', (c) => (t += c));
      r.on('end', () => { try { r.statusCode === 200 ? ok(JSON.parse(t)) : falha(new Error('HTTP ' + r.statusCode)); } catch (e) { falha(e); } });
    }).on('error', falha).setTimeout(15000, function () { this.destroy(new Error('tempo esgotado')); });
  });
}
ipcMain.handle('versao:ultima', async () => {
  try { return (await getJson('https://api.github.com/repos/dseronn-code/leagcy/releases/latest')).tag_name || null; }
  catch { return null; }
});
ipcMain.handle('abrir:externo', (_e, url) => {
  if (/^https:\/\//.test(url)) shell.openExternal(url);
  return true;
});
ipcMain.handle('abrir:pasta', (_e, id) => { if (PRODUTOS[id]) shell.openPath(pastaProduto(id)); return true; });

function baixar(url, destino, progresso, voltas = 0) {
  return new Promise((ok, falha) => {
    https.get(url, { headers: { 'User-Agent': 'Legacy-Launcher' } }, (r) => {
      if ([301, 302, 303, 307, 308].includes(r.statusCode) && voltas < 5) {
        r.resume();
        return baixar(r.headers.location, destino, progresso, voltas + 1).then(ok, falha);
      }
      if (r.statusCode !== 200) { r.resume(); return falha(new Error('Download falhou (erro ' + r.statusCode + ').')); }
      const total = Number(r.headers['content-length'] || 0);
      let feito = 0;
      const f = fs.createWriteStream(destino);
      r.on('data', (c) => { feito += c.length; if (total) progresso(feito / total); });
      r.on('error', falha);
      f.on('error', falha);
      r.pipe(f);
      f.on('finish', () => f.close(() => (total && feito < total ? falha(new Error('Download incompleto. Tente de novo.')) : ok())));
    }).on('error', () => falha(new Error('Sem conexão com a internet para baixar.')));
  });
}

function rodar(exe, args) {
  return new Promise((ok, falha) => {
    const p = spawn(exe, args, { windowsHide: true });
    p.on('exit', (c) => (c === 0 ? ok() : falha(new Error('O instalador terminou com erro ' + c + '.'))));
    p.on('error', falha);
  });
}

// Instalar ou atualizar: baixa o instalador do catálogo e roda em silêncio (cria atalhos e desinstalador). Atualizar
// é o mesmo instalador por cima (o Inno Setup substitui os arquivos e mantém a pasta).
ipcMain.handle('produto:instalar', async (e, id, url, versao) => {
  if (!PRODUTOS[id]) throw new Error('Produto desconhecido.');
  const destino = path.join(app.getPath('temp'), id + '-setup.exe');
  await baixar(url, destino, (p) => e.sender.send('produto:progresso', id, p));
  e.sender.send('produto:progresso', id, -1);  // -1 = instalando
  await rodar(destino, ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER']);
  try { fs.unlinkSync(destino); } catch {}
  if (!fs.existsSync(exeProduto(id))) throw new Error('A instalação não terminou. Tente de novo.');
  if (versao) fs.writeFileSync(arquivoVersao(id), String(versao));
  return true;
});

// Atualizar só o que mudou (manifesto da Release). info = {versao, arquivos, bytes} ou null (sem manifesto).
ipcMain.handle('produto:verificar', async (_e, id) => {
  if (!PRODUTOS[id] || !fs.existsSync(exeProduto(id))) return null;
  return atualizador.verificar(id, pastaProduto(id));
});
ipcMain.handle('produto:atualizar', async (e, id) => {
  if (!PRODUTOS[id] || !fs.existsSync(exeProduto(id))) throw new Error('Não está instalado.');
  return atualizador.atualizar(id, pastaProduto(id), (p) => e.sender.send('produto:progresso', id, p));
});

ipcMain.handle('produto:desinstalar', async (_e, id) => {
  if (!PRODUTOS[id]) throw new Error('Produto desconhecido.');
  if (fs.existsSync(desinstalador(id))) await rodar(desinstalador(id), ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART']);
  else fs.rmSync(pastaProduto(id), { recursive: true, force: true });
  return !fs.existsSync(exeProduto(id));
});

// Jogar: o launcher pede ao servidor um código de uso único e abre o jogo com ele (--legacy-codigo): o jogo entra
// na mesma conta Legacy sem senha e sem o token do launcher na linha de comando.
ipcMain.handle('produto:jogar', (_e, id, codigo) => {
  if (!PRODUTOS[id] || !fs.existsSync(exeProduto(id))) throw new Error('Não está instalado.');
  const args = [...PRODUTOS[id].args, ...(codigo ? ['--legacy-codigo', codigo] : [])];
  const p = spawn(exeProduto(id), args, { cwd: pastaProduto(id), detached: true, stdio: 'ignore',
                                          env: { ...process.env, LEGACY_API: API } });
  p.unref();
  return true;
});

if (!app.requestSingleInstanceLock()) app.quit();
app.on('second-instance', () => {
  const w = BrowserWindow.getAllWindows()[0];
  if (w) { if (w.isMinimized()) w.restore(); w.focus(); }
});
app.whenReady().then(() => {
  criarJanela();
  autoAtualizar();
});

// O launcher se atualiza sozinho (electron-updater lendo latest.yml da Release mais nova no GitHub): baixa em
// segundo plano, só a parte que mudou (blockmap), e instala ao fechar — ou na hora, se a pessoa clicar em
// "Reiniciar". Só no launcher instalado (não em "npm start").
function autoAtualizar() {
  if (!app.isPackaged) return;
  let autoUpdater;
  try { ({ autoUpdater } = require('electron-updater')); } catch { return; }
  const avisar = (canal, ...a) => BrowserWindow.getAllWindows().forEach((w) => w.webContents.send(canal, ...a));
  autoUpdater.autoDownload = true;
  autoUpdater.autoInstallOnAppQuit = true;
  autoUpdater.on('update-available', (i) => avisar('launcher:atualizacao', 'baixando', i.version));
  autoUpdater.on('download-progress', (p) => avisar('launcher:atualizacao', 'progresso', p.percent));
  autoUpdater.on('update-downloaded', (i) => avisar('launcher:atualizacao', 'pronta', i.version));
  autoUpdater.on('error', () => avisar('launcher:atualizacao', 'erro'));
  const checar = () => autoUpdater.checkForUpdates().catch(() => {});
  checar();
  setInterval(checar, 4 * 3600 * 1000);  // launcher aberto o dia todo: confere de 4 em 4 horas
  ipcMain.handle('launcher:reiniciar', () => autoUpdater.quitAndInstall(true, true));
}
app.on('window-all-closed', () => app.quit());
