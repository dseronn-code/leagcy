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

async function chamar(metodo, rota, corpo) {
  const r = await fetch(API + rota, {
    method: metodo,
    headers: { 'Content-Type': 'application/json', ...(token ? { Authorization: 'Bearer ' + token } : {}) },
    body: corpo ? JSON.stringify(corpo) : undefined,
  });
  const dados = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(dados.detail || 'Não foi possível falar com o servidor Legacy.');
  return dados;
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

async function mostrarBiblioteca() {
  const lista = $('#lista-biblioteca');
  if (!perfil) {
    lista.innerHTML = '<p class="vazio">Entre com uma conta Legacy para ter sua biblioteca em qualquer computador.</p>';
    return;
  }
  const [cat, meus] = await Promise.all([chamar('GET', '/catalogo'), chamar('GET', '/eu/biblioteca')]);
  const ids = new Set(meus.map((m) => m.produto));
  const itens = cat.produtos.filter((p) => ids.has(p.id));
  lista.innerHTML = itens.length ? '' : '<p class="vazio">Sua biblioteca está vazia. Conheça a Times Square na Loja.</p>';
  for (const p of itens) {
    const el = document.createElement('div');
    el.className = 'jogo';
    el.innerHTML = `<img src="${IMAGENS[p.id]}" alt=""><div><b></b><button class="botao primario">Instalar</button></div>`;
    $('b', el).textContent = p.nome;
    $('button', el).addEventListener('click', () => aviso(p.download ? 'Baixando...' : 'O instalador fica pronto em breve.'));
    lista.appendChild(el);
  }
}

$$('[data-produto]').forEach((b) =>
  b.addEventListener('click', async () => {
    const id = b.dataset.produto;
    if (!perfil) {
      if (id === 'legacy-engine') return aviso('O Legacy Engine pede uma conta Legacy. Crie a sua em Conta.');
      return aviso('A Times Square chega em breve para download.');
    }
    await chamar('POST', '/eu/biblioteca/' + id);
    aviso('Adicionado à sua biblioteca.');
    location.hash = 'biblioteca';
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
    <div class="botoes"><button class="botao" id="c-sair">Sair</button>
    <button class="botao perigo" id="c-apagar">Apagar conta</button></div>`;
  const b = $$('.linha b', p);
  b[0].textContent = perfil.username;
  b[1].textContent = perfil.email;
  $('#c-sair').onclick = () => sair('passo-inicio');
  $('#c-apagar').onclick = async () => {
    if (!confirm('Apagar a conta Legacy e todos os dados? Isso não pode ser desfeito.')) return;
    await chamar('DELETE', '/conta');
    token = null;
    sair('passo-inicio');
  };
}

async function sair(destino) {
  if (token) await chamar('POST', '/sessao/sair').catch(() => {});
  token = null;
  perfil = null;
  await ponte.gravarSessao(null);
  $('#app').hidden = true;
  $('#entrada').hidden = false;
  passo(destino);
}

// comparador antes/depois
const comp = $('#comparador');
$('input', comp).addEventListener('input', (e) => {
  comp.style.setProperty('--pos', e.target.value + '%');
  $('.lado-b', comp).style.clipPath = `inset(0 0 0 ${e.target.value}%)`;
});
$('#btn-reveal').addEventListener('click', () => aviso('O vídeo de apresentação está sendo produzido.'));

// ---------------------------------------------------------------- início: sessão guardada?
(async () => {
  API = (await ponte.config()).api;
  token = await ponte.lerSessao();
  if (token) {
    try {
      const eu = await chamar('GET', '/eu');
      perfil = eu.visitante ? null : eu;
      if (perfil && perfil.precisa_username) return passo('passo-username');
      return abrirApp();
    } catch {
      token = null;
      await ponte.gravarSessao(null);
    }
  }
  passo('passo-inicio');
})();
