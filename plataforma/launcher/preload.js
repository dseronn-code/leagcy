const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('legacy', {
  config: () => ipcRenderer.invoke('config'),
  lerSessao: () => ipcRenderer.invoke('sessao:ler'),
  gravarSessao: (t) => ipcRenderer.invoke('sessao:gravar', t),
  instalado: (id) => ipcRenderer.invoke('produto:instalado', id),
  instalar: (id, url) => ipcRenderer.invoke('produto:instalar', id, url),
  jogar: (id, codigo) => ipcRenderer.invoke('produto:jogar', id, codigo),
  aoProgresso: (f) => ipcRenderer.on('produto:progresso', (_e, id, p) => f(id, p)),
});
