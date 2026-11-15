// Legacy Launcher: telas de entrada (visitante, login, cadastro com código, username) e o app (loja, biblioteca,
// Legacy Engine, conta). Tudo de conta passa pelo servidor Legacy; o token fica com o processo principal (Electron,
// criptografado) e é o mesmo que o Legacy Engine e os jogos usam.
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

// Fora do Electron (testes no navegador): API em ?api=..., sessão no localStorage.
const ponte = window.legacy || {
  config: async () => ({ api: new URLSearchParams(location.search).get('api') || 'http://localhost:8000', versao: 'dev' }),
  lerSessao: async () => localStorage.getItem('legacy_sessao'),
  gravarSessao: async (t) => (t ? localStorage.setItem('legacy_sessao', t) : localStorage.removeItem('legacy_sessao'), true),
};

let API = '';
let token = null;
let perfil = null;          // null = visitante
let pendente = null;        // {email, senha} durante o cadastro

async function chamar(metodo, rota, corpo, segundos = 30) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), segundos * 1000);
  let r;
  try {
    r = await fetch(API + rota, {
      method: metodo, signal: ctl.signal,
      headers: { 'Content-Type': 'application/json', ...(token ? { Authorization: 'Bearer ' + token } : {}) },
      body: corpo ? JSON.stringify(corpo) : undefined,
    });
  } catch {
    const e = new Error('Sem conexão com o servidor Legacy. Verifique a internet.');
    e.rede = true;
    throw e;
  } finally {
    clearTimeout(t);
  }
  const dados = await r.json().catch(() => ({}));
  if (!r.ok) {
    const e = new Error(typeof dados.detail === 'string' ? dados.detail : 'O servidor Legacy recusou o pedido.');
    e.status = r.status;
    throw e;
  }
  return dados;
}

// O servidor (plano grátis) dorme sem uso e leva até ~1 min para acordar: espera por ele mostrando o progresso.
async function acordarServidor() {
  const inicio = Date.now();
  for (let tentativa = 0; Date.now() - inicio < 90000; ++tentativa) {
    try {
      await chamar('GET', '/saude', null, 20);
      return true;
    } catch (e) {
      if (!e.rede) return true;  // respondeu (mesmo com erro): está acordado
      const s = Math.round((Date.now() - inicio) / 1000);
      $('#texto-conectando').textContent = s < 10 ? '' : 'Quase lá…';
      await new Promise((ok) => setTimeout(ok, 3000));
    }
  }
  return false;
}

function aviso(texto) {
  const t = $('#aviso');
  t.textContent = texto;
  t.hidden = false;
  clearTimeout(aviso.t);
  aviso.t = setTimeout(() => (t.hidden = true), 3500);
}

// ---------------------------------------------------------------- entrada
function passo(id) {
  $$('.passo').forEach((p) => (p.hidden = p.id !== id));
  $$('.erro').forEach((e) => (e.textContent = ''));
  const primeiro = $('#' + id + ' input');
  if (primeiro) primeiro.focus();
}
$$('[data-ir]').forEach((b) => b.addEventListener('click', () => passo(b.dataset.ir)));

function formulario(id, acao) {
  const f = $('#' + id);
  f.addEventListener('submit', async (e) => {
    e.preventDefault();
    const botao = $('button[type=submit]', f);
    $('.erro', f).textContent = '';
    botao.disabled = true;
    try {
      await acao(Object.fromEntries(new FormData(f)));
    } catch (err) {
      $('.erro', f).textContent = err.message;
    } finally {
      botao.disabled = false;
    }
  });
}

async function entrou(resposta) {
  token = resposta.token;
  await ponte.gravarSessao(token);
  perfil = resposta.perfil;
  if (perfil && perfil.precisa_username) return passo('passo-username');
  abrirApp();
}

formulario('passo-login', async (d) => entrou(await chamar('POST', '/sessao/login', { ...d, app: 'launcher' })));
formulario('passo-cadastro', async (d) => {
  await chamar('POST', '/conta/cadastro', d);
  pendente = d;
  $('#email-enviado').textContent = d.email;
  passo('passo-codigo');
});
formulario('passo-codigo', async (d) =>
  entrou(await chamar('POST', '/conta/verificar', { email: pendente.email, codigo: d.codigo, app: 'launcher' })));
let emailRedefinir = '';
formulario('passo-esqueci', async (d) => {
  await chamar('POST', '/conta/esqueci', d);
  emailRedefinir = d.email;
  $('#email-redefinir').textContent = d.email;
  passo('passo-redefinir');
});
formulario('passo-redefinir', async (d) =>
  entrou(await chamar('POST', '/conta/redefinir', { email: emailRedefinir, codigo: d.codigo, senha: d.senha, app: 'launcher' })));
formulario('passo-username', async (d) => {
  perfil = await chamar('POST', '/conta/username', d);
  abrirApp();
});
$('#btn-reenviar').addEventListener('click', async () => {
  try {
    await chamar('POST', '/conta/reenviar', pendente);
    aviso('Código reenviado.');
  } catch (err) {
    $('#passo-codigo .erro').textContent = err.message;
  }
});
$('#btn-visitante').addEventListener('click', async () => {
  try {
    await entrou(await chamar('POST', '/sessao/visitante'));
  } catch (err) {
    aviso(err.message);
  }
});

// ---------------------------------------------------------------- app
function abrirApp() {
  $('#entrada').hidden = true;
  $('#app').hidden = false;
  const nome = perfil ? perfil.username : 'Visitante';
  $('#nome-usuario').textContent = nome;
  $('#avatar').textContent = nome[0].toUpperCase();
  pagina(location.hash.slice(1) || 'loja');
  autoAtualizarProdutos();
}

function pagina(nome) {
  if (!['loja', 'biblioteca', 'engine', 'conta'].includes(nome)) nome = 'loja';
  $$('.pagina').forEach((p) => (p.hidden = p.id !== 'pg-' + nome));
  $$('.lateral .item').forEach((a) => a.classList.toggle('ativo', a.dataset.pagina === nome));
  if (nome === 'biblioteca') mostrarBiblioteca();
  if (nome === 'conta') mostrarConta();
  $('.conteudo').scrollTop = 0;
}
window.addEventListener('hashchange', () => pagina(location.hash.slice(1)));

const IMAGENS = { 'times-square': 'img/ts_hero.jpg', 'legacy-engine': 'img/eng_hero.jpg' };
let catalogo = null;
let ultimaVersao = null;  // tag da Release mais nova (ex.: v0.1.282): a versão dos instaladores do catálogo
const ocupado = new Set();  // produtos baixando/instalando
const atualizando = new Set();  // produtos atualizando por arquivos (o progresso mostra "Atualizando")

async function lerCatalogo() {
  if (!catalogo) catalogo = await chamar('GET', '/catalogo');
  return catalogo;
}

// Biblioteca: com conta, os produtos da conta; como visitante, os gratuitos (que não pedem conta).
async function mostrarBiblioteca() {
  const lista = $('#lista-biblioteca');
  let itens;
  try {
    const cat = await lerCatalogo();
    if (perfil) {
      const meus = new Set((await chamar('GET', '/eu/biblioteca')).map((m) => m.produto));
      itens = cat.produtos.filter((p) => meus.has(p.id));
    } else {
      itens = cat.produtos.filter((p) => !p.precisa_conta);
    }
  } catch (e) {
    lista.innerHTML = '';
    const v = document.createElement('p');
    v.className = 'vazio';
    v.textContent = e.message;
    lista.appendChild(v);
    return;
  }
  lista.innerHTML = itens.length ? '' : '<p class="vazio">Sua biblioteca está vazia. Conheça a Times Square na Loja.</p>';
  for (const p of itens) lista.appendChild(await cartaoProduto(p));
}

async function cartaoProduto(p) {
  const el = document.createElement('div');
  el.className = 'jogo';
  el.dataset.id = p.id;
  el.innerHTML = `<img src="${IMAGENS[p.id] || 'img/ts_hero.jpg'}" alt=""><div class="jogo-corpo"><div><b></b>
    <small class="estado"></small><div class="barra" hidden><span></span></div></div>
    <div class="jogo-acoes"><button class="botao primario principal"></button>
    <button class="botao mais" title="Mais opções" hidden>⋯</button>
    <div class="menu" hidden><button data-acao="pasta">Abrir pasta</button><button data-acao="desinstalar" class="perigo">Desinstalar</button></div>
    </div></div>`;
  $('b', el).textContent = p.nome;
  await atualizarCartao(el, p);
  $('.principal', el).addEventListener('click', () => acaoPrincipal(el, p));
  $('.mais', el).addEventListener('click', (e) => { e.stopPropagation(); $('.menu', el).hidden = !$('.menu', el).hidden; });
  document.addEventListener('click', () => ($('.menu', el).hidden = true));
  $('[data-acao=pasta]', el).addEventListener('click', () => window.legacy && window.legacy.abrirPasta(p.id));
  $('[data-acao=desinstalar]', el).addEventListener('click', async () => {
    if (!confirm(`Desinstalar ${p.nome}?`)) return;
    ocupado.add(p.id);
    await atualizarCartao(el, p, 'Desinstalando…');
    try {
      await window.legacy.desinstalar(p.id);
      aviso(p.nome + ' foi desinstalado.');
    } catch (err) {
      aviso(err.message);
    }
    ocupado.delete(p.id);
    await atualizarCartao(el, p);
  });
  return el;
}

function tamanho(bytes) {
  return bytes >= 1e9 ? (bytes / 1e9).toFixed(1) + ' GB' : bytes >= 1e6 ? Math.max(1, Math.round(bytes / 1e6)) + ' MB'
    : Math.max(1, Math.round(bytes / 1e3)) + ' KB';
}

function versaoMaior(a, b) {  // "v0.1.282" > "v0.1.278"?
  const n = (v) => String(v || '').replace(/^v/, '').split('.').map((x) => parseInt(x, 10) || 0);
  const x = n(a), y = n(b);
  for (let i = 0; i < Math.max(x.length, y.length); ++i) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) > (y[i] || 0);
  return false;
}

async function atualizarCartao(el, p, ocupadoTexto) {
  const botao = $('.principal', el), estado = $('.estado', el), mais = $('.mais', el);
  if (!window.legacy) {
    botao.textContent = 'Instalar';
    estado.textContent = 'Disponível no Legacy Launcher para Windows';
    return;
  }
  const info = await window.legacy.info(p.id);
  mais.hidden = !info.instalado;
  botao.disabled = ocupado.has(p.id);
  if (ocupadoTexto) { botao.textContent = ocupadoTexto; return; }
  if (!info.instalado) {
    botao.textContent = 'Instalar';
    estado.textContent = p.tamanho ? `Download de ${p.tamanho}` : '';
  } else if (ultimaVersao && (!info.versao || versaoMaior(ultimaVersao, info.versao))) {
    botao.textContent = 'Atualizar';
    estado.textContent = `${info.versao ? 'Versão ' + info.versao + ' instalada · ' : ''}nova: ${ultimaVersao}`;
    if (window.legacy.verificar) {  // quanto falta baixar (só os arquivos que mudaram)
      window.legacy.verificar(p.id).then((v) => {
        if (!v || ocupado.has(p.id)) return;
        if (v.arquivos === 0) { botao.textContent = 'Jogar'; estado.textContent = 'Versão ' + v.versao; return; }
        botao.textContent = `Atualizar (${tamanho(v.bytes)})`;
      }).catch(() => {});
    }
  } else {
    botao.textContent = 'Jogar';
    estado.textContent = info.versao ? `Versão ${info.versao}` : 'Instalado';
  }
}

// Atualiza baixando só os arquivos que mudaram. Sem manifesto na Release (versões antigas), cai no instalador.
async function atualizarProduto(p, el, silencioso = false) {
  if (ocupado.has(p.id)) return;
  ocupado.add(p.id);
  atualizando.add(p.id);
  if (el) { $('.barra', el).hidden = false; await atualizarCartao(el, p, 'Atualizando…'); }
  try {
    const r = await window.legacy.atualizar(p.id);
    if (r.arquivos || !silencioso) aviso(`${p.nome} atualizado para ${r.versao}` + (r.arquivos ? ` (${r.arquivos} arquivo${r.arquivos > 1 ? 's' : ''}).` : '.'));
  } catch (err) {
    if (/não tem atualização pelo launcher/.test(err.message) && p.download && !silencioso) {
      atualizando.delete(p.id);
      try {
        await window.legacy.instalar(p.id, p.download, ultimaVersao);
        aviso(p.nome + ' atualizado.');
      } catch (e2) { aviso(e2.message); }
    } else if (!silencioso || /Feche o jogo/.test(err.message)) {
      aviso(err.message);
    }
  }
  ocupado.delete(p.id);
  atualizando.delete(p.id);
  const cartao = el || $(`.jogo[data-id="${p.id}"]`);
  if (cartao) { $('.barra', cartao).hidden = true; await atualizarCartao(cartao, p); }
}

// Ao abrir o launcher: o que está instalado e tem versão nova se atualiza sozinho (como na Epic).
async function autoAtualizarProdutos() {
  if (!window.legacy || !window.legacy.atualizar || !ultimaVersao) return;
  let cat;
  try { cat = await lerCatalogo(); } catch { return; }
  for (const p of cat.produtos) {
    const info = await window.legacy.info(p.id);
    if (info.instalado && (!info.versao || versaoMaior(ultimaVersao, info.versao))) await atualizarProduto(p, null, true);
  }
}

async function acaoPrincipal(el, p) {
  if (!window.legacy) return aviso('Instale o Legacy Launcher no Windows para baixar e jogar.');
  const info = await window.legacy.info(p.id);
  const atualizar = info.instalado && ultimaVersao && (!info.versao || versaoMaior(ultimaVersao, info.versao));
  if (info.instalado && !atualizar) {
    try {
      // o jogo entra na mesma conta Legacy (código de uso único); sem servidor, abre sem conta
      const codigo = token ? (await chamar('POST', '/sessao/codigo-troca').catch(() => ({}))).codigo : null;
      await window.legacy.jogar(p.id, codigo);
      aviso('Abrindo ' + p.nome + '…');
    } catch (err) {
      aviso(err.message);
    }
    return;
  }
  if (atualizar && window.legacy.atualizar) return atualizarProduto(p, el);
  if (!p.download) return aviso('O instalador fica pronto em breve.');
  ocupado.add(p.id);
  $('.barra', el).hidden = false;
  await atualizarCartao(el, p, 'Baixando…');
  try {
    await window.legacy.instalar(p.id, p.download, ultimaVersao);
    aviso(p.nome + (atualizar ? ' atualizado.' : ' instalado. Bom jogo!'));
  } catch (err) {
    aviso(err.message);
  }
  ocupado.delete(p.id);
  $('.barra', el).hidden = true;
  await atualizarCartao(el, p);
}

$$('[data-produto]').forEach((b) =>
  b.addEventListener('click', async () => {
    const id = b.dataset.produto;
    if (!perfil) {
      if (id === 'legacy-engine') return aviso('O Legacy Engine pede uma conta Legacy. Crie a sua em Conta.');
      location.hash = 'biblioteca';  // gratuito: o visitante instala direto
      return;
    }
    try {
      await chamar('POST', '/eu/biblioteca/' + id);
      aviso('Adicionado à sua biblioteca.');
      location.hash = 'biblioteca';
    } catch (err) {
      aviso(err.message);
    }
  }));

function mostrarConta() {
  const p = $('#painel-conta');
  if (!perfil) {
    p.innerHTML = `<p>Você está como visitante. Crie uma conta Legacy para salvar sua biblioteca, usar o Legacy Engine e
      ter o mesmo nome em todos os jogos.</p><div class="botoes"><button class="botao primario" id="c-criar">Criar conta</button>
      <button class="botao" id="c-entrar">Entrar</button></div>`;
    $('#c-criar').onclick = () => sair('passo-cadastro');
    $('#c-entrar').onclick = () => sair('passo-login');
    return;
  }
  p.innerHTML = `<div class="linha"><span>Username</span><b></b></div><div class="linha"><span>E-mail</span><b></b></div>
    <form id="form-senha" class="form-senha"><h3>Trocar senha</h3>
      <label>Senha atual<input type="password" name="atual" autocomplete="current-password" required></label>
      <label>Senha nova <small>(mínimo 8 caracteres)</small><input type="password" name="nova" minlength="8" autocomplete="new-password" required></label>
      <p class="erro" aria-live="polite"></p><button class="botao" type="submit">Salvar senha</button></form>
    <div class="botoes"><button class="botao" id="c-sair">Sair</button>
    <button class="botao perigo" id="c-apagar">Apagar conta</button></div>
    <p class="dica">Legacy Launcher ${versaoLauncher} · <a href="#" id="c-termos">Termos</a> · <a href="#" id="c-privacidade">Privacidade</a></p>`;
  formulario('form-senha', async (d) => {
    await chamar('POST', '/eu/senha', d);
    $('#form-senha').reset();
    aviso('Senha trocada. As outras sessões da conta foram encerradas.');
  });
  $('#c-termos').onclick = (e) => { e.preventDefault(); mostrarTexto('termos'); };
  $('#c-privacidade').onclick = (e) => { e.preventDefault(); mostrarTexto('privacidade'); };
  const b = $$('.linha b', p);
  b[0].textContent = perfil.username;
  b[1].textContent = perfil.email;
  $('#c-sair').onclick = () => sair('passo-inicio');
  $('#c-apagar').onclick = async () => {
    if (!confirm('Apagar a conta Legacy e todos os dados? Isso não pode ser desfeito.')) return;
    try {
      await chamar('DELETE', '/conta');
    } catch (err) {
      return aviso(err.message);
    }
    token = null;
    sair('passo-inicio');
  };
}

async function sair(destino) {
  if (token) await chamar('POST', '/sessao/sair').catch(() => {});
  token = null;
  perfil = null;
  await ponte.gravarSessao(null);
  history.replaceState(null, '', location.pathname + location.search);  // quem entrar depois começa na Loja
  $('#app').hidden = true;
  $('#entrada').hidden = false;
  passo(destino);
}

if (window.legacy) window.legacy.aoProgresso((id, p) => {
  const el = $(`.jogo[data-id="${id}"]`);
  if (!el) return;
  if (p < 0) {
    $('.principal', el).textContent = 'Instalando…';
    $('.barra span', el).style.width = '100%';
    return;
  }
  $('.principal', el).textContent = (atualizando.has(id) ? 'Atualizando ' : 'Baixando ')
    + Math.round(p * 100) + '%';
  $('.barra span', el).style.width = (p * 100).toFixed(1) + '%';
});

// Termos e privacidade: abrem no navegador (o processo principal manda links para fora do app)
function mostrarTexto(qual) {
  $('#modal-conteudo').innerHTML = (window.TEXTOS_LEGACY || {})[qual] || '<p>Texto indisponível.</p>';
  $('#modal-texto').hidden = false;
}
$('#fechar-modal').addEventListener('click', () => ($('#modal-texto').hidden = true));
$('#modal-texto').addEventListener('click', (e) => { if (e.target.id === 'modal-texto') $('#modal-texto').hidden = true; });
$('#link-termos').addEventListener('click', (e) => { e.preventDefault(); mostrarTexto('termos'); });
$('#link-privacidade').addEventListener('click', (e) => { e.preventDefault(); mostrarTexto('privacidade'); });

// comparador antes/depois
const comp = $('#comparador');
$('input', comp).addEventListener('input', (e) => {
  comp.style.setProperty('--pos', e.target.value + '%');
  $('.lado-b', comp).style.clipPath = `inset(0 0 0 ${e.target.value}%)`;
});
$('#btn-reveal').addEventListener('click', () => aviso('O vídeo de apresentação está sendo produzido.'));

// ---------------------------------------------------------------- início
let versaoLauncher = 'dev';

async function mostrarOffline() {
  passo('passo-offline');
  const l = $('#lista-offline');
  l.innerHTML = '';
  if (!window.legacy) return;
  for (const [id, nome] of [['times-square', 'Times Square']]) {
    if (!(await window.legacy.instalado(id))) continue;
    const b = document.createElement('button');
    b.className = 'botao largo';
    b.textContent = 'Jogar ' + nome + ' offline';
    b.onclick = () => window.legacy.jogar(id, null).then(() => aviso('Abrindo ' + nome + '…'), (e) => aviso(e.message));
    l.appendChild(b);
  }
}
$('#btn-tentar').addEventListener('click', () => iniciar());

let launcherSeAtualiza = false;  // launcher com electron-updater: o banner só aparece quando a versão nova já baixou
async function verAtualizacoes() {
  if (!window.legacy) return;
  ultimaVersao = await window.legacy.ultimaVersao();
  if (!$('#pg-biblioteca').hidden) mostrarBiblioteca();  // os cartões já na tela passam a mostrar "Atualizar"
  if (!$('#app').hidden) autoAtualizarProdutos();
  if (!launcherSeAtualiza && ultimaVersao && versaoMaior(ultimaVersao, versaoLauncher)) {
    $('#versao-nova').textContent = ultimaVersao;
    $('#banner-atualizacao').hidden = false;
  }
}
if (window.legacy && window.legacy.aoAtualizarLauncher) {
  launcherSeAtualiza = true;
  window.legacy.aoAtualizarLauncher((estado, valor) => {
    const banner = $('#banner-atualizacao'), texto = $('span', banner), botao = $('#btn-atualizar-launcher');
    if (estado === 'baixando') { banner.hidden = false; botao.hidden = true; texto.textContent = `Baixando a versão ${valor} do Legacy Launcher…`; }
    if (estado === 'progresso') texto.textContent = `Baixando a versão nova do Legacy Launcher… ${Math.round(valor)}%`;
    if (estado === 'pronta') {
      banner.hidden = false;
      botao.hidden = false;
      botao.textContent = 'Reiniciar agora';
      botao.dataset.reiniciar = '1';
      texto.textContent = `A versão ${valor} do Legacy Launcher está pronta. Ela instala sozinha quando você fechar o launcher.`;
    }
    if (estado === 'erro') {  // falhou: volta ao aviso com download manual
      launcherSeAtualiza = false;
      botao.hidden = false;
      if (ultimaVersao && versaoMaior(ultimaVersao, versaoLauncher)) {
        texto.innerHTML = 'Há uma versão nova do Legacy Launcher (<b></b>).';
        $('b', texto).textContent = ultimaVersao;
        banner.hidden = false;
      } else banner.hidden = true;
    }
  });
}
$('#btn-atualizar-launcher').addEventListener('click', async () => {
  if ($('#btn-atualizar-launcher').dataset.reiniciar) return window.legacy.reiniciarLauncher();
  const url = (catalogo && catalogo.launcher) || 'https://github.com/dseronn-code/leagcy/releases/latest/download/Legacy-Setup.exe';
  await window.legacy.abrirExterno(url);
  aviso('O download do instalador novo começou no navegador. Feche o launcher e rode o instalador.');
});

async function iniciar() {
  passo('passo-conectando');
  if (!(await acordarServidor())) return mostrarOffline();
  token = await ponte.lerSessao();
  if (token) {
    try {
      const eu = await chamar('GET', '/eu');
      perfil = eu.visitante ? null : eu;
      if (perfil && perfil.precisa_username) return passo('passo-username');
      return abrirApp();
    } catch (e) {
      if (e.rede) return mostrarOffline();
      token = null;  // sessão expirada ou apagada
      await ponte.gravarSessao(null);
    }
  }
  passo('passo-inicio');
}

(async () => {
  const cfg = await ponte.config();
  API = cfg.api;
  versaoLauncher = cfg.versao;
  verAtualizacoes();  // em paralelo: a Release mais nova (launcher e jogos)
  iniciar();
})();
