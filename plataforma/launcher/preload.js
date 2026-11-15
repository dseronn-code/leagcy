const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('legacy', {
  config: () => ipcRenderer.invoke('config'),
  lerSessao: () => ipcRenderer.invoke('sessao:ler'),
  gravarSessao: (t) => ipcRenderer.invoke('sessao:gravar', t),
});
