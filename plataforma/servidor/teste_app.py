"""Testes do servidor de contas (python3 -m pytest teste_app.py, de plataforma/servidor)."""
import os
import re
import tempfile

if not os.environ.get('DATABASE_URL'):
    os.environ['LEGACY_DB'] = os.path.join(tempfile.mkdtemp(), 'teste.db')

from fastapi.testclient import TestClient  # noqa: E402

import app as servidor  # noqa: E402

c = TestClient(servidor.app)
enviados = []
servidor.enviar_email = lambda para, assunto, texto, html=None: enviados.append((para, texto))


def codigo():
    return re.search(r'\b(\d{6})\b', enviados[-1][1]).group(1)


def test_fluxo_completo():
    r = c.post('/conta/cadastro', json={'email': 'Ana@Exemplo.com', 'senha': 'segredo123'})
    assert r.status_code == 200
    assert c.post('/sessao/login', json={'email': 'ana@exemplo.com', 'senha': 'segredo123'}).status_code == 403
    assert c.post('/conta/verificar', json={'email': 'ana@exemplo.com', 'codigo': '000000' if codigo() != '000000' else '111111'}).status_code == 400
    r = c.post('/conta/verificar', json={'email': 'ana@exemplo.com', 'codigo': codigo(), 'app': 'launcher'})
    assert r.status_code == 200, r.text
    tok = r.json()['token']
    assert r.json()['perfil']['precisa_username']
    h = {'Authorization': 'Bearer ' + tok}
    assert c.post('/conta/username', json={'username': 'a'}, headers=h).status_code == 400
    r = c.post('/conta/username', json={'username': 'AnaPlays'}, headers=h)
    assert r.json()['username'] == 'AnaPlays'
    # a mesma conta no Legacy Engine: login com outro app, mesmo perfil e mesmas configurações
    r = c.post('/sessao/login', json={'email': 'ana@exemplo.com', 'senha': 'segredo123', 'app': 'legacy-engine'})
    h2 = {'Authorization': 'Bearer ' + r.json()['token']}
    c.put('/eu/configuracoes', json={'idioma': 'pt', 'qualidade': 'alto'}, headers=h)
    assert c.get('/eu/configuracoes', headers=h2).json() == {'idioma': 'pt', 'qualidade': 'alto'}
    assert c.get('/eu', headers=h2).json()['username'] == 'AnaPlays'
    assert c.post('/eu/biblioteca/times-square', headers=h2).status_code == 200
    assert c.get('/eu/biblioteca', headers=h).json()[0]['produto'] == 'times-square'
    assert c.post('/sessao/login', json={'email': 'ana@exemplo.com', 'senha': 'errada123'}).status_code == 401


def test_username_unico_e_visitante():
    c.post('/conta/cadastro', json={'email': 'bia@exemplo.com', 'senha': 'outrasenha1'})
    tok = c.post('/conta/verificar', json={'email': 'bia@exemplo.com', 'codigo': codigo()}).json()['token']
    r = c.post('/conta/username', json={'username': 'anaplays'}, headers={'Authorization': 'Bearer ' + tok})
    assert r.status_code == 409  # sem diferença de maiúsculas
    v = c.post('/sessao/visitante').json()['token']
    hv = {'Authorization': 'Bearer ' + v}
    assert c.get('/eu', headers=hv).json() == {'visitante': True}
    assert c.get('/eu/configuracoes', headers=hv).status_code == 403
    assert c.get('/catalogo').json()['produtos'][0]['id'] == 'times-square'


def test_sair_e_apagar():
    c.post('/conta/cadastro', json={'email': 'caio@exemplo.com', 'senha': 'caio12345'})
    tok = c.post('/conta/verificar', json={'email': 'caio@exemplo.com', 'codigo': codigo()}).json()['token']
    h = {'Authorization': 'Bearer ' + tok}
    assert c.delete('/conta', headers=h).status_code == 200
    assert c.get('/eu', headers=h).status_code == 401


def test_codigo_de_troca():
    c.post('/conta/cadastro', json={'email': 'duda@exemplo.com', 'senha': 'duda12345'})
    tok = c.post('/conta/verificar', json={'email': 'duda@exemplo.com', 'codigo': codigo()}).json()['token']
    h = {'Authorization': 'Bearer ' + tok}
    c.post('/conta/username', json={'username': 'Duda'}, headers=h)
    cod = c.post('/sessao/codigo-troca', headers=h).json()['codigo']
    r = c.post('/sessao/trocar', json={'codigo': cod, 'app': 'times-square'})
    assert r.status_code == 200 and r.json()['perfil']['username'] == 'Duda'
    assert c.post('/sessao/trocar', json={'codigo': cod}).status_code == 401  # uso único


def test_esqueci_e_trocar_senha():
    c.post('/conta/cadastro', json={'email': 'eva@exemplo.com', 'senha': 'eva123456'})
    tok_antigo = c.post('/conta/verificar', json={'email': 'eva@exemplo.com', 'codigo': codigo()}).json()['token']
    # mesma resposta exista a conta ou não (não revela quem tem conta)
    assert c.post('/conta/esqueci', json={'email': 'ninguem@exemplo.com'}).json()['ok']
    n = len(enviados)
    assert c.post('/conta/esqueci', json={'email': 'eva@exemplo.com'}).json()['ok']
    assert len(enviados) == n + 1
    # o código de cadastro não serve para senha (finalidades separadas)
    r = c.post('/conta/redefinir', json={'email': 'eva@exemplo.com', 'codigo': '999999' if codigo() != '999999' else '888888',
                                          'senha': 'novasenha1'})
    assert r.status_code == 400
    r = c.post('/conta/redefinir', json={'email': 'eva@exemplo.com', 'codigo': codigo(), 'senha': 'novasenha1'})
    assert r.status_code == 200, r.text
    h = {'Authorization': 'Bearer ' + r.json()['token']}
    assert c.get('/eu', headers={'Authorization': 'Bearer ' + tok_antigo}).status_code == 401  # sessões antigas caem
    assert c.post('/sessao/login', json={'email': 'eva@exemplo.com', 'senha': 'eva123456'}).status_code == 401
    assert c.post('/eu/senha', json={'atual': 'errada000', 'nova': 'outra12345'}, headers=h).status_code == 401
    assert c.post('/eu/senha', json={'atual': 'novasenha1', 'nova': 'outra12345'}, headers=h).status_code == 200
    assert c.get('/eu', headers=h).status_code == 200  # a sessão que trocou continua
    assert c.post('/sessao/login', json={'email': 'eva@exemplo.com', 'senha': 'outra12345'}).status_code == 200


def test_cinco_tentativas_bloqueiam_o_codigo():
    servidor._pedidos.clear()
    c.post('/conta/cadastro', json={'email': 'fabio@exemplo.com', 'senha': 'fabio1234'})
    certo = codigo()
    errado = '000000' if certo != '000000' else '111111'
    for _ in range(5):
        assert c.post('/conta/verificar', json={'email': 'fabio@exemplo.com', 'codigo': errado}).status_code == 400
    # depois de 5 erros, nem o código certo vale mais (precisa pedir outro)
    r = c.post('/conta/verificar', json={'email': 'fabio@exemplo.com', 'codigo': certo})
    assert r.status_code == 400 and 'expirado' in r.json()['detail']


def test_saude():
    assert c.get('/saude').json()['ok']
