const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('legacy', {
  config: () => ipcRenderer.invoke('config'),
  lerSessao: () => ipcRenderer.invoke('sessao:ler'),
  gravarSessao: (t) => ipcRenderer.invoke('sessao:gravar', t),
  instalado: (id) => ipcRenderer.invoke('produto:instalado', id),
  info: (id) => ipcRenderer.invoke('produto:info', id),
  instalar: (id, url, versao) => ipcRenderer.invoke('produto:instalar', id, url, versao),
  verificar: (id) => ipcRenderer.invoke('produto:verificar', id),
  atualizar: (id) => ipcRenderer.invoke('produto:atualizar', id),
  reiniciarLauncher: () => ipcRenderer.invoke('launcher:reiniciar'),
  aoAtualizarLauncher: (f) => ipcRenderer.on('launcher:atualizacao', (_e, estado, valor) => f(estado, valor)),
  desinstalar: (id) => ipcRenderer.invoke('produto:desinstalar', id),
  jogar: (id, codigo) => ipcRenderer.invoke('produto:jogar', id, codigo),
  ultimaVersao: () => ipcRenderer.invoke('versao:ultima'),
  abrirExterno: (url) => ipcRenderer.invoke('abrir:externo', url),
  abrirPasta: (id) => ipcRenderer.invoke('abrir:pasta', id),
  aoProgresso: (f) => ipcRenderer.on('produto:progresso', (_e, id, p) => f(id, p)),
});
