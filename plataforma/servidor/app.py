"""Servidor de contas Legacy: a conta única de todo o ecossistema (launcher, Legacy Engine, jogos).

Como a conta Epic para o Fortnite e a Unreal: nenhum app guarda conta, perfil ou configuração por conta própria — todos
falam com este servidor. Rodar localmente (de plataforma/servidor):

    pip install fastapi uvicorn
    uvicorn app:app --reload

Variáveis de ambiente:
    LEGACY_DB              arquivo SQLite (padrão: legacy.db ao lado deste arquivo)
    LEGACY_PEPPER          segredo extra misturado aos códigos de e-mail (obrigatório em produção)
    LEGACY_EMAIL           "console" (padrão: o código aparece no log, para desenvolvimento) ou "resend"
    RESEND_API_KEY         chave do Resend (quando LEGACY_EMAIL=resend)
    LEGACY_EMAIL_FROM      remetente, ex.: "Legacy <conta@seu-dominio>"

Segurança: senha com scrypt (sal por conta); código de 6 dígitos guardado só como hash, vale 15 min, 5 tentativas;
sessões por token aleatório de 256 bits (guardado como hash), 30 dias; limite de pedidos por IP.
"""
import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import threading
import time
import urllib.request
from contextlib import contextmanager

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

AQUI = os.path.dirname(os.path.abspath(__file__))
DB = os.environ.get('LEGACY_DB', os.path.join(AQUI, 'legacy.db'))
PEPPER = os.environ.get('LEGACY_PEPPER', 'dev-pepper-troque-em-producao')
CODIGO_VALIDADE = 15 * 60
SESSAO_VALIDADE = 30 * 24 * 3600
USERNAME_RE = re.compile(r'^[A-Za-z0-9_]{3,16}$')
EMAIL_RE = re.compile(r'^[^@\s]{1,64}@[^@\s]{1,190}\.[^@\s]{2,}$')

app = FastAPI(title='Legacy', version='0.1.0')
# O launcher (Electron, página local) e o site chamam a API de outra origem; a sessão vai no cabeçalho Authorization,
# não em cookie, então liberar as origens não expõe nada.
app.add_middleware(CORSMiddleware, allow_origins=['*'], allow_methods=['*'], allow_headers=['*'])
_lock = threading.Lock()


# ---------------------------------------------------------------------------------------------------------- banco
SCHEMA = """
CREATE TABLE IF NOT EXISTS contas (
  id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, senha TEXT NOT NULL, username TEXT UNIQUE COLLATE NOCASE,
  verificada INTEGER NOT NULL DEFAULT 0, criada REAL NOT NULL);
CREATE TABLE IF NOT EXISTS codigos (
  email TEXT PRIMARY KEY, hash TEXT NOT NULL, expira REAL NOT NULL, tentativas INTEGER NOT NULL DEFAULT 0,
  enviado REAL NOT NULL);
CREATE TABLE IF NOT EXISTS sessoes (
  token TEXT PRIMARY KEY, conta_id INTEGER, visitante INTEGER NOT NULL DEFAULT 0, criada REAL NOT NULL,
  expira REAL NOT NULL, app TEXT);
CREATE TABLE IF NOT EXISTS configuracoes (conta_id INTEGER PRIMARY KEY, dados TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS trocas (codigo TEXT PRIMARY KEY, conta_id INTEGER, visitante INTEGER NOT NULL,
  expira REAL NOT NULL);
CREATE TABLE IF NOT EXISTS biblioteca (conta_id INTEGER NOT NULL, produto TEXT NOT NULL, adicionado REAL NOT NULL,
  PRIMARY KEY (conta_id, produto));
"""


@contextmanager
def db():
    with _lock:
        con = sqlite3.connect(DB)
        con.row_factory = sqlite3.Row
        try:
            con.executescript(SCHEMA)
            yield con
            con.commit()
        finally:
            con.close()


# ---------------------------------------------------------------------------------------------------------- utilidades
def hash_senha(senha: str) -> str:
    sal = secrets.token_bytes(16)
    h = hashlib.scrypt(senha.encode(), salt=sal, n=2 ** 14, r=8, p=1, dklen=32)
    return f'scrypt$16384$8$1${sal.hex()}${h.hex()}'


def confere_senha(senha: str, guardada: str) -> bool:
    try:
        _, n, r, p, sal, h = guardada.split('$')
        calc = hashlib.scrypt(senha.encode(), salt=bytes.fromhex(sal), n=int(n), r=int(r), p=int(p), dklen=32)
        return hmac.compare_digest(calc.hex(), h)
    except ValueError:
        return False


def hash_token(valor: str) -> str:
    return hmac.new(PEPPER.encode(), valor.encode(), hashlib.sha256).hexdigest()


_pedidos: dict = {}


def limite(request: Request, chave: str, max_por_min: int):
    """Limite simples por IP e rota (contra força bruta e spam de e-mail)."""
    ip = request.client.host if request.client else '?'
    agora = time.time()
    lista = [t for t in _pedidos.get((ip, chave), []) if agora - t < 60]
    if len(lista) >= max_por_min:
        raise HTTPException(429, 'Muitas tentativas. Espere um minuto.')
    lista.append(agora)
    _pedidos[(ip, chave)] = lista


def enviar_email(para: str, assunto: str, texto: str):
    modo = os.environ.get('LEGACY_EMAIL', 'console')
    if modo == 'resend':
        req = urllib.request.Request(
            'https://api.resend.com/emails', method='POST',
            data=json.dumps({'from': os.environ['LEGACY_EMAIL_FROM'], 'to': [para], 'subject': assunto,
                             'text': texto}).encode(),
            headers={'Authorization': 'Bearer ' + os.environ['RESEND_API_KEY'], 'Content-Type': 'application/json'})
        urllib.request.urlopen(req, timeout=15).read()
    else:  # desenvolvimento: o código vai para o log do servidor
        print(f'[e-mail para {para}] {assunto}\n{texto}', flush=True)


def novo_codigo(con, email: str):
    linha = con.execute('SELECT enviado FROM codigos WHERE email=?', (email,)).fetchone()
    if linha and time.time() - linha['enviado'] < 60:
        raise HTTPException(429, 'Já enviamos um código há menos de um minuto.')
    codigo = f'{secrets.randbelow(10 ** 6):06d}'
    con.execute('INSERT OR REPLACE INTO codigos (email, hash, expira, tentativas, enviado) VALUES (?,?,?,0,?)',
                (email, hash_token(email + ':' + codigo), time.time() + CODIGO_VALIDADE, time.time()))
    enviar_email(email, f'Seu código Legacy: {codigo}',
                 f'Use o código {codigo} para confirmar sua conta Legacy. Ele vale por 15 minutos.\n\n'
                 'Se não foi você, ignore este e-mail.')


def nova_sessao(con, conta_id, visitante=False, app_nome=None) -> str:
    token = secrets.token_urlsafe(32)
    con.execute('INSERT INTO sessoes (token, conta_id, visitante, criada, expira, app) VALUES (?,?,?,?,?,?)',
                (hash_token(token), conta_id, int(visitante), time.time(), time.time() + SESSAO_VALIDADE, app_nome))
    return token


def perfil(con, conta_id):
    c = con.execute('SELECT id, email, username, criada FROM contas WHERE id=?', (conta_id,)).fetchone()
    return {'id': c['id'], 'email': c['email'], 'username': c['username'], 'criada': c['criada'],
            'precisa_username': c['username'] is None}


class Sessao(BaseModel):
    conta_id: int | None
    visitante: bool
    token_hash: str


def sessao_atual(authorization: str = Header(default='')) -> Sessao:
    if not authorization.startswith('Bearer '):
        raise HTTPException(401, 'Faça login.')
    th = hash_token(authorization[7:])
    with db() as con:
        s = con.execute('SELECT * FROM sessoes WHERE token=?', (th,)).fetchone()
        if not s or s['expira'] < time.time():
            raise HTTPException(401, 'Sessão expirada. Faça login de novo.')
        return Sessao(conta_id=s['conta_id'], visitante=bool(s['visitante']), token_hash=th)


def conta_logada(s: Sessao = Depends(sessao_atual)) -> Sessao:
    if s.visitante:
        raise HTTPException(403, 'Crie uma conta Legacy para usar isto.')
    return s


# ---------------------------------------------------------------------------------------------------------- rotas
class Cadastro(BaseModel):
    email: str
    senha: str


class Verificacao(BaseModel):
    email: str
    codigo: str
    app: str | None = None


class Login(BaseModel):
    email: str
    senha: str
    app: str | None = None


class Username(BaseModel):
    username: str


@app.post('/conta/cadastro')
def cadastro(dados: Cadastro, request: Request):
    """Passo 1: e-mail e senha. Manda o código de 6 dígitos para o e-mail."""
    limite(request, 'cadastro', 5)
    email = dados.email.strip().lower()
    if not EMAIL_RE.match(email):
        raise HTTPException(400, 'E-mail inválido.')
    if len(dados.senha) < 8:
        raise HTTPException(400, 'A senha precisa ter pelo menos 8 caracteres.')
    with db() as con:
        c = con.execute('SELECT verificada FROM contas WHERE email=?', (email,)).fetchone()
        if c and c['verificada']:
            raise HTTPException(409, 'Já existe uma conta com este e-mail. Faça login.')
        if c:
            con.execute('UPDATE contas SET senha=? WHERE email=?', (hash_senha(dados.senha), email))
        else:
            con.execute('INSERT INTO contas (email, senha, criada) VALUES (?,?,?)', (email, hash_senha(dados.senha), time.time()))
        novo_codigo(con, email)
    return {'ok': True, 'mensagem': 'Enviamos um código para o seu e-mail.'}


@app.post('/conta/reenviar')
def reenviar(dados: Cadastro, request: Request):
    limite(request, 'reenviar', 3)
    email = dados.email.strip().lower()
    with db() as con:
        c = con.execute('SELECT senha, verificada FROM contas WHERE email=?', (email,)).fetchone()
        if not c or c['verificada'] or not confere_senha(dados.senha, c['senha']):
            raise HTTPException(400, 'Não há cadastro pendente com estes dados.')
        novo_codigo(con, email)
    return {'ok': True}


@app.post('/conta/verificar')
def verificar(dados: Verificacao, request: Request):
    """Passo 2: o código do e-mail. Ativa a conta e já devolve a sessão (falta só escolher o username)."""
    limite(request, 'verificar', 10)
    email = dados.email.strip().lower()
    with db() as con:
        k = con.execute('SELECT * FROM codigos WHERE email=?', (email,)).fetchone()
        if not k or k['expira'] < time.time() or k['tentativas'] >= 5:
            raise HTTPException(400, 'Código expirado. Peça um novo.')
        if not hmac.compare_digest(k['hash'], hash_token(email + ':' + dados.codigo.strip())):
            con.execute('UPDATE codigos SET tentativas=tentativas+1 WHERE email=?', (email,))
            raise HTTPException(400, 'Código incorreto.')
        con.execute('DELETE FROM codigos WHERE email=?', (email,))
        con.execute('UPDATE contas SET verificada=1 WHERE email=?', (email,))
        cid = con.execute('SELECT id FROM contas WHERE email=?', (email,)).fetchone()['id']
        return {'token': nova_sessao(con, cid, app_nome=dados.app), 'perfil': perfil(con, cid)}


@app.post('/conta/username')
def escolher_username(dados: Username, s: Sessao = Depends(conta_logada)):
    """Passo 3: o nome público (o mesmo no launcher, no Legacy Engine e em todos os jogos)."""
    if not USERNAME_RE.match(dados.username):
        raise HTTPException(400, 'Use de 3 a 16 letras, números ou _.')
    with db() as con:
        try:
            con.execute('UPDATE contas SET username=? WHERE id=?', (dados.username, s.conta_id))
        except sqlite3.IntegrityError:
            raise HTTPException(409, 'Este username já está em uso.')
        return perfil(con, s.conta_id)


@app.post('/sessao/login')
def login(dados: Login, request: Request):
    limite(request, 'login', 10)
    email = dados.email.strip().lower()
    with db() as con:
        c = con.execute('SELECT * FROM contas WHERE email=?', (email,)).fetchone()
        if not c or not confere_senha(dados.senha, c['senha']):
            raise HTTPException(401, 'E-mail ou senha incorretos.')
        if not c['verificada']:
            raise HTTPException(403, 'Confirme seu e-mail com o código antes de entrar.')
        return {'token': nova_sessao(con, c['id'], app_nome=dados.app), 'perfil': perfil(con, c['id'])}


@app.post('/sessao/visitante')
def visitante(request: Request):
    """Entrar sem conta: pode navegar e baixar jogos gratuitos; o Legacy Engine e o progresso pedem conta."""
    limite(request, 'visitante', 10)
    with db() as con:
        return {'token': nova_sessao(con, None, visitante=True), 'perfil': None}


class Troca(BaseModel):
    codigo: str
    app: str | None = None


@app.post('/sessao/codigo-troca')
def codigo_troca(s: Sessao = Depends(sessao_atual)):
    """O launcher pede um código de uso único (vale 2 minutos) e passa para o jogo ou para o Legacy Engine ao abrir
    (como o "exchange code" da Epic): o app troca o código pela própria sessão, sem senha e sem token na linha de
    comando."""
    codigo = secrets.token_urlsafe(24)
    with db() as con:
        con.execute('DELETE FROM trocas WHERE expira < ?', (time.time(),))
        con.execute('INSERT INTO trocas (codigo, conta_id, visitante, expira) VALUES (?,?,?,?)',
                    (hash_token(codigo), s.conta_id, int(s.visitante), time.time() + 120))
    return {'codigo': codigo, 'expira_em': 120}


@app.post('/sessao/trocar')
def trocar(dados: Troca, request: Request):
    limite(request, 'trocar', 20)
    with db() as con:
        t = con.execute('SELECT * FROM trocas WHERE codigo=?', (hash_token(dados.codigo),)).fetchone()
        if not t or t['expira'] < time.time():
            raise HTTPException(401, 'Código de troca inválido ou expirado. Abra pelo Legacy Launcher.')
        con.execute('DELETE FROM trocas WHERE codigo=?', (hash_token(dados.codigo),))  # uso único
        token = nova_sessao(con, t['conta_id'], visitante=bool(t['visitante']), app_nome=dados.app)
        return {'token': token, 'perfil': perfil(con, t['conta_id']) if t['conta_id'] else None}


@app.post('/sessao/sair')
def sair(s: Sessao = Depends(sessao_atual)):
    with db() as con:
        con.execute('DELETE FROM sessoes WHERE token=?', (s.token_hash,))
    return {'ok': True}


@app.get('/eu')
def eu(s: Sessao = Depends(sessao_atual)):
    if s.visitante:
        return {'visitante': True}
    with db() as con:
        return {'visitante': False, **perfil(con, s.conta_id)}


@app.get('/eu/configuracoes')
def ler_config(s: Sessao = Depends(conta_logada)):
    """Configurações da conta, as mesmas em todos os apps Legacy (idioma, gráficos, controles...)."""
    with db() as con:
        r = con.execute('SELECT dados FROM configuracoes WHERE conta_id=?', (s.conta_id,)).fetchone()
        return json.loads(r['dados']) if r else {}


@app.put('/eu/configuracoes')
def gravar_config(dados: dict, s: Sessao = Depends(conta_logada)):
    texto = json.dumps(dados)
    if len(texto) > 64_000:
        raise HTTPException(413, 'Configurações grandes demais.')
    with db() as con:
        con.execute('INSERT OR REPLACE INTO configuracoes (conta_id, dados) VALUES (?,?)', (s.conta_id, texto))
    return dados


@app.get('/catalogo')
def catalogo():
    with open(os.path.join(AQUI, 'catalogo.json'), encoding='utf-8') as f:
        return json.load(f)


@app.get('/eu/biblioteca')
def biblioteca(s: Sessao = Depends(conta_logada)):
    with db() as con:
        return [dict(r) for r in con.execute('SELECT produto, adicionado FROM biblioteca WHERE conta_id=?', (s.conta_id,))]


@app.post('/eu/biblioteca/{produto}')
def adicionar(produto: str, s: Sessao = Depends(conta_logada)):
    ids = {p['id'] for p in catalogo()['produtos']}
    if produto not in ids:
        raise HTTPException(404, 'Produto não existe.')
    with db() as con:
        con.execute('INSERT OR IGNORE INTO biblioteca (conta_id, produto, adicionado) VALUES (?,?,?)',
                    (s.conta_id, produto, time.time()))
    return {'ok': True}


@app.delete('/conta')
def apagar_conta(s: Sessao = Depends(conta_logada)):
    """LGPD: a pessoa pode apagar a conta e todos os dados dela."""
    with db() as con:
        for t in ('sessoes', 'configuracoes', 'biblioteca'):
            con.execute(f'DELETE FROM {t} WHERE conta_id=?', (s.conta_id,))
        con.execute('DELETE FROM contas WHERE id=?', (s.conta_id,))
    return {'ok': True}
